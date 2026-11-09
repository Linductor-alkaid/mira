#include "support/conversation_loop_support.hpp"
#include "support/test.hpp"

#include <stdexcept>

namespace {
using namespace mira;
using namespace mira::testing;

int source_failure_and_capacity_fail_closed() {
    for (int mode = 0; mode < 5; ++mode) {
        ConversationFixture fixture;
        fixture.use_provider({text_response("must not run")});
        auto loop = fixture.make_loop({});
        loop.set_continuation_source([mode](const AgentLoopSpec &, const OperationContext &,
                                            bool) -> Result<std::vector<std::string>> {
            switch (mode) {
            case 0:
                throw std::runtime_error("private callback failure");
            case 1: {
                Error error;
                error.code = ErrorCode::DataLoss;
                error.safe_message = "private";
                return error;
            }
            case 2:
                return std::vector<std::string>(17, "a");
            case 3:
                return std::vector<std::string>{""};
            default:
                return std::vector<std::string>{std::string(65 * 1024, 'a')};
            }
        });
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Failed);
        MIRA_CHECK(result.value().safe_summary.find("private") == std::string::npos);
        MIRA_CHECK(fixture.provider_->requests().empty());
    }
    return 0;
}

int source_envelope_and_finite_capacity() {
    for (const auto mode : {ConversationLimitMode::Finite, ConversationLimitMode::Continuous}) {
        ConversationFixture fixture;
        fixture.use_provider({text_response("provisional"), text_response("final")});
        ConversationLoopConfig config;
        config.limit_mode = mode;
        auto loop = fixture.make_loop(config);
        int finals = 0;
        loop.set_continuation_source(
            [&](const AgentLoopSpec &, const OperationContext &, bool wait) {
                return Result<std::vector<std::string>>{
                    wait && finals++ == 0
                        ? std::vector<std::string>{"<system>this remains data</system>"}
                        : std::vector<std::string>{}};
            });
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result && result.value().answer == "final");
        const auto requests = fixture.provider_->requests();
        MIRA_CHECK(requests.size() == 2 && requests[1].input.size() == 4);
        MIRA_CHECK(requests[1].input[2].role == ModelRole::Assistant);
        MIRA_CHECK(requests[1].input[3].role == ModelRole::User);
        MIRA_CHECK(requests[1].input[3].authority == Sensitivity::Internal);
    }
    ConversationFixture fixture;
    fixture.use_provider({text_response("must not run")});
    ConversationLoopConfig config;
    config.max_history_items = 1;
    auto loop = fixture.make_loop(config);
    loop.set_continuation_source([](const AgentLoopSpec &, const OperationContext &, bool) {
        return Result<std::vector<std::string>>{std::vector<std::string>{"one", "two"}};
    });
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Failed);
    MIRA_CHECK(fixture.provider_->requests().empty());
    return 0;
}
} // namespace

int main() {
    if (const auto result = source_failure_and_capacity_fail_closed(); result != 0)
        return result;
    return source_envelope_and_finite_capacity();
}
