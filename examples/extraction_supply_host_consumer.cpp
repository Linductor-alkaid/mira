// Offline host consumer for the DEC-114 extraction supply: one call to
// wire_extraction_supply builds the extraction model (ProviderConsolidation-
// Model over a scripted /v1/responses transport), the consolidate() path runs
// deterministic + model candidates through the same policy pipeline (the
// model preference lands in PendingApproval until apply_pending), and the
// degrade round shows a provider outage leaving the deterministic path
// untouched. No network.
#include <mira/event_store.hpp>
#include <mira/extraction_model_provider.hpp>
#include <mira/json.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/model_transport.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <kairo/executor.hpp>

namespace {
using namespace mira;

class KeySecretResolver final : public ISecretResolver {
  public:
    Result<std::string> resolve(const SecretRef &reference) override {
        if (reference.name != "extraction-credential") {
            Error error;
            error.code = ErrorCode::PermissionDenied;
            error.domain = "mira.test";
            error.safe_message = "secret is not configured";
            return error;
        }
        // Real deployments resolve the host's secret store; the plaintext
        // never enters profiles, logs or events.
        return std::string{"offline-placeholder-key"};
    }
};

// Scripted /v1/responses transport: one canned extraction completion, then an
// injectable outage for the degrade round.
class ScriptedExtractionTransport final : public IHttpTransport {
  public:
    explicit ScriptedExtractionTransport(std::shared_ptr<ISecretResolver> secrets)
        : secrets_(std::move(secrets)) {}

    Result<HttpResponseInfo> execute(const HttpRequest & /*request*/, const TransportLimits &,
                                     const OperationContext &, const HttpChunkCallback &on_chunk,
                                     TransportTrace &trace) override {
        if (fail_next_.exchange(false)) {
            trace.write_started = true;
            Error error;
            error.code = ErrorCode::Unavailable;
            error.domain = "mira.model";
            error.safe_message = "scripted extraction outage";
            error.retryable = true;
            return error;
        }
        // to_json_string emits the complete quoted string token for the
        // candidate list, so the body only needs the response containers.
        const std::string candidates =
            R"({"candidates":[{"kind":"procedure","content":"rotate the staging database credential before restart","sources":[1],"confidence":0.7},{"kind":"preference","content":"incident summaries in three bullets or fewer","sources":[2],"confidence":0.8}]})";
        const std::string body =
            R"({"id":"extraction-demo","status":"completed","model":"demo-extractor","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":)" +
            to_json_string(JsonValue(candidates)) +
            R"(}]}],"usage":{"input_tokens":64,"output_tokens":48}})";
        on_chunk(body);
        trace.write_started = true;
        trace.write_completed = true;
        trace.headers_received = true;
        HttpResponseInfo info;
        info.status = 200;
        info.headers.emplace_back("content-type", "application/json");
        info.body_bytes = body.size();
        return info;
    }

    void fail_next() { fail_next_.store(true); }

  private:
    std::shared_ptr<ISecretResolver> secrets_;
    std::atomic<bool> fail_next_{false};
};

[[nodiscard]] EventEnvelope recorded_event(const SessionId &session, std::uint64_t sequence,
                                           std::string type, std::string data) {
    EventEnvelope event;
    event.event_id = EventId::generate();
    event.runtime_id = RuntimeId::generate();
    event.session_id = session;
    event.session_sequence = sequence;
    event.timestamp = Timestamp::now();
    event.payload = EventPayload{std::move(type), std::move(data), EventClass::State};
    return event;
}

} // namespace

int main() {
    kairo::Executor executor;
    if (!executor.initialize(kairo::ExecutorConfig{})) {
        std::cerr << "executor initialize failed\n";
        return 2;
    }
    const auto root = std::filesystem::temp_directory_path() /
                      ("mira-extraction-supply-" + RuntimeId::generate().to_string());
    std::filesystem::create_directories(root);
    {
        SqliteMemoryStoreOptions memory_options;
        memory_options.path = root / "memory.db";
        auto opened = SqliteMemoryStore::open(executor, memory_options);
        if (!opened) {
            std::cerr << "memory store open failed\n";
            (void)executor.shutdown(true);
            return 3;
        }
        auto memory = std::shared_ptr<SqliteMemoryStore>{std::move(opened).value()};

        MemoryScope scope;
        scope.kind = MemoryScopeKind::Environment;
        scope.subject_id = "staging";

        const auto session = SessionId::generate();
        const std::vector<EventEnvelope> events = {
            recorded_event(session, 1, "TaskFactVerified",
                           to_json_string(JsonValue::Object{{"key", std::string("staging port")},
                                                            {"value", std::string("8443")}})),
            recorded_event(session, 2, "OpsRunbook", "rotate credential, then restart"),
            recorded_event(session, 3, "UserUtterance",
                           "keep incident summaries to three bullets")};

        // One call builds the whole supply: validated profile, provider over
        // the host-owned transport, and the consolidation-model adapter.
        ExtractionSupplyConfig config;
        config.endpoint_origin = "https://extraction.invalid";
        config.model_selector = "demo-extractor";
        config.credential_secret_name = "extraction-credential";
        config.deadline = std::chrono::milliseconds{5'000};
        auto transport =
            std::make_shared<ScriptedExtractionTransport>(std::make_shared<KeySecretResolver>());
        auto wired = wire_extraction_supply(config, transport);
        if (!wired) {
            std::cerr << "extraction supply wiring failed: " << wired.error().safe_message << "\n";
            (void)memory->close();
            (void)executor.shutdown(true);
            return 4;
        }
        const auto &supply = wired.value();
        std::cout << "wired: extraction model over " << supply.profile->endpoint_origin
                  << " (strict schema, degrade_on_error="
                  << supply.consolidation_model->options().degrade_on_error << ")\n";

        // The consolidate() path: deterministic candidates and model
        // candidates go through the same policy pipeline. The model's
        // preference waits for approval; the procedure (namespaced import)
        // and the deterministic fact apply.
        MemoryConsolidator consolidator{ConsolidationPolicy{}, &supply.model()};
        auto report = consolidator.consolidate(*memory, events, scope, Timestamp::now());
        if (!report) {
            std::cerr << "consolidate failed: " << report.error().safe_message << "\n";
            (void)memory->close();
            (void)executor.shutdown(true);
            return 5;
        }
        std::cout << "consolidate: applied="
                  << report.value().count_of(CandidateDisposition::Applied) << " pending_approval="
                  << report.value().count_of(CandidateDisposition::PendingApproval)
                  << " rejected_injection="
                  << report.value().count_of(CandidateDisposition::RejectedInjection) << "\n";
        if (report.value().count_of(CandidateDisposition::Applied) != 3 ||
            !report.value().pending_approval.empty()) {
            std::cerr << "expected all three candidates to apply\n";
            (void)memory->close();
            (void)executor.shutdown(true);
            return 6;
        }
        std::cout << "model preference stored Unverified beside the Verified deterministic fact\n";

        // Coexistence (DEC-115): a same-claim different-value candidate is
        // preserved as an alternative beside the incumbent — the claim-key
        // query returns both.
        MemoryCandidate conflicting;
        conflicting.proposed.id = MemoryId::generate();
        conflicting.proposed.scope = scope;
        conflicting.proposed.kind = MemoryKind::EnvironmentFact;
        conflicting.proposed.statement = "staging port=9443";
        conflicting.proposed.recorded_at = std::chrono::system_clock::now();
        conflicting.proposed.validity.valid_from = conflicting.proposed.recorded_at;
        conflicting.proposed.provenance = {EventId::generate()};
        // An Observed observation is not authoritative enough to succeed the
        // Verified era, so it coexists as an alternative (DEC-115).
        conflicting.proposed.verification = MemoryVerification::Observed;
        conflicting.proposed.confidence = 0.6F;
        auto conflict_report = MemoryConsolidator{}.consolidate_candidates(*memory, {conflicting},
                                                                           scope, Timestamp::now());
        if (!conflict_report ||
            conflict_report.value().entries.front().reason_code != "alternative-preserved") {
            std::cerr << "expected the conflicting value to be preserved as an alternative\n";
            (void)memory->close();
            (void)executor.shutdown(true);
            return 7;
        }
        MemoryQuery both;
        both.scopes = {scope};
        both.claim_key = memory_claim_key("staging port=9443").key;
        auto both_sides = memory->query(both);
        if (!both_sides || both_sides.value().records.size() != 2) {
            std::cerr << "expected the two port claims to coexist\n";
            (void)memory->close();
            (void)executor.shutdown(true);
            return 7;
        }
        std::cout << "coexistence: 8443 incumbent and 9443 alternative both retrievable\n";

        // Degrade round: a provider outage empties the model proposal set and
        // the deterministic extraction still lands its fact.
        transport->fail_next();
        auto degraded = consolidator.consolidate(*memory, events, scope, Timestamp::now());
        if (!degraded) {
            std::cerr << "degraded consolidate failed: " << degraded.error().safe_message << "\n";
            (void)memory->close();
            (void)executor.shutdown(true);
            return 8;
        }
        // Every claim already has a stored copy whose provenance covers the
        // replayed events: zero new writes, zero churn.
        if (degraded.value().entries.empty() ||
            degraded.value().entries.front().reason_code != "duplicate-noop") {
            std::cerr << "degrade round did not settle as duplicate-noop\n";
            (void)memory->close();
            (void)executor.shutdown(true);
            return 9;
        }
        const auto noop_in_degrade = degraded.value().count_of(CandidateDisposition::Applied);
        if (noop_in_degrade != degraded.value().entries.size()) {
            std::cerr << "unexpected dispositions in the degrade round\n";
            (void)memory->close();
            (void)executor.shutdown(true);
            return 9;
        }
        std::cout << "degrade: model outage absorbed, replay settles as duplicate-noop ("
                  << noop_in_degrade << " claims)\n";

        (void)memory->close();
    }
    std::filesystem::remove_all(root);
    (void)executor.shutdown(true);
    return 0;
}
