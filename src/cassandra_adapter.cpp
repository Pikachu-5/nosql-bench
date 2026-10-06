#include "database_adapter.hpp"
#include "benchforge/metrics.hpp"
#include "cql_client.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <stdexcept>
#include <utility>

namespace benchforge::detail {
namespace {
std::string KeyspaceForPrefix(const std::string& prefix) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : prefix) { hash ^= byte; hash *= 1099511628211ULL; }
    constexpr char digits[] = "0123456789abcdef";
    std::string suffix(16, '0');
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        suffix[suffix.size() - i - 1U] = digits[hash & 0x0fU]; hash >>= 4U;
    }
    return "benchforge_run_" + suffix;
}
struct PostRef { std::uint64_t id; std::uint64_t timestamp; };
bool NewestFirst(const PostRef& a, const PostRef& b) {
    return a.timestamp != b.timestamp ? a.timestamp > b.timestamp : a.id > b.id;
}

class CassandraAdapter final : public DatabaseAdapter {
public:
    CassandraAdapter(const RunConfig& config, const std::string& prefix)
        : host_(config.database_host),
          port_(config.database_port == 0 ? DefaultDatabasePort("cassandra") : config.database_port),
          keyspace_(KeyspaceForPrefix(prefix)), client_(host_, port_) {}

    std::string Name() const override { return "cassandra"; }
    std::string Version() const override { return version_; }
    std::string Endpoint() const override { return host_ + ":" + std::to_string(port_) + "/" + keyspace_; }
    std::string StorageConfiguration() const override {
        return "Cassandra keyspace=" + keyspace_ +
            "; protocol=v4; prepared statements; loader=seeded-v1; "
            "replication=SimpleStrategy RF=1; read/write consistency=ONE; durable_writes=true; "
            "commitlog sync=server-managed; compaction=SizeTieredCompactionStrategy; "
            "tables=users,posts,posts_by_author,posts_by_tag,follows_by_user,likes_by_post; "
            "clustering=(created_at desc,id desc); post create=3-statement logged batch; "
            "likes=idempotent rows and partition count; timeline=read-time per-author top20 merge; "
            "partition bounds=unbucketed laptop-scale; JVM heap/GC=not observable via CQL; "
            "server " + server_settings_;
    }

    void LoadDataset(const RunConfig& config) override {
        CheckCancellation();
        // Deliberately omit IF NOT EXISTS: a reused run ID must not adopt another run's data.
        client_.Query("CREATE KEYSPACE " + keyspace_ +
            " WITH replication = {'class':'SimpleStrategy','replication_factor':1} AND durable_writes = true");
        owns_keyspace_ = true;
        NamespaceAcquired();
        const std::string compaction = " AND compaction = {'class':'SizeTieredCompactionStrategy'}";
        for (const auto& table : std::vector<std::string>{
            "users (id bigint PRIMARY KEY, display_name text)",
            "posts (id bigint PRIMARY KEY, author bigint, created_at bigint, tag bigint)",
            "follows_by_user (source bigint, target bigint, PRIMARY KEY (source,target))",
            "likes_by_post (post bigint, user_id bigint, PRIMARY KEY (post,user_id))"}) {
            client_.Query("CREATE TABLE " + keyspace_ + "." + table +
                " WITH compaction = {'class':'SizeTieredCompactionStrategy'}");
        }
        for (const auto& [name, partition] : std::vector<std::pair<std::string, std::string>>{
                {"posts_by_author", "author"}, {"posts_by_tag", "tag"}}) {
            client_.Query("CREATE TABLE " + keyspace_ + "." + name + " (" + partition +
                " bigint, created_at bigint, id bigint, PRIMARY KEY (" + partition +
                ",created_at,id)) WITH CLUSTERING ORDER BY (created_at DESC,id DESC)" + compaction);
        }
        PrepareStatements();
        const auto insert_user = client_.Prepare("INSERT INTO " + keyspace_ + ".users (id,display_name) VALUES (?,?)");
        for (std::uint64_t user = 0; user < config.users; ++user) {
            CheckCancellation();
            client_.Execute(insert_user, {CqlBigint(user), "user|" + std::to_string(user)});
        }
        auto add_follow = [&](std::uint64_t source, std::uint64_t target) {
            CheckCancellation();
            client_.Execute(follow_, {CqlBigint(source), CqlBigint(target)});
        };
        if (config.scenario == Scenario::Celebrity) {
            const auto celebrity_edges = std::min(config.follows / 2U, config.users - 1U);
            for (std::uint64_t source = 1; source <= celebrity_edges; ++source) add_follow(source, 0);
            auto remaining = config.follows - celebrity_edges;
            for (std::uint64_t source = 0; source < config.users && remaining != 0; ++source) {
                for (std::uint64_t target = 0; target < config.users && remaining != 0; ++target) {
                    if (source == target || target == 0) continue;
                    add_follow(source, target); --remaining;
                }
            }
            if (remaining != 0) throw std::invalid_argument("celebrity scenario cannot represent the requested follow count");
        } else {
            for (std::uint64_t edge = 0; edge < config.follows; ++edge) {
                const auto source = edge % config.users;
                const auto target = (source + 1U + (edge / config.users) % (config.users - 1U)) % config.users;
                add_follow(source, target);
            }
        }
        std::mt19937_64 random(config.seed);
        for (std::uint64_t id = 0; id < config.posts; ++id) {
            CheckCancellation();
            const auto author = random() % config.users;
            const auto tag = random() % config.hashtags;
            InsertPost(id, author, id + 1U, tag);
        }
        VerifyCount("users", config.users);
        VerifyCount("posts", config.posts);
        VerifyCount("posts_by_author", config.posts);
        VerifyCount("posts_by_tag", config.posts);
        VerifyCount("follows_by_user", config.follows);
        const auto server = client_.Query("SELECT release_version,cluster_name,data_center,partitioner FROM system.local");
        if (server.rows.size() != 1 || server.rows[0].size() != 4 || !server.rows[0][0]) {
            throw std::runtime_error("Cassandra system.local metadata missing");
        }
        version_ = "Cassandra " + *server.rows[0][0];
        server_settings_ = "cluster=" + server.rows[0][1].value_or("unknown") +
            "; data_center=" + server.rows[0][2].value_or("unknown") +
            "; partitioner=" + server.rows[0][3].value_or("unknown");
    }

    TransportCalibration Calibrate() override {
        LatencyHistogram histogram;
        std::uint64_t total = 0;
        constexpr std::uint64_t samples = 256;
        for (std::uint64_t i = 0; i < samples; ++i) {
            CheckCancellation();
            const auto start = std::chrono::steady_clock::now();
            client_.Probe();
            const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
            histogram.Record(elapsed); total += elapsed;
        }
        return {"CQL OPTIONS round-trip", samples, histogram.Percentile(0.5), histogram.Percentile(0.95), total / samples};
    }

    void Execute(const Operation& operation) override {
        switch (operation.type) {
        case OperationType::TimelineRead: {
            std::vector<PostRef> newest;
            std::string state;
            do {
                const auto authors = client_.Execute(following_, {CqlBigint(operation.user_id)}, state);
                for (const auto& row : authors.rows) {
                    if (row.size() != 1) throw std::runtime_error("invalid Cassandra follow row");
                    const auto posts = client_.Execute(author_posts_, {RequiredBigint(row[0])});
                    AddPosts(newest, posts);
                    std::sort(newest.begin(), newest.end(), NewestFirst);
                    if (newest.size() > 20U) newest.resize(20U);
                }
                if (!authors.paging_state.empty() && authors.paging_state == state) {
                    throw std::runtime_error("Cassandra follow paging did not advance");
                }
                state = authors.paging_state;
            } while (!state.empty());
            ReadPosts(newest);
            return;
        }
        case OperationType::UserProfileRead: {
            const auto profile = client_.Execute(profile_, {CqlBigint(operation.target_user_id)});
            if (profile.rows.size() != 1 || profile.rows[0].size() != 1 || !profile.rows[0][0]) {
                throw std::runtime_error("requested profile was not present");
            }
            return;
        }
        case OperationType::PostLike: {
            client_.Execute(like_, {CqlBigint(operation.post_id), CqlBigint(operation.user_id)});
            const auto count = client_.Execute(like_count_, {CqlBigint(operation.post_id)});
            if (count.rows.size() != 1 || count.rows[0].size() != 1 || CqlReadBigint(count.rows[0][0]) < 1) {
                throw std::runtime_error("Cassandra like count missing");
            }
            return;
        }
        case OperationType::PostCreate:
            InsertPost(operation.post_id, operation.user_id, operation.created_at_ms, operation.hashtag_id);
            return;
        case OperationType::HashtagSearch: {
            std::vector<PostRef> posts;
            AddPosts(posts, client_.Execute(tag_posts_, {CqlBigint(operation.hashtag_id)}));
            ReadPosts(posts);
            return;
        }
        case OperationType::UserFollow:
            client_.Execute(follow_, {CqlBigint(operation.user_id), CqlBigint(operation.target_user_id)});
            return;
        case OperationType::Count: break;
        }
        throw std::invalid_argument("unsupported workload operation");
    }

    std::string RecoverNamespace() noexcept override { owns_keyspace_ = true; return Cleanup(); }
    std::string Cleanup() noexcept override {
        if (!owns_keyspace_) return "no run keyspace owned";
        try {
            // Use a fresh connection: a timed-out loader connection may have been closed.
            CqlClient cleanup(host_, port_);
            cleanup.Query("DROP KEYSPACE IF EXISTS " + keyspace_);
            owns_keyspace_ = false;
            return "run keyspace removed";
        } catch (const std::exception& error) { return std::string("cleanup failed: ") + error.what(); }
        catch (...) { return "cleanup failed: unknown Cassandra error"; }
    }

    void PrepareWorker() override { PrepareStatements(); }

private:
    std::string host_;
    std::uint16_t port_;
    std::string keyspace_;
    CqlClient client_;
    bool owns_keyspace_{false};
    bool prepared_{false};
    std::string version_{"Cassandra version unavailable"};
    std::string server_settings_{"not captured"};
    std::string profile_, post_, following_, author_posts_, tag_posts_, follow_, like_, like_count_;
    std::string insert_post_, insert_author_, insert_tag_;

    void PrepareStatements() {
        if (prepared_) return;
        auto prepare = [&](const std::string& query) { return client_.Prepare(query); };
        const auto table = [&](const std::string& name) { return keyspace_ + "." + name; };
        profile_ = prepare("SELECT display_name FROM " + table("users") + " WHERE id=?");
        post_ = prepare("SELECT author,created_at,tag FROM " + table("posts") + " WHERE id=?");
        following_ = prepare("SELECT target FROM " + table("follows_by_user") + " WHERE source=?");
        author_posts_ = prepare("SELECT id,created_at FROM " + table("posts_by_author") + " WHERE author=? LIMIT 20");
        tag_posts_ = prepare("SELECT id,created_at FROM " + table("posts_by_tag") + " WHERE tag=? LIMIT 20");
        follow_ = prepare("INSERT INTO " + table("follows_by_user") + " (source,target) VALUES (?,?)");
        like_ = prepare("INSERT INTO " + table("likes_by_post") + " (post,user_id) VALUES (?,?)");
        like_count_ = prepare("SELECT count(*) FROM " + table("likes_by_post") + " WHERE post=?");
        insert_post_ = prepare("INSERT INTO " + table("posts") + " (id,author,created_at,tag) VALUES (?,?,?,?)");
        insert_author_ = prepare("INSERT INTO " + table("posts_by_author") + " (author,created_at,id) VALUES (?,?,?)");
        insert_tag_ = prepare("INSERT INTO " + table("posts_by_tag") + " (tag,created_at,id) VALUES (?,?,?)");
        prepared_ = true;
    }
    static std::string RequiredBigint(const std::optional<std::string>& value) {
        if (CqlReadBigint(value) < 0) throw std::runtime_error("negative Cassandra ID or timestamp");
        return *value;
    }
    static void AddPosts(std::vector<PostRef>& posts, const CqlResult& result) {
        if (!result.paging_state.empty() || result.rows.size() > 20U) {
            throw std::runtime_error("Cassandra top20 query returned an incomplete or oversized page");
        }
        for (const auto& row : result.rows) {
            if (row.size() != 2) throw std::runtime_error("invalid Cassandra post index row");
            (void)RequiredBigint(row[0]); (void)RequiredBigint(row[1]);
            posts.push_back({static_cast<std::uint64_t>(CqlReadBigint(row[0])),
                             static_cast<std::uint64_t>(CqlReadBigint(row[1]))});
        }
    }
    void ReadPosts(const std::vector<PostRef>& posts) {
        for (const auto& ref : posts) {
            const auto result = client_.Execute(post_, {CqlBigint(ref.id)});
            if (result.rows.size() != 1 || result.rows[0].size() != 3) {
                throw std::runtime_error("indexed Cassandra post was not present");
            }
            for (const auto& value : result.rows[0]) (void)RequiredBigint(value);
        }
    }
    void InsertPost(std::uint64_t id, std::uint64_t author, std::uint64_t timestamp, std::uint64_t tag) {
        const auto post_id = CqlBigint(id), author_id = CqlBigint(author);
        const auto created = CqlBigint(timestamp), tag_id = CqlBigint(tag);
        client_.Batch({{insert_post_, {post_id, author_id, created, tag_id}},
                       {insert_author_, {author_id, created, post_id}},
                       {insert_tag_, {tag_id, created, post_id}}});
    }
    void VerifyCount(const std::string& table, std::uint64_t expected) {
        const auto result = client_.Query("SELECT count(*) FROM " + keyspace_ + "." + table);
        if (result.rows.size() != 1 || result.rows[0].size() != 1 ||
            CqlReadBigint(result.rows[0][0]) != static_cast<std::int64_t>(expected)) {
            throw std::runtime_error("Cassandra loader count check failed for " + table);
        }
    }
};
} // namespace

std::unique_ptr<DatabaseAdapter> CreateCassandraAdapter(const RunConfig& config, const std::string& prefix) {
    return std::make_unique<CassandraAdapter>(config, prefix);
}
} // namespace benchforge::detail
