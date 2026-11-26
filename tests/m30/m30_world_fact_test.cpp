#include "support/m36_support.hpp"
#include "support/test.hpp"

#include <mira/verified_fact.hpp>
#include <mira/world_fact.hpp>
#include <mira/world_fact_recording.hpp>

namespace {
using namespace mira;
using namespace mira::testing;

Result<ConsolidationReport> write(MemoryAgentFixture &fixture, const MemoryRecord &record,
                                  bool model_assisted = false) {
    return fixture.executor
        .submit_auto([&] {
            MemoryCandidate candidate;
            candidate.proposed = record;
            candidate.evidence = record.provenance;
            candidate.model_assisted = model_assisted;
            return MemoryConsolidator{}.consolidate_candidates(*fixture.memory, {candidate},
                                                               record.scope, Timestamp::now());
        })
        .get();
}

bool reads(SqliteMemoryStore &memory, const WorldFactQuery &query, const MemoryRecord &record) {
    const auto lookup = memory.lookup_world_fact(query);
    return lookup && lookup.value().state == WorldFactState::Current && lookup.value().record &&
           memory_record_to_json(*lookup.value().record) == memory_record_to_json(record);
}

class NativeObservationLease final : public IVerifiedFactLease {
  public:
    Result<void> validate(const PublishedTaskFact &fact, const EventEnvelope &source,
                          const OperationContext &) override {
        const auto body = parse_json(source.payload.data);
        const auto *scale = body ? body.value().find("display_scale") : nullptr;
        if (!scale || !scale->is_string() || *scale->as_string() != fact.claim.value) {
            Error error;
            error.code = ErrorCode::InvalidObservation;
            return error;
        }
        return {};
    }
};

struct WorldRecordingAuthority final {
    bool allowed = true;
    bool cancel_on_validation = false;
    bool cancelled = false;
    unsigned entered = 0;
    unsigned released = 0;
};

class WorldRecordingLease final : public IWorldFactRecordingLease {
  public:
    explicit WorldRecordingLease(WorldRecordingAuthority &authority) : authority_(authority) {
        ++authority_.entered;
    }
    ~WorldRecordingLease() override { ++authority_.released; }

    Result<void> validate() override {
        if (!authority_.allowed)
            return make_memory_error(MemoryDomainCode::ScopeDenied, "fixture withdrew access");
        authority_.cancelled = authority_.cancel_on_validation;
        return {};
    }

  private:
    WorldRecordingAuthority &authority_;
};

int records_committed_verification() {
    MemoryAgentFixture fixture;
    const auto work = fixture.binding();
    const auto tenant = fixture.principal.tenant_id;
    const auto environment = Id128::generate();
    const WorldFactRecordingBinding binding{
        fixture.runtime,
        {MemoryScopeKind::Environment, "test-display", tenant.to_string()},
        "screen.display_scale",
        world_environment_scope(tenant, environment).value(),
        {Id128::generate(), "display_scale"}};

    auto context = make_control_context();
    context.session = work.work.session_id;
    context.task = work.work.task_id;
    context.task_epoch = work.work.task_epoch;
    VerifiedFactClaim claim;
    claim.publication_id = EventId::generate();
    claim.task = context.task;
    claim.task_epoch = context.task_epoch;
    claim.environment_epoch = work.environment_epoch;
    claim.key = binding.verified_key;
    claim.value = "125%";
    claim.verifier = "fixture.display";
    claim.evidence_event = EventId::generate();
    const auto observation = canonical_json_string(
        JsonValue::Object{{"observation_id", ObservationId::generate().to_string()},
                          {"observed_at", wall_nanos(Timestamp::now())},
                          {"task_epoch", static_cast<std::int64_t>(claim.task_epoch)},
                          {"environment_epoch", static_cast<std::int64_t>(claim.environment_epoch)},
                          {"display_scale", claim.value}});
    const auto observed = fixture.events->append({claim.evidence_event,
                                                  fixture.runtime,
                                                  context.session,
                                                  context.task,
                                                  {"ObservationRecorded", observation}});
    MIRA_CHECK(observed);
    claim.evidence_digest = digest_string(observation);
    claim.valid_from = Timestamp::now().wall;
    claim.valid_until = claim.valid_from + 1h;
    const auto publish =
        [](const PublishedTaskFact &, const EventEnvelope &,
           const OperationContext &) -> Result<std::unique_ptr<IVerifiedFactLease>> {
        return std::unique_ptr<IVerifiedFactLease>{std::make_unique<NativeObservationLease>()};
    };
    const auto published =
        fixture.executor
            .submit_auto([&] {
                return VerifiedFactPublisher{fixture.runtime, context.session,
                                             binding.verified_scope, fixture.events, publish}
                    .publish(claim, context);
            })
            .get();
    MIRA_CHECK(published);
    const WorldFactSource source{published.value().session_sequence, published.value().event_id};

    WorldRecordingAuthority authority;
    context.cancellation_requested = [&] { return authority.cancelled; };
    const WorldFactRecordingLeaseAcquirer acquire =
        [&](const WorldFactRecordingBinding &, const WorldFactSource &,
            const OperationContext &) -> Result<std::unique_ptr<IWorldFactRecordingLease>> {
        return std::unique_ptr<IWorldFactRecordingLease>{
            std::make_unique<WorldRecordingLease>(authority)};
    };
    const auto record = [&](const WorldFactRecordingBinding &target, const WorldFactSource &event) {
        return fixture.executor
            .submit_auto([&] {
                return record_verified_world_fact(*fixture.memory, *fixture.events, target, event,
                                                  context, acquire);
            })
            .get();
    };

    auto foreign = binding;
    foreign.world_scope = world_environment_scope(TenantId::generate(), environment).value();
    MIRA_CHECK(!record(foreign, source) && authority.entered == 0);
    auto wrong_key = binding;
    wrong_key.verified_key = "screen.rotation";
    MIRA_CHECK(!record(wrong_key, source));
    auto wrong_scope = binding;
    wrong_scope.verified_scope.subject_id = "unrelated-display";
    MIRA_CHECK(!record(wrong_scope, source));
    wrong_scope.verified_scope.kind = MemoryScopeKind::User;
    MIRA_CHECK(!record(wrong_scope, source));
    MIRA_CHECK(!record(binding, {source.sequence, EventId::generate()}));
    MIRA_CHECK(!record(binding, {observed.value().session_sequence, claim.evidence_event}));
    ++context.task_epoch;
    MIRA_CHECK(!record(binding, source));
    --context.task_epoch;
    context.deadline = context.started_at.monotonic;
    auto expired = record(binding, source);
    MIRA_CHECK(!expired && expired.error().code == ErrorCode::DeadlineExceeded);
    context.deadline.reset();

    authority.cancel_on_validation = true;
    const auto stopped = record(binding, source);
    MIRA_CHECK(!stopped && stopped.error().code == ErrorCode::Cancelled);
    authority.cancel_on_validation = false;
    authority.cancelled = false;
    const WorldFactQuery query{binding.world_scope, binding.property, {}, {}};
    MIRA_CHECK(fixture.memory->lookup_world_fact(query).value().state == WorldFactState::Missing);

    const auto applied = record(binding, source);
    MIRA_CHECK(applied && applied.value().count_of(CandidateDisposition::Applied) == 1 &&
               !applied.value().entries.front().idempotent_replay);
    const auto current = fixture.memory->lookup_world_fact(query);
    MIRA_CHECK(current && current.value().record &&
               current.value().record->statement == claim.value &&
               current.value().record->kind == MemoryKind::EnvironmentFact &&
               current.value().record->verification == MemoryVerification::Observed &&
               current.value().record->provenance == std::vector<EventId>{claim.publication_id});

    fixture.events = std::make_shared<FileEventStore>(fixture.root / "events");
    MIRA_CHECK(fixture.memory->close());
    SqliteMemoryStoreOptions options;
    options.path = fixture.root / "memory.db";
    auto reopened = SqliteMemoryStore::open(fixture.executor, options);
    MIRA_CHECK(reopened);
    fixture.memory = std::move(reopened).value();
    const auto replay = record(binding, source);
    MIRA_CHECK(replay && replay.value().entries.front().idempotent_replay &&
               reads(*fixture.memory, query, *current.value().record));

    authority.allowed = false;
    ErasureRequest erase;
    erase.scope = binding.world_scope;
    erase.reason = "test source retired";
    const auto erased = fixture.memory->erase(erase);
    MIRA_CHECK(erased && erased.value().status == ErasureStatus::Complete);
    MIRA_CHECK(!record(binding, source) && authority.entered == authority.released);
    MIRA_CHECK(fixture.memory->lookup_world_fact(query).value().state == WorldFactState::Missing);
    return 0;
}

int exact_scope_and_freshness() {
    MemoryAgentFixture fixture;
    const auto tenant = fixture.principal.tenant_id;
    const auto room = Id128::generate();
    const auto app = Id128::generate();
    const auto entity = Id128::generate();
    const WorldProperty property{entity, "display_scale"};
    const auto environment = world_environment_scope(tenant, room).value();
    const auto other_environment = world_environment_scope(tenant, Id128::generate()).value();
    const auto application = world_application_scope(tenant, room, app).value();
    const auto other_app = world_application_scope(tenant, room, Id128::generate()).value();
    const auto foreign = world_application_scope(TenantId::generate(), room, app).value();
    const auto now = std::chrono::system_clock::now();
    const auto event = EventId::generate();
    const auto old = make_world_observation(environment, property, "100%", event,
                                            {now - 4h, now + 4h}, now - 3h);
    const auto expired = make_world_observation(environment, property, "125%", EventId::generate(),
                                                {now - 2h, now - 1h}, now - 90min);
    const auto delayed = make_world_observation(environment, property, "110%", EventId::generate(),
                                                {now - 3h, now + 4h}, now - 20min);
    const auto legacy_remote = make_world_observation(other_environment, property, "200%", event,
                                                      {now - 1h, now + 1h}, now - 1h);
    const auto local_app = make_world_observation(application, property, "150%", event,
                                                  {now - 1h, now + 1h}, now - 1h);
    const auto remote_app =
        make_world_observation(other_app, property, "175%", event, {now - 1h, now + 1h}, now - 1h);
    const auto remote_tenant =
        make_world_observation(foreign, property, "300%", event, {now - 1h, now + 1h}, now - 1h);
    for (const auto *record :
         {&old, &expired, &delayed, &legacy_remote, &local_app, &remote_app, &remote_tenant}) {
        MIRA_CHECK(*record);
        const auto written = write(fixture, record->value());
        MIRA_CHECK(written && written.value().count_of(CandidateDisposition::Applied) == 1);
    }
    MIRA_CHECK(old.value().id != legacy_remote.value().id &&
               local_app.value().id != remote_tenant.value().id);

    WorldFactQuery query{environment, property, {}, {}};
    auto stale = fixture.memory->lookup_world_fact(query);
    MIRA_CHECK(stale && stale.value().state == WorldFactState::Expired &&
               stale.value().requires_observation() && !stale.value().record);
    query.as_of_valid = now - 90min;
    MIRA_CHECK(reads(*fixture.memory, query, expired.value()));
    query.as_of_recorded = now - 100min;
    MIRA_CHECK(reads(*fixture.memory, query, old.value()));
    query.as_of_recorded.reset();
    query.as_of_valid = expired.value().validity.valid_until;
    MIRA_CHECK(fixture.memory->lookup_world_fact(query).value().state == WorldFactState::Expired);
    query.as_of_valid.reset();
    query.property.property = "color_scheme";
    MIRA_CHECK(fixture.memory->lookup_world_fact(query).value().state == WorldFactState::Missing);
    query.property = property;
    query.property.entity = Id128::generate();
    MIRA_CHECK(fixture.memory->lookup_world_fact(query).value().state == WorldFactState::Missing);
    query.property = property;
    for (const auto *record : {&legacy_remote, &local_app, &remote_app, &remote_tenant}) {
        query.scope = record->value().scope;
        MIRA_CHECK(reads(*fixture.memory, query, record->value()));
    }

    const auto refreshed = make_world_observation(
        environment, property, "125%", EventId::generate(), {now - 10min, now + 2h}, now - 5min);
    MIRA_CHECK(refreshed &&
               write(fixture, refreshed.value()).value().count_of(CandidateDisposition::Applied) ==
                   1);
    query.scope = environment;
    MIRA_CHECK(reads(*fixture.memory, query, refreshed.value()));
    const auto confirmed = write(fixture, refreshed.value());
    MIRA_CHECK(confirmed && confirmed.value().entries.front().idempotent_replay);
    auto conflicting = refreshed.value();
    conflicting.statement = "130%";
    auto rejection = write(fixture, conflicting);
    MIRA_CHECK(rejection &&
               rejection.value().count_of(CandidateDisposition::RejectedConflict) == 1);

    MIRA_CHECK(fixture.memory->close());
    SqliteMemoryStoreOptions options;
    options.path = fixture.root / "memory.db";
    auto reopened = SqliteMemoryStore::open(fixture.executor, options);
    MIRA_CHECK(reopened);
    fixture.memory = std::move(reopened).value();
    MIRA_CHECK(reads(*fixture.memory, query, refreshed.value()));

    ErasureRequest erase;
    erase.scope = environment;
    erase.reason = "fixture world erasure";
    const auto erased = fixture.memory->erase(erase);
    MIRA_CHECK(erased && erased.value().status == ErasureStatus::Complete &&
               erased.value().counts.records_removed == 4);
    query.as_of_valid = now - 90min;
    MIRA_CHECK(fixture.memory->lookup_world_fact(query).value().state == WorldFactState::Missing);
    query.as_of_valid.reset();
    query.scope = application;
    MIRA_CHECK(reads(*fixture.memory, query, local_app.value()));
    return 0;
}

int ambiguous_and_untrusted_observation() {
    MemoryAgentFixture fixture;
    const auto scope =
        world_environment_scope(fixture.principal.tenant_id, Id128::generate()).value();
    const WorldProperty property{Id128::generate(), "orientation"};
    const auto now = std::chrono::system_clock::now();
    const auto build = [&](std::string value, ValidityInterval validity = {}) {
        if (validity.valid_from == std::chrono::system_clock::time_point{})
            validity = {now - 1h, now + 1h};
        return make_world_observation(scope, property, std::move(value), EventId::generate(),
                                      validity, now);
    };
    MIRA_CHECK(!build("Portrait", {now - 1h, {}}));
    const auto fact = build("Portrait");
    const auto tie = build("Landscape");
    MIRA_CHECK(fact && tie);
    auto untrusted = write(fixture, fact.value(), true);
    MIRA_CHECK(untrusted && untrusted.value().count_of(CandidateDisposition::RejectedInvalid) == 1);
    auto unsafe = build(default_forbidden_content_markers().front());
    MIRA_CHECK(
        unsafe &&
        write(fixture, unsafe.value()).value().count_of(CandidateDisposition::RejectedForbidden) ==
            1);
    MIRA_CHECK(write(fixture, fact.value()).value().count_of(CandidateDisposition::Applied) == 1);
    MIRA_CHECK(write(fixture, tie.value()).value().count_of(CandidateDisposition::Applied) == 1);
    auto conflict = fixture.memory->lookup_world_fact({scope, property, {}, {}});
    MIRA_CHECK(conflict && conflict.value().state == WorldFactState::Ambiguous &&
               conflict.value().requires_observation() && !conflict.value().record);

    MemoryQuery raw;
    raw.scopes = {scope};
    MIRA_CHECK(!fixture.memory->query(raw));
    fixture.start();
    auto binding = fixture.binding();
    binding.scopes = {scope};
    MIRA_CHECK(!fixture.service->bind(binding));
    return 0;
}
} // namespace

int main() {
    if (const int result = exact_scope_and_freshness(); result != 0)
        return result;
    if (const int result = ambiguous_and_untrusted_observation(); result != 0)
        return result;
    return records_committed_verification();
}
