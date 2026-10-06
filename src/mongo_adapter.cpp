#include "database_adapter.hpp"

#include "benchforge/metrics.hpp"
#include "mongo_client.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace benchforge::detail {
namespace {

constexpr std::size_t kInsertBatchSize = 256;
constexpr std::uint64_t kTimelineLimit = 20;

std::int64_t AsInt64(std::uint64_t value, const std::string& label) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument(label + " exceeds MongoDB signed integer range");
    }
    return static_cast<std::int64_t>(value);
}

BsonDocument WriteConcern() {
    BsonBuilder concern;
    concern.Int32("w", 1);
    return concern.Finish();
}

BsonDocument EmptyDocument() {
    return BsonBuilder{}.Finish();
}

BsonDocument CommandDocument(const std::string& name, std::int32_t value = 1) {
    BsonBuilder command;
    command.Int32(name, value);
    return command.Finish();
}

BsonDocument Int64Filter(const std::string& field, std::int64_t value) {
    BsonBuilder filter;
    filter.Int64(field, value);
    return filter.Finish();
}

std::string DatabaseForPrefix(const std::string& prefix) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : prefix) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string suffix(16U, '0');
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        suffix[suffix.size() - i - 1U] = digits[hash & 0x0fU];
        hash >>= 4U;
    }
    return "benchforge_run_" + suffix;
}

std::string PlanSummary(const MongoValue& value) {
    std::set<std::string> tokens;
    auto visit = [&](const auto& self, const MongoValue& node) -> void {
        if (node.kind == MongoValue::Kind::Document) {
            for (const auto& [key, child] : node.fields) {
                if ((key == "stage" || key == "indexName") &&
                    child.kind == MongoValue::Kind::String) {
                    tokens.insert(child.text);
                }
                self(self, child);
            }
        } else if (node.kind == MongoValue::Kind::Array) {
            for (const auto& child : node.elements) self(self, child);
        }
    };
    visit(visit, value);
    std::string result;
    for (const auto& token : tokens) {
        if (!result.empty()) result += ",";
        result += token;
    }
    return result.empty() ? "plan details unavailable" : result;
}

class MongoAdapter final : public DatabaseAdapter {
public:
    MongoAdapter(const RunConfig& config, std::string key_prefix)
        : host_(config.database_host),
          port_(config.database_port == 0 ? DefaultDatabasePort("mongo")
                                          : config.database_port),
          database_(DatabaseForPrefix(key_prefix)), client_(host_, port_) {}

    std::string Name() const override { return "mongo"; }
    std::string Version() const override { return version_; }
    std::string StorageConfiguration() const override {
        return "MongoDB database=" + database_ +
            "; collections=users,follows,posts; indexes=unique follows(source,target), "
            "posts(author,createdAt desc,_id desc), posts(hashtags,createdAt desc,_id desc); "
            "writeConcern={w:1}; journaling=server-managed; "
            "timeline=read-time aggregation lookup; queryPlanner timeline={" +
            timeline_plan_ + "}, hashtag={" + hashtag_plan_ + "}";
    }
    std::string Endpoint() const override {
        return host_ + ":" + std::to_string(port_) + "/" + database_;
    }

    void LoadDataset(const RunConfig& config) override {
        CheckCancellation();
        BsonBuilder list;
        list.Int32("listCollections",1).Boolean("nameOnly",true);
        const auto existing = RunCommand(database_,list.Finish());
        const auto* cursor = existing.Find("cursor");
        const auto* batch = cursor ? cursor->Find("firstBatch") : nullptr;
        if (!batch || !batch->Array("namespace collections").empty())
            throw std::runtime_error("run database already exists or collection metadata is unavailable");
        BsonBuilder marker;
        marker.String("create", "__benchforge_owner");
        RunCommand(database_, marker.Finish());
        owns_ = true;
        NamespaceAcquired();
        InsertUsers(config.users);
        InsertFollows(config);
        InsertPosts(config);
        CreateIndexes();
        VerifyCount("users", config.users);
        VerifyCount("follows", config.follows);
        VerifyCount("posts", config.posts);

        const auto build_info = client_.Command("admin", CommandDocument("buildInfo"));
        const auto* version = build_info.Find("version");
        if (version == nullptr) throw std::runtime_error("MongoDB buildInfo omitted version");
        version_ = "MongoDB " + version->String("MongoDB version");
        timeline_plan_ = ExplainPlan(TimelineCommand(0));
        hashtag_plan_ = ExplainPlan(HashtagCommand(0));
    }

    TransportCalibration Calibrate() override {
        constexpr std::uint64_t sample_count = 256;
        LatencyHistogram latency;
        std::uint64_t total_ns = 0;
        const auto ping = CommandDocument("ping");
        for (std::uint64_t sample = 0; sample < sample_count; ++sample) {
            const auto start = std::chrono::steady_clock::now();
            RunCommand(database_, ping);
            const auto end = std::chrono::steady_clock::now();
            const auto elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            latency.Record(elapsed);
            total_ns += elapsed;
        }
        return {"MongoDB ping round-trip", sample_count,
                latency.Percentile(0.50), latency.Percentile(0.95),
                total_ns / sample_count};
    }

    void Execute(const Operation& operation) override {
        switch (operation.type) {
        case OperationType::TimelineRead: {
            const auto reply = RunCommand(database_, TimelineCommand(operation.user_id));
            (void)CursorBatch(reply, "timeline read");
            return;
        }
        case OperationType::UserProfileRead: {
            const auto reply = RunCommand(database_, FindCommand("users",
                Int64Filter("_id", AsInt64(operation.target_user_id, "user id")), 1));
            if (CursorBatch(reply, "profile read").empty()) {
                throw std::runtime_error("requested profile was not present");
            }
            return;
        }
        case OperationType::PostLike: {
            const auto like_values = ArrayInt64({AsInt64(operation.user_id, "user id")});
            BsonBuilder if_null_args;
            if_null_args.String("0", "$likes").Array("1", EmptyDocument());
            BsonBuilder if_null;
            if_null.Array("$ifNull", if_null_args.Finish());
            const auto existing_likes = if_null.Finish();
            BsonBuilder union_args;
            union_args.Document("0", existing_likes).Array("1", like_values);
            BsonBuilder set_union;
            set_union.Array("$setUnion", union_args.Finish());
            const auto unique_likes = set_union.Finish();
            BsonBuilder size_expr;
            size_expr.Document("$size", unique_likes);
            BsonBuilder set_fields;
            set_fields.Document("likes", unique_likes)
                      .Document("likeCount", size_expr.Finish());
            BsonBuilder stage;
            stage.Document("$set", set_fields.Finish());
            const auto pipeline = ArrayDocuments({stage.Finish()});

            BsonBuilder filter;
            filter.Int64("_id", AsInt64(operation.post_id, "post id"));
            BsonBuilder update;
            update.Document("q", filter.Finish()).Array("u", pipeline)
                  .Boolean("multi", false).Boolean("upsert", false);
            BsonBuilder updates;
            updates.Document("0", update.Finish());
            BsonBuilder command;
            command.String("update", "posts").Array("updates", updates.Finish())
                   .Document("writeConcern", WriteConcern());
            const auto reply = RunCommand(database_, command.Finish());
            if (RequiredInteger(reply, "n", "post like update") != 1) {
                throw std::runtime_error("post like referenced a missing post");
            }
            return;
        }
        case OperationType::PostCreate: {
            BsonBuilder post;
            post.Int64("_id", AsInt64(operation.post_id, "post id"))
                .Int64("author", AsInt64(operation.user_id, "author id"))
                .Int64("createdAt", AsInt64(operation.created_at_ms, "post timestamp"))
                .Array("hashtags", ArrayInt64({static_cast<std::int64_t>(operation.hashtag_id)}))
                .Array("likes", EmptyDocument()).Int64("likeCount", 0);
            InsertDocuments("posts", {post.Finish()});
            return;
        }
        case OperationType::HashtagSearch: {
            const auto reply = RunCommand(database_, HashtagCommand(operation.hashtag_id));
            (void)CursorBatch(reply, "hashtag search");
            return;
        }
        case OperationType::UserFollow: {
            BsonBuilder filter;
            filter.Int64("source", AsInt64(operation.user_id, "source user id"))
                  .Int64("target", AsInt64(operation.target_user_id, "target user id"));
            BsonBuilder set_on_insert;
            set_on_insert.Int64("source", AsInt64(operation.user_id, "source user id"))
                         .Int64("target", AsInt64(operation.target_user_id, "target user id"));
            BsonBuilder update_doc;
            update_doc.Document("$setOnInsert", set_on_insert.Finish());
            BsonBuilder update;
            update.Document("q", filter.Finish()).Document("u", update_doc.Finish())
                  .Boolean("multi", false).Boolean("upsert", true);
            BsonBuilder updates;
            updates.Document("0", update.Finish());
            BsonBuilder command;
            command.String("update", "follows").Array("updates", updates.Finish())
                   .Document("writeConcern", WriteConcern());
            RunCommand(database_, command.Finish());
            return;
        }
        case OperationType::Count:
            break;
        }
        throw std::invalid_argument("unsupported workload operation");
    }

    std::string RecoverNamespace() noexcept override { owns_ = true; return Cleanup(); }
    std::string Cleanup() noexcept override {
        if (!owns_) return "no run database owned";
        try {
            MongoClient cleanup(host_, port_);
            cleanup.Command(database_, DropDatabaseCommand());
            owns_ = false;
            return "run database removed";
        } catch (const std::exception& error) {
            return std::string("cleanup failed: ") + error.what();
        } catch (...) {
            return "cleanup failed: unknown MongoDB error";
        }
    }

private:
    bool owns_{false};
    std::string host_;
    std::uint16_t port_;
    std::string database_;
    MongoClient client_;
    std::string version_{"MongoDB version unavailable"};
    std::string timeline_plan_{"not captured"};
    std::string hashtag_plan_{"not captured"};

    MongoValue RunCommand(const std::string& database, const BsonDocument& command) {
        CheckCancellation();
        return client_.Command(database, command);
    }

    static BsonDocument DropDatabaseCommand() {
        BsonBuilder command;
        command.Int32("dropDatabase", 1).Document("writeConcern", WriteConcern());
        return command.Finish();
    }

    void InsertDocuments(const std::string& collection,
                         const std::vector<BsonDocument>& documents) {
        if (documents.empty()) return;
        BsonBuilder command;
        command.String("insert", collection)
               .Array("documents", ArrayDocuments(documents))
               .Boolean("ordered", false)
               .Document("writeConcern", WriteConcern());
        RunCommand(database_, command.Finish());
    }

    void InsertUsers(std::uint64_t users) {
        std::vector<BsonDocument> pending;
        pending.reserve(kInsertBatchSize);
        for (std::uint64_t id = 0; id < users; ++id) {
            BsonBuilder user;
            user.Int64("_id", AsInt64(id, "user id"))
                .String("displayName", "user|" + std::to_string(id));
            pending.push_back(user.Finish());
            if (pending.size() == kInsertBatchSize) {
                InsertDocuments("users", pending);
                pending.clear();
            }
        }
        InsertDocuments("users", pending);
    }

    void InsertFollows(const RunConfig& config) {
        std::vector<BsonDocument> pending;
        pending.reserve(kInsertBatchSize);
        auto add = [&](std::uint64_t source, std::uint64_t target) {
            BsonBuilder follow;
            follow.Int64("source", AsInt64(source, "source user id"))
                  .Int64("target", AsInt64(target, "target user id"));
            pending.push_back(follow.Finish());
            if (pending.size() == kInsertBatchSize) {
                InsertDocuments("follows", pending);
                pending.clear();
            }
        };

        if (config.scenario == Scenario::Celebrity) {
            const auto celebrity_edges = std::min<std::uint64_t>(
                config.follows / 2U, config.users - 1U);
            for (std::uint64_t source = 1; source <= celebrity_edges; ++source) {
                add(source, 0);
            }
            auto remaining = config.follows - celebrity_edges;
            for (std::uint64_t source = 0;
                 source < config.users && remaining != 0; ++source) {
                for (std::uint64_t target = 0;
                     target < config.users && remaining != 0; ++target) {
                    if (target == source || target == 0) continue;
                    add(source, target);
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
                add(source, target);
            }
        }
        InsertDocuments("follows", pending);
    }

    void InsertPosts(const RunConfig& config) {
        std::mt19937_64 random(config.seed);
        std::vector<BsonDocument> pending;
        pending.reserve(kInsertBatchSize);
        for (std::uint64_t id = 0; id < config.posts; ++id) {
            const auto author = random() % config.users;
            const auto hashtag = random() % config.hashtags;
            BsonBuilder post;
            post.Int64("_id", AsInt64(id, "post id"))
                .Int64("author", AsInt64(author, "author id"))
                .Int64("createdAt", AsInt64(id + 1U, "post timestamp"))
                .Array("hashtags", ArrayInt64({AsInt64(hashtag, "hashtag id")}))
                .Array("likes", EmptyDocument()).Int64("likeCount", 0);
            pending.push_back(post.Finish());
            if (pending.size() == kInsertBatchSize) {
                InsertDocuments("posts", pending);
                pending.clear();
            }
        }
        InsertDocuments("posts", pending);
    }

    void CreateIndexes() {
        BsonBuilder follow_keys;
        follow_keys.Int32("source", 1).Int32("target", 1);
        BsonBuilder unique_follow;
        unique_follow.String("name", "source_target_unique")
                    .Document("key", follow_keys.Finish()).Boolean("unique", true);
        BsonBuilder follow_indexes;
        follow_indexes.Document("0", unique_follow.Finish());
        BsonBuilder follows;
        follows.String("createIndexes", "follows")
               .Array("indexes", follow_indexes.Finish())
               .Document("writeConcern", WriteConcern());
        RunCommand(database_, follows.Finish());

        BsonBuilder timeline_keys;
        timeline_keys.Int32("author", 1).Int32("createdAt", -1).Int32("_id", -1);
        BsonBuilder hashtag_keys;
        hashtag_keys.Int32("hashtags", 1).Int32("createdAt", -1).Int32("_id", -1);
        BsonBuilder timeline_index;
        timeline_index.String("name", "author_createdAt_id")
                      .Document("key", timeline_keys.Finish());
        BsonBuilder hashtag_index;
        hashtag_index.String("name", "hashtags_createdAt_id")
                     .Document("key", hashtag_keys.Finish());
        BsonBuilder post_indexes;
        post_indexes.Document("0", timeline_index.Finish())
                    .Document("1", hashtag_index.Finish());
        BsonBuilder posts;
        posts.String("createIndexes", "posts").Array("indexes", post_indexes.Finish())
             .Document("writeConcern", WriteConcern());
        RunCommand(database_, posts.Finish());
    }

    std::int64_t CountCollection(const std::string& collection) {
        BsonBuilder command;
        command.String("count", collection).Document("query", EmptyDocument());
        const auto reply = RunCommand(database_, command.Finish());
        return RequiredInteger(reply, "n", collection + " count");
    }

    void VerifyCount(const std::string& collection, std::uint64_t expected) {
        const auto actual = CountCollection(collection);
        if (actual < 0 || static_cast<std::uint64_t>(actual) != expected) {
            throw std::runtime_error("MongoDB loader count check failed for " + collection);
        }
    }

    static std::int64_t RequiredInteger(const MongoValue& value,
                                        const std::string& field,
                                        const std::string& context) {
        const auto* member = value.Find(field);
        if (member == nullptr) throw std::runtime_error(context + " omitted " + field);
        return member->Integer(context + " " + field);
    }

    static const std::vector<MongoValue>& CursorBatch(const MongoValue& reply,
                                                       const std::string& context) {
        const auto* cursor = reply.Find("cursor");
        if (cursor == nullptr || cursor->kind != MongoValue::Kind::Document) {
            throw std::runtime_error(context + " returned no cursor");
        }
        const auto* batch = cursor->Find("firstBatch");
        if (batch == nullptr) batch = cursor->Find("nextBatch");
        if (batch == nullptr) throw std::runtime_error(context + " returned no result batch");
        return batch->Array(context + " result batch");
    }

    BsonDocument FindCommand(const std::string& collection,
                             const BsonDocument& filter, std::int32_t limit) const {
        BsonBuilder command;
        command.String("find", collection).Document("filter", filter)
               .Int32("limit", limit).Int32("batchSize", limit);
        return command.Finish();
    }

    BsonDocument TimelinePipeline(std::uint64_t user_id) const {
        BsonBuilder source_filter;
        source_filter.Int64("source", AsInt64(user_id, "user id"));
        BsonBuilder match_body;
        match_body.Document("$match", source_filter.Finish());

        BsonBuilder lookup_body;
        lookup_body.String("from", "posts").String("localField", "target")
                   .String("foreignField", "author").String("as", "post");
        BsonBuilder lookup;
        lookup.Document("$lookup", lookup_body.Finish());

        BsonBuilder unwind;
        unwind.String("$unwind", "$post");

        BsonBuilder replace_body;
        replace_body.String("newRoot", "$post");
        BsonBuilder replace;
        replace.Document("$replaceRoot", replace_body.Finish());

        BsonBuilder sort_body;
        sort_body.Int32("createdAt", -1).Int32("_id", -1);
        BsonBuilder sort;
        sort.Document("$sort", sort_body.Finish());

        BsonBuilder limit;
        limit.Int32("$limit", static_cast<std::int32_t>(kTimelineLimit));

        return ArrayDocuments({match_body.Finish(), lookup.Finish(), unwind.Finish(),
            replace.Finish(), sort.Finish(), limit.Finish()});
    }

    BsonDocument TimelineCommand(std::uint64_t user_id) const {
        BsonBuilder cursor;
        cursor.Int32("batchSize", static_cast<std::int32_t>(kTimelineLimit));
        BsonBuilder command;
        command.String("aggregate", "follows")
               .Array("pipeline", TimelinePipeline(user_id))
               .Document("cursor", cursor.Finish());
        return command.Finish();
    }

    BsonDocument HashtagCommand(std::uint32_t hashtag_id) const {
        BsonBuilder filter;
        filter.Int64("hashtags", static_cast<std::int64_t>(hashtag_id));
        BsonBuilder sort;
        sort.Int32("createdAt", -1).Int32("_id", -1);
        BsonBuilder command;
        command.String("find", "posts").Document("filter", filter.Finish())
               .Document("sort", sort.Finish()).Int32("limit", 20)
               .Int32("batchSize", 20);
        return command.Finish();
    }

    std::string ExplainPlan(const BsonDocument& inner_command) {
        BsonBuilder explain;
        explain.Document("explain", inner_command).String("verbosity", "queryPlanner");
        const auto reply = RunCommand(database_, explain.Finish());
        return PlanSummary(reply);
    }
};

} // namespace

std::unique_ptr<DatabaseAdapter> CreateMongoAdapter(const RunConfig& config,
                                                   const std::string& key_prefix) {
    return std::make_unique<MongoAdapter>(config, key_prefix);
}

} // namespace benchforge::detail
