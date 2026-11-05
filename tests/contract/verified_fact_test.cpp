#include "support/test.hpp"
#include <mira/memory_consolidation.hpp>
#include <mira/task_checkpoint.hpp>
#include <mira/verified_fact.hpp>

#include <atomic>
#include <functional>
#include <stdexcept>

namespace {
using namespace mira;
using namespace std::chrono_literals;

class Store final : public IEventStore {
  public:
    MemoryEventStore data;
    enum Mode {
        Normal,
        Reject,
        Weak,
        WrongId,
        ThrowAppend,
        ThrowRead,
        EmptyMore,
        ModelEvidence
    } mode = Normal;
    Result<AppendReceipt> append(const AppendRequest &request) override {
        if (mode == Reject) {
            Error e;
            e.code = ErrorCode::ResourceExhausted;
            return e;
        }
        if (mode == ThrowAppend)
            throw std::runtime_error("private store failure");
        auto receipt = data.append(request);
        if (receipt && mode == Weak)
            receipt.value().achieved = Durability::Buffered;
        if (receipt && mode == WrongId)
            receipt.value().event_id = EventId::generate();
        return receipt;
    }
    Result<EventPage> read(const EventQuery &query) const override {
        if (mode == ThrowRead)
            throw std::runtime_error("private read failure");
        if (mode == EmptyMore)
            return EventPage{{}, true};
        auto result = data.read(query);
        if (result && mode == ModelEvidence) {
            for (auto &event : result.value().events)
                event.payload.type = "ModelResponseReceived";
        }
        return result;
    }
    Result<std::vector<AppendReceipt>> append_batch(std::span<const AppendRequest> r) override {
        return data.append_batch(r);
    }
    Result<StoreRecoveryReport> recover(const RecoveryOptions &r) override {
        return data.recover(r);
    }
    Result<void> flush(Durability d) override { return data.flush(d); }
};

struct Authority final {
    bool active = true;
    bool withdraw = false;
    bool throwing = false;
    std::atomic<bool> cancelled{false};
    unsigned releases = 0;
};

struct Fixture final {
    RuntimeId runtime = RuntimeId::generate();
    SessionId session = SessionId::generate();
    TaskId task = TaskId::generate();
    MemoryScope scope{MemoryScopeKind::Environment, "fixture-device", "tenant-a"};
    std::shared_ptr<Store> store = std::make_shared<Store>();
    std::shared_ptr<Authority> authority = std::make_shared<Authority>();
    VerifiedFactClaim claim;
    OperationContext context;
    VerifiedFactLeaseAcquirer acquire;
    Fixture() {
        context.session = session;
        context.task = task;
        context.task_epoch = 1;
        context.operation = OperationId::generate();
        context.started_at = Timestamp::now();
        context.deadline = context.started_at.monotonic + 10s;
        context.cancellation_requested = [a = authority] { return a->cancelled.load(); };
        claim.publication_id = EventId::generate();
        claim.task = task;
        claim.task_epoch = 1;
        claim.environment_epoch = 2;
        claim.key = "screen.width";
        claim.value = "640";
        claim.verifier = "host.native.v1";
        claim.evidence_event = EventId::generate();
        auto source = to_json_string(
            JsonValue::Object{{"observation_id", ObservationId::generate().to_string()},
                              {"observed_at", wall_nanos(Timestamp::now().wall)},
                              {"task_epoch", 1},
                              {"environment_epoch", 2},
                              {"width", 640}});
        (void)store->append({claim.evidence_event,
                             runtime,
                             session,
                             task,
                             {"ObservationRecorded", source, EventClass::State}});
        claim.evidence_digest = digest_string(source);
        claim.valid_from = Timestamp::now().wall;
        claim.valid_until = claim.valid_from + 5min;
        class Lease final : public IVerifiedFactLease {
          public:
            explicit Lease(std::shared_ptr<Authority> authority) : a(std::move(authority)) {}
            ~Lease() override { ++a->releases; }
            Result<void> validate(const PublishedTaskFact &fact, const EventEnvelope &,
                                  const OperationContext &) override {
                if (a->throwing)
                    throw std::runtime_error("private verifier failure");
                if (!a->active || fact.claim.value != "640" ||
                    fact.claim.verifier != "host.native.v1") {
                    Error error;
                    error.code = ErrorCode::InvalidObservation;
                    return error;
                }
                if (a->withdraw)
                    a->cancelled = true;
                return {};
            }
            std::shared_ptr<Authority> a;
        };
        acquire = [a = authority](
                      const PublishedTaskFact &, const EventEnvelope &,
                      const OperationContext &) -> Result<std::unique_ptr<IVerifiedFactLease>> {
            return std::unique_ptr<IVerifiedFactLease>(std::make_unique<Lease>(a));
        };
    }
    VerifiedFactPublisher publisher(VerifiedFactLimits limits = {}) {
        return {runtime, session, scope, store, acquire, limits};
    }
    std::vector<EventEnvelope> events() {
        return store->data.read({session, std::nullopt, 32}).value().events;
    }
};

int positive_identity_expiry_and_compatibility() {
    Fixture f;
    auto first = f.publisher().publish(f.claim, f.context);
    MIRA_CHECK(first && f.store->data.size() == 2 && f.authority->releases == 1);
    auto retry = f.publisher().publish(f.claim, f.context);
    MIRA_CHECK(retry && retry.value().session_sequence == first.value().session_sequence);
    MIRA_CHECK(f.store->data.size() == 2);
    auto conflicting = f.claim;
    conflicting.valid_until += 1min;
    auto conflict = f.publisher().publish(conflicting, f.context);
    MIRA_CHECK(!conflict && conflict.error().code == ErrorCode::DataLoss);
    MIRA_CHECK(f.store->data.size() == 2);
    auto events = f.events();
    auto decoded = parse_published_task_fact(events.back());
    MIRA_CHECK(decoded && decoded.value().scope == f.scope && decoded.value().claim.value == "640");
    MIRA_CHECK(decoded.value().valid_at(Timestamp::now().wall));
    // Recovery may regenerate envelope timestamps (BUG-20261106-001). V1
    // proof times come only from its persisted payload, including on replay.
    auto recovered = events.back();
    recovered.timestamp.wall = f.claim.valid_from - 1h;
    MIRA_CHECK(parse_published_task_fact(recovered));
    CheckpointBuilder builder;
    auto built = builder.build(std::nullopt, events, Timestamp::now());
    MIRA_CHECK(built.checkpoint.verified_facts.size() == 1);
    MIRA_CHECK(built.checkpoint.verified_facts.front().provenance.size() == 2);
    MIRA_CHECK(built.checkpoint.schema_version == SchemaVersion(2, 0));
    auto restored = checkpoint_from_json(checkpoint_to_json(built.checkpoint));
    MIRA_CHECK(restored && restored.value().verified_facts == built.checkpoint.verified_facts);
    auto future = Timestamp::now();
    future.wall = f.claim.valid_until;
    MIRA_CHECK(builder.build(restored.value(), {}, future).checkpoint.verified_facts.empty());
    auto bad = checkpoint_to_json(built.checkpoint);
    auto malformed = *bad.find("verified_facts")->as_array();
    malformed.front().set("valid_until", "bad");
    bad.set("verified_facts", JsonValue{std::move(malformed)});
    MIRA_CHECK(!checkpoint_from_json(bad));
    auto old = built.checkpoint;
    old.schema_version = {1, 0};
    MIRA_CHECK(!old.validate());
    old.verified_facts.front().valid_from.reset();
    old.verified_facts.front().valid_until.reset();
    MIRA_CHECK(old.validate() && checkpoint_from_json(checkpoint_to_json(old)));
    auto late = events.back();
    auto terminated = built.checkpoint;
    terminated.terminal_state = TaskState::Cancelled;
    terminated.through_event_sequence = 0;
    MIRA_CHECK(builder.build(terminated, std::span(&late, 1), Timestamp::now())
                   .checkpoint.verified_facts.size() == 1);
    terminated.verified_facts.clear();
    MIRA_CHECK(builder.build(terminated, std::span(&late, 1), Timestamp::now())
                   .checkpoint.verified_facts.empty());
    auto newer = built.checkpoint;
    newer.task_epoch = f.claim.task_epoch + 1;
    newer.through_event_sequence = 0;
    newer.verified_facts.clear();
    MIRA_CHECK(builder.build(newer, std::span(&late, 1), Timestamp::now())
                   .checkpoint.verified_facts.empty());
    return 0;
}

int invalid_claims_never_publish() {
    using Change = std::function<void(Fixture &)>;
    const Change changes[] = {
        [](auto &f) { f.claim.key.clear(); },
        [](auto &f) { f.claim.value.clear(); },
        [](auto &f) { f.claim.value = "641"; },
        [](auto &f) { f.claim.value = "secret=fixture"; },
        [](auto &f) { f.claim.evidence_event = {}; },
        [](auto &f) { f.claim.evidence_digest = {}; },
        [](auto &f) { f.claim.verifier = "host.other"; },
        [](auto &f) { f.claim.task_epoch = 2; },
        [](auto &f) { f.claim.environment_epoch = 3; },
        [](auto &f) { f.context.session = SessionId::generate(); },
        [](auto &f) { f.claim.task = TaskId::generate(); },
        [](auto &f) {
            f.claim.valid_from += 1h;
            f.claim.valid_until += 1h;
        },
        [](auto &f) { f.claim.valid_until = f.claim.valid_from - 1s; },
        [](auto &f) { f.claim.valid_until = f.claim.valid_from + 48h; },
        [](auto &f) { f.authority->active = false; },
        [](auto &f) { f.authority->cancelled = true; },
        [](auto &f) { f.authority->withdraw = true; },
        [](auto &f) { f.context.deadline = Timestamp::now().monotonic - 1s; },
        [](auto &f) { f.store->mode = Store::ModelEvidence; },
        [](auto &f) { f.acquire = {}; },
    };
    for (const auto &change : changes) {
        Fixture f;
        change(f);
        MIRA_CHECK(!f.publisher().publish(f.claim, f.context));
        MIRA_CHECK(f.store->data.size() == 1);
    }
    Fixture limit;
    limit.claim.evidence_event = EventId::generate();
    auto exhausted = limit.publisher({1, 1}).publish(limit.claim, limit.context);
    MIRA_CHECK(!exhausted && exhausted.error().code == ErrorCode::ResourceExhausted);
    return 0;
}

int store_and_verifier_failures_are_visible() {
    for (auto mode : {Store::Reject, Store::Weak, Store::WrongId, Store::ThrowAppend,
                      Store::ThrowRead, Store::EmptyMore}) {
        Fixture f;
        f.store->mode = mode;
        auto result = f.publisher().publish(f.claim, f.context);
        MIRA_CHECK(!result);
        MIRA_CHECK(result.error().safe_message.find("private") == std::string::npos);
        if (mode == Store::Weak || mode == Store::WrongId) {
            MIRA_CHECK(result.error().code == ErrorCode::DataLoss);
            MIRA_CHECK(f.store->data.size() == 2); // Error does not undo append.
        } else
            MIRA_CHECK(f.store->data.size() == 1);
    }
    Fixture f;
    f.authority->throwing = true;
    auto result = f.publisher().publish(f.claim, f.context);
    MIRA_CHECK(!result && result.error().code == ErrorCode::Internal);
    MIRA_CHECK(f.authority->releases == 1 && f.store->data.size() == 1);
    return 0;
}

int memory_scope_expiry_approval_and_replay_boundaries() {
    class Memory final : public IMemory {
      public:
        mutable unsigned queries = 0;
        unsigned writes = 0;
        std::vector<MemoryRecord> existing;
        MemoryMutation last;
        Result<MemoryQueryResult> query(const MemoryQuery &) const override {
            ++queries;
            MemoryQueryResult result;
            result.records = existing;
            return result;
        }
        Result<std::optional<MemoryRecord>> get(MemoryId, const MemoryScope &) const override {
            return std::optional<MemoryRecord>{};
        }
        Result<MemoryMutationResult> apply(const MemoryMutation &mutation) override {
            ++writes;
            last = mutation;
            MemoryMutationResult result;
            result.new_version = 1;
            return result;
        }
        Result<MemoryCompactionResult> compact(const MemoryScope &) override { return Error{}; }
        Result<ErasureResult> erase(const ErasureRequest &) override { return Error{}; }
    } memory;
    Fixture f;
    MIRA_CHECK(f.publisher().publish(f.claim, f.context));
    const auto events = f.events();
    MemoryConsolidator consolidator;
    auto applied = consolidator.consolidate(memory, events, f.scope, Timestamp::now());
    MIRA_CHECK(applied && applied.value().count_of(CandidateDisposition::Applied) == 1);
    MIRA_CHECK(memory.writes == 1 && memory.last.proposed.provenance.size() == 2);
    MIRA_CHECK(memory.last.proposed.expires_at.has_value());
    MIRA_CHECK(memory.last.proposed.recorded_at == f.claim.valid_from);
    const EventPayload unverified[] = {
        {"VerificationResult", R"({"verdict":"Satisfied"})", EventClass::State},
        {"VerificationResult", R"({"verdict":"Invalid"})", EventClass::State},
        {"ModelResponseReceived", R"({"done":true})", EventClass::State},
        {"TaskFactVerified",
         R"({"schema":"mira.task-fact-verified.v2","key":"screen.width","value":"640"})",
         EventClass::Critical},
        {"TaskFactVerified", R"({"schema":true,"key":"screen.width","value":"640"})",
         EventClass::Critical},
    };
    for (const auto &payload : unverified) {
        auto event = events.back();
        event.payload = payload;
        const auto calls = memory.queries;
        auto ignored =
            consolidator.consolidate(memory, std::span(&event, 1), f.scope, Timestamp::now());
        MIRA_CHECK(ignored && ignored.value().candidates_examined == 0 && memory.queries == calls &&
                   memory.writes == 1);
        MIRA_CHECK(CheckpointBuilder{}
                       .build(std::nullopt, std::span(&event, 1), Timestamp::now())
                       .checkpoint.verified_facts.empty());
    }
    memory.existing = {memory.last.proposed};
    memory.existing.front().version = 1;
    auto replay = consolidator.consolidate(memory, events, f.scope, Timestamp::now());
    MIRA_CHECK(replay && replay.value().entries.front().reason_code == "duplicate-noop");
    MIRA_CHECK(memory.writes == 1);
    memory.existing.front().statement = "screen.width=641";
    memory.existing.front().recorded_at += 1s;
    auto stale = consolidator.consolidate(memory, events, f.scope, Timestamp::now());
    MIRA_CHECK(stale && stale.value().count_of(CandidateDisposition::RejectedConflict) == 1);
    MIRA_CHECK(memory.writes == 1);
    auto legacy_conflict = events.back();
    legacy_conflict.payload = {
        "TaskFactVerified",
        to_json_string(JsonValue::Object{{"key", "screen.width"}, {"value", "642"}}),
        EventClass::State};
    auto legacy_rejected =
        consolidator.consolidate(memory, std::span(&legacy_conflict, 1), f.scope, Timestamp::now());
    MIRA_CHECK(legacy_rejected &&
               legacy_rejected.value().entries.front().reason_code == "legacy-over-bounded-fact");
    MIRA_CHECK(memory.writes == 1);
    // A bounded new proof can replace an unbounded legacy row whose envelope
    // time was regenerated on reopen; envelope time must not override proof.
    memory.existing.front().validity.valid_until.reset();
    auto upgraded = consolidator.consolidate(memory, events, f.scope, Timestamp::now());
    MIRA_CHECK(upgraded && upgraded.value().count_of(CandidateDisposition::Applied) == 1);
    MIRA_CHECK(memory.writes == 2);
    auto other = f.scope;
    other.tenant_id = "tenant-b";
    const auto calls = memory.queries;
    auto foreign = consolidator.consolidate(memory, events, other, Timestamp::now());
    MIRA_CHECK(foreign && foreign.value().candidates_examined == 0 && memory.queries == calls);
    auto future = Timestamp::now();
    future.wall = f.claim.valid_until;
    auto expired = consolidator.consolidate(memory, events, f.scope, future);
    MIRA_CHECK(expired && expired.value().candidates_examined == 0 && memory.queries == calls);
    auto malformed = events;
    auto json = parse_json(malformed.back().payload.data).value();
    json.set("scope", JsonValue::Object{{"kind", "Environment"},
                                        {"subject_id", "fixture-device"},
                                        {"tenant_id", 42}});
    malformed.back().payload.data = to_json_string(json);
    MIRA_CHECK(!parse_published_task_fact(malformed.back()));
    Fixture user;
    user.scope = MemoryScope{MemoryScopeKind::User, "user-fixture", "tenant-a"};
    MIRA_CHECK(user.publisher().publish(user.claim, user.context));
    Memory approvals;
    auto pending = consolidator.consolidate(approvals, user.events(), user.scope, Timestamp::now());
    MIRA_CHECK(pending && pending.value().count_of(CandidateDisposition::PendingApproval) == 1);
    MIRA_CHECK(approvals.writes == 0);
    auto legacy = events.back();
    legacy.payload = {"TaskFactVerified",
                      to_json_string(JsonValue::Object{{"key", "legacy"}, {"value", "fixture"}}),
                      EventClass::State};
    auto old = CheckpointBuilder{}.build(std::nullopt, std::span(&legacy, 1), Timestamp::now());
    MIRA_CHECK(old.checkpoint.verified_facts.size() == 1 &&
               !old.checkpoint.verified_facts.front().valid_until);
    return 0;
}
} // namespace

int main() {
    MIRA_CHECK(positive_identity_expiry_and_compatibility() == 0);
    MIRA_CHECK(invalid_claims_never_publish() == 0);
    MIRA_CHECK(store_and_verifier_failures_are_visible() == 0);
    MIRA_CHECK(memory_scope_expiry_approval_and_replay_boundaries() == 0);
    return 0;
}
