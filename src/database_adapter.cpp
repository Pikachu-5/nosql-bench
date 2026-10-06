#include "database_adapter.hpp"

#include "benchforge/operation.hpp"
#include "benchforge/metrics.hpp"
#include "resp_client.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace benchforge::detail {
namespace {

constexpr std::size_t kPipelineSize = 128;
constexpr std::uint64_t kTimelineLimit = 20;

std::string Number(std::uint64_t value) {
    return std::to_string(value);
}

std::string InfoField(const std::string& info, const std::string& name) {
    const auto prefix = name + ":";
    std::size_t start = 0;
    while (start < info.size()) {
        const auto end = info.find('\n', start);
        auto line = info.substr(start, end == std::string::npos
                                          ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
        if (end == std::string::npos) break;
        start = end + 1U;
    }
    return {};
}

struct PostRef {
    std::uint64_t id;
    std::uint64_t timestamp;
};

class NetworkAdapter final : public DatabaseAdapter {
public:
    NetworkAdapter(std::string name, const RunConfig& config,
                   std::string key_prefix)
        : name_(std::move(name)), host_(config.database_host),
          port_(config.database_port == 0 ? DefaultDatabasePort(name_)
                                          : config.database_port),
          prefix_(std::move(key_prefix)), client_(host_, port_) {
        const auto ping = client_.Command({"PING"});
        if (ping.kind != RespValue::Kind::Simple || ping.text != "PONG") {
            throw std::runtime_error("database endpoint did not answer PING");
        }
        if (name_ == "feedkv") {
            version_ = "BenchForge FeedKV RESP2 subset 0.1";
            storage_configuration_ =
                "volatile in-memory; no persistence; one process-wide store";
        } else {
            DiscoverRedisMetadata();
        }
    }

    std::string Name() const override { return name_; }
    std::string Version() const override { return version_; }
    std::string StorageConfiguration() const override {
        return storage_configuration_;
    }
    std::string Endpoint() const override {
        return host_ + ":" + std::to_string(port_);
    }

    void LoadDataset(const RunConfig& config) override {
        ClearNamespace();
        const auto cleanup_probe = prefix_ + "__cleanup_probe__";
        (void)RequireText(client_.Command({"SET", cleanup_probe, "probe"}),
                          "namespace cleanup permission check");
        (void)RequireInteger(client_.Command({"DEL", cleanup_probe}),
                             "namespace cleanup permission check");
        std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> following;
        following.reserve(static_cast<std::size_t>(
            std::min<std::uint64_t>(config.users, config.follows)));

        std::vector<RespCommand> pending;
        pending.reserve(kPipelineSize);
        std::uint64_t first_follow_source = 0;
        std::uint64_t first_follow_target = 0;
        auto push = [&](RespCommand command) {
            pending.push_back(std::move(command));
            if (pending.size() >= kPipelineSize) Flush(pending);
        };

        for (std::uint64_t user = 0; user < config.users; ++user) {
            push({"SET", Key("profile", user), "user|" + Number(user)});
        }

        auto add_follow = [&](std::uint64_t source, std::uint64_t target) {
            if (following.empty()) {
                first_follow_source = source;
                first_follow_target = target;
            }
            following[source].push_back(target);
            push({"SADD", Key("following", source), Number(target)});
            push({"SADD", Key("followers", target), Number(source)});
        };

        if (config.scenario == Scenario::Celebrity) {
            const auto celebrity_edges = std::min<std::uint64_t>(
                config.follows / 2U, config.users - 1U);
            for (std::uint64_t source = 1; source <= celebrity_edges; ++source) {
                add_follow(source, 0);
            }
            auto remaining = config.follows - celebrity_edges;
            for (std::uint64_t source = 0;
                 source < config.users && remaining != 0; ++source) {
                for (std::uint64_t target = 0;
                     target < config.users && remaining != 0; ++target) {
                    if (target == source || target == 0) continue;
                    add_follow(source, target);
                    --remaining;
                }
            }
            if (remaining != 0) {
                throw std::invalid_argument(
                    "celebrity scenario cannot represent the requested follow count");
            }
        } else {
            for (std::uint64_t edge = 0; edge < config.follows; ++edge) {
                const auto source = edge % config.users;
                const auto cycle = edge / config.users;
                const auto target = (source + 1U + cycle % (config.users - 1U)) %
                                    config.users;
                add_follow(source, target);
            }
        }

        std::mt19937_64 random(config.seed);
        std::unordered_map<std::uint64_t, std::vector<PostRef>> posts_by_author;
        posts_by_author.reserve(static_cast<std::size_t>(
            std::min(config.users, config.posts)));
        std::uint64_t post_zero_tag = 0;
        for (std::uint64_t post = 0; post < config.posts; ++post) {
            const auto author = random() % config.users;
            const auto tag = random() % config.hashtags;
            if (post == 0) {
                post_zero_tag = tag;
            }
            const auto timestamp = post + 1U;
            posts_by_author[author].push_back({post, timestamp});
            push({"SET", Key("post", post),
                  Number(author) + "|" + Number(timestamp) + "|" + Number(tag)});
            push({"ZADD", Key("tag", tag), Number(timestamp), Number(post)});
            push({"ZADD", Key("author_posts", author), Number(timestamp),
                  Number(post)});
        }

        for (std::uint64_t reader = 0; reader < config.users; ++reader) {
            const auto found = following.find(reader);
            if (found == following.end()) continue;
            std::vector<PostRef> candidates;
            for (const auto author : found->second) {
                const auto authored = posts_by_author.find(author);
                if (authored == posts_by_author.end()) continue;
                const auto& author_posts = authored->second;
                const auto count = std::min<std::size_t>(
                    author_posts.size(), static_cast<std::size_t>(kTimelineLimit));
                candidates.insert(candidates.end(), author_posts.end() - count,
                                  author_posts.end());
            }
            const auto newest_first = [](const PostRef& left, const PostRef& right) {
                if (left.timestamp != right.timestamp) {
                    return left.timestamp > right.timestamp;
                }
                return left.id > right.id;
            };
            std::sort(candidates.begin(), candidates.end(), newest_first);
            const auto count = std::min<std::size_t>(
                candidates.size(), static_cast<std::size_t>(kTimelineLimit));
            for (std::size_t i = 0; i < count; ++i) {
                const auto& post = candidates[i];
                push({"ZADD", Key("feed", reader), Number(post.timestamp),
                      Number(post.id)});
            }
        }
        Flush(pending);

        const auto profile = client_.Command({"GET", Key("profile", 0)});
        if (RequireText(profile, "profile loader check") != "user|0") {
            throw std::runtime_error("database profile loader check failed");
        }
        const auto stored_posts = client_.Command({"GET", Key("post", 0)});
        if (config.posts != 0 && stored_posts.kind == RespValue::Kind::Null) {
            throw std::runtime_error("database post loader check failed");
        }
        const auto indexed_post = client_.Command(
            {"ZSCORE", Key("tag", post_zero_tag), "0"});
        if (indexed_post.kind == RespValue::Kind::Null) {
            throw std::runtime_error("database hashtag index loader check failed");
        }
        if (config.follows != 0) {
            const auto forward = client_.Command(
                {"SISMEMBER", Key("following", first_follow_source),
                 Number(first_follow_target)});
            const auto reverse = client_.Command(
                {"SISMEMBER", Key("followers", first_follow_target),
                 Number(first_follow_source)});
            if (RequireInteger(forward, "follow loader check") != 1 ||
                RequireInteger(reverse, "reverse follow loader check") != 1) {
                throw std::runtime_error("database follow loader check failed");
            }
        }
        bool checked_timeline = false;
        for (const auto& [reader, authors] : following) {
            for (const auto author : authors) {
                const auto authored = posts_by_author.find(author);
                if (authored == posts_by_author.end() || authored->second.empty()) continue;
                const auto timeline = client_.Command(
                    {"ZREVRANGE", Key("feed", reader), "0", "19"});
                RequireArray(timeline, "timeline loader check");
                if (timeline.elements.empty()) {
                    throw std::runtime_error("database timeline loader check failed");
                }
                ReadIndexedPosts(timeline, "timeline loader check");
                checked_timeline = true;
                break;
            }
            if (checked_timeline) break;
        }
    }

    TransportCalibration Calibrate() override {
        constexpr std::uint64_t sample_count = 256;
        LatencyHistogram latency;
        std::uint64_t total_ns = 0;
        for (std::uint64_t sample = 0; sample < sample_count; ++sample) {
            const auto start = std::chrono::steady_clock::now();
            const auto response = client_.Command({"PING"});
            const auto end = std::chrono::steady_clock::now();
            if (response.kind != RespValue::Kind::Simple || response.text != "PONG") {
                throw std::runtime_error("transport calibration received an invalid PING response");
            }
            const auto elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            latency.Record(elapsed);
            total_ns += elapsed;
        }
        return {"RESP PING round-trip", sample_count,
                latency.Percentile(0.50), latency.Percentile(0.95),
                total_ns / sample_count};
    }

    void Execute(const Operation& operation) override {
        switch (operation.type) {
        case OperationType::TimelineRead: {
            const auto value = client_.Command(
                {"ZREVRANGE", Key("feed", operation.user_id), "0", "19"});
            RequireArray(value, "timeline read");
            ReadIndexedPosts(value, "timeline read");
            return;
        }
        case OperationType::UserProfileRead: {
            const auto value = client_.Command(
                {"GET", Key("profile", operation.target_user_id)});
            if (value.kind == RespValue::Kind::Null) {
                throw std::runtime_error("requested profile was not present");
            }
            (void)RequireText(value, "profile read");
            return;
        }
        case OperationType::PostLike: {
            const auto responses = client_.Pipeline({
                {"SADD", Key("likes", operation.post_id), Number(operation.user_id)},
                {"SCARD", Key("likes", operation.post_id)}
            });
            if (responses.size() != 2U) throw std::runtime_error("post like failed");
            (void)RequireInteger(responses[0], "post like update");
            (void)RequireInteger(responses[1], "post like count");
            return;
        }
        case OperationType::PostCreate: {
            const auto post_id = Number(operation.post_id);
            client_.Pipeline({
                {"SET", Key("post", operation.post_id),
                 Number(operation.user_id) + "|" +
                     Number(operation.created_at_ms) + "|" +
                     Number(operation.hashtag_id)},
                {"ZADD", Key("tag", operation.hashtag_id),
                 Number(operation.created_at_ms), post_id},
                {"ZADD", Key("author_posts", operation.user_id),
                 Number(operation.created_at_ms), post_id}
            });
            const auto followers = client_.Command(
                {"SMEMBERS", Key("followers", operation.user_id)});
            RequireArray(followers, "post fan-out");
            std::vector<RespCommand> pending;
            pending.reserve(kPipelineSize);
            for (const auto& follower : followers.elements) {
                const auto follower_id = RequireText(follower, "follower id");
                const auto feed_key = prefix_ + "feed:" + follower_id;
                pending.push_back({"ZADD", feed_key,
                                   Number(operation.created_at_ms), post_id});
                pending.push_back({"ZREMRANGEBYRANK", feed_key, "0", "-21"});
                if (pending.size() >= kPipelineSize) Flush(pending);
            }
            Flush(pending);
            return;
        }
        case OperationType::HashtagSearch: {
            const auto value = client_.Command(
                {"ZREVRANGE", Key("tag", operation.hashtag_id), "0", "19"});
            RequireArray(value, "hashtag search");
            ReadIndexedPosts(value, "hashtag search");
            return;
        }
        case OperationType::UserFollow: {
            (void)RequireInteger(client_.Command(
                {"SADD", Key("following", operation.user_id),
                 Number(operation.target_user_id)}), "following update");
            (void)RequireInteger(client_.Command(
                {"SADD", Key("followers", operation.target_user_id),
                 Number(operation.user_id)}), "follower update");
            const auto author_posts = client_.Command(
                {"ZREVRANGE", Key("author_posts", operation.target_user_id),
                 "0", "19", "WITHSCORES"});
            RequireArray(author_posts, "follow timeline backfill");
            if (author_posts.elements.size() % 2U != 0) {
                throw std::runtime_error("author post index returned an invalid WITHSCORES response");
            }
            std::vector<RespCommand> pending;
            pending.reserve(kPipelineSize);
            const auto feed_key = Key("feed", operation.user_id);
            for (std::size_t i = 0; i < author_posts.elements.size(); i += 2U) {
                const auto post_id = RequireText(author_posts.elements[i], "follow post id");
                const auto score = RequireText(author_posts.elements[i + 1U], "follow post score");
                pending.push_back({"ZADD", feed_key, score, post_id});
                pending.push_back({"ZREMRANGEBYRANK", feed_key, "0", "-21"});
                if (pending.size() >= kPipelineSize) Flush(pending);
            }
            Flush(pending);
            return;
        }
        case OperationType::Count:
            break;
        }
        throw std::invalid_argument("unsupported workload operation");
    }

    std::string Cleanup() noexcept override {
        try {
            ClearNamespace();
            return "run namespace removed";
        } catch (const std::exception& error) {
            return std::string("cleanup failed: ") + error.what();
        } catch (...) {
            return "cleanup failed: unknown database error";
        }
    }

private:
    std::string name_;
    std::string host_;
    std::uint16_t port_;
    std::string prefix_;
    RespClient client_;
    std::string version_;
    std::string storage_configuration_;

    std::string Key(const std::string& kind, std::uint64_t id) const {
        return prefix_ + kind + ":" + Number(id);
    }

    void Flush(std::vector<RespCommand>& commands) {
        if (commands.empty()) return;
        client_.Pipeline(commands);
        commands.clear();
    }

    void ClearNamespace() {
        std::vector<std::string> keys;
        std::string cursor = "0";
        do {
            const auto page = client_.Command(
                {"SCAN", cursor, "MATCH", prefix_ + "*", "COUNT", "256"});
            RequireArray(page, "run cleanup scan");
            if (page.elements.size() != 2U ||
                page.elements[1].kind != RespValue::Kind::Array) {
                throw std::runtime_error("database returned an invalid SCAN response");
            }
            cursor = RequireText(page.elements[0], "SCAN cursor");
            for (const auto& key : page.elements[1].elements) {
                keys.push_back(RequireText(key, "SCAN key"));
            }
        } while (cursor != "0");

        std::vector<RespCommand> commands;
        commands.reserve(64);
        for (std::size_t offset = 0; offset < keys.size(); offset += 100U) {
            RespCommand command{"DEL"};
            const auto end = std::min(keys.size(), offset + 100U);
            command.insert(command.end(), keys.begin() + offset,
                           keys.begin() + end);
            commands.push_back(std::move(command));
            if (commands.size() == 64U) Flush(commands);
        }
        Flush(commands);
    }

    void ReadIndexedPosts(const RespValue& ids, const std::string& context) {
        std::vector<RespCommand> commands;
        commands.reserve(ids.elements.size());
        for (const auto& id : ids.elements) {
            commands.push_back({"GET", prefix_ + "post:" +
                                        RequireText(id, context + " post id")});
        }
        const auto posts = client_.Pipeline(commands);
        for (const auto& post : posts) {
            if (post.kind == RespValue::Kind::Null) {
                throw std::runtime_error(context + " referenced a missing post");
            }
            (void)RequireText(post, context + " post");
        }
    }

    void DiscoverRedisMetadata() {
        version_ = "Redis (version unavailable)";
        storage_configuration_ =
            "Redis-managed persistence; INFO metadata unavailable";
        try {
            const auto server = client_.Command({"INFO", "server"});
            const auto server_info = RequireText(server, "Redis INFO server");
            const auto version = InfoField(server_info, "redis_version");
            if (!version.empty()) version_ = "Redis " + version;
            const auto persistence = client_.Command({"INFO", "persistence"});
            const auto persistence_info =
                RequireText(persistence, "Redis INFO persistence");
            const auto append_only = InfoField(persistence_info, "aof_enabled");
            const auto loading = InfoField(persistence_info, "loading");
            const auto save_time = InfoField(persistence_info, "rdb_last_save_time");
            storage_configuration_ = "Redis-managed persistence";
            if (!append_only.empty()) storage_configuration_ += "; aof_enabled=" + append_only;
            if (!loading.empty()) storage_configuration_ += "; loading=" + loading;
            if (!save_time.empty()) {
                storage_configuration_ += "; rdb_last_save_time=" + save_time;
            }
        } catch (...) {
            // Restricted Redis ACLs may deny INFO. Keep the limitation explicit.
        }
        for (const auto& setting : {std::string("save"), std::string("appendonly")}) {
            try {
                const auto response = client_.Command({"CONFIG", "GET", setting});
                if (response.kind != RespValue::Kind::Array ||
                    response.elements.size() != 2U) {
                    storage_configuration_ += "; CONFIG GET " + setting + " unavailable";
                    continue;
                }
                const auto value = RequireText(response.elements[1],
                                               "Redis CONFIG GET " + setting);
                storage_configuration_ += "; " + setting + "=" + value;
            } catch (...) {
                storage_configuration_ += "; " + setting + " config unavailable";
            }
        }
    }
};

} // namespace

std::unique_ptr<DatabaseAdapter> CreateNetworkAdapter(
    const std::string& name, const RunConfig& config,
    const std::string& key_prefix) {
    return std::make_unique<NetworkAdapter>(name, config, key_prefix);
}

} // namespace benchforge::detail
