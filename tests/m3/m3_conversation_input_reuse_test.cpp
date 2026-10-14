// ConversationLoop input-reuse identity annotation tests (DEC-088,
// downstream ledger MIRA-20261014-005): the host owns the cross-turn reuse
// count — the loop is per-run — while the loop renders one Internal note as
// the last part of the user envelope from the third consecutive identical
// image supply, with the count and annotation visible on the
// ModelInputSupplied event and the run result.

#include "support/conversation_input_supply_fixture.hpp"
#include "support/test.hpp"

namespace {

using namespace mira;
using namespace mira::testing;

// ---------------------------------------------------------------------------
// 15. DEC-088 input-reuse identity annotation.
//
// The host owns the cross-turn reuse count (the loop is per-run); the loop
// renders one Internal note as the last part of the user envelope from the
// third consecutive identical image supply, with the count and annotation
// visible on the ModelInputSupplied event and the run result.
// ---------------------------------------------------------------------------

int reuse_annotation_renders_from_third_identical_supply() {
    SupplyFixture fixture;
    fixture.use_provider({
        tool_call_response(fixture.screenshot_spec_, R"json({})json", "call-1"),
        tool_call_response(fixture.screenshot_spec_, R"json({})json", "call-2"),
        tool_call_response(fixture.screenshot_spec_, R"json({})json", "call-3"),
        text_response("already analyzed"),
    });
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_model_input_supplier(
        [&fixture](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
            const std::lock_guard lock(fixture.captured_mutex_);
            std::vector<ModelInputItem> items;
            if (!fixture.captured_.empty()) {
                items.push_back(user_image_item(fixture.captured_.back()));
            }
            return items;
        });
    // The seam resolves once per image-bearing request: the empty first offer
    // carries no identity, then the re-offer counts run 0, 1, 2.
    std::uint32_t identity_calls = 0;
    loop.set_model_input_identity([&identity_calls](const AgentLoopSpec &) {
        InputReuseMeta meta;
        meta.content_digest = "shot-digest";
        meta.reuse_count = identity_calls++;
        meta.semantic_label = "screenshot";
        return meta;
    });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().answer == "already analyzed");
    MIRA_CHECK(result.value().supplied_input_items == 3);
    MIRA_CHECK(result.value().supplied_input_reuse_annotations == 1);

    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 4);
    // Turn 1 offers nothing yet and carries no identity.
    const auto *first = primary_user_item(requests[0]);
    MIRA_CHECK(first != nullptr && first->content.size() == 1);
    // Turns 2-3: first supplies and re-offers below the threshold stay clean.
    const auto *second = primary_user_item(requests[1]);
    MIRA_CHECK(second != nullptr && second->content.size() == 2);
    const auto *third = primary_user_item(requests[2]);
    MIRA_CHECK(third != nullptr && third->content.size() == 2);
    // Turn 4: the second re-offer (reuse_count 2) appends the Internal note
    // as the last part of the user envelope.
    const auto *fourth = primary_user_item(requests[3]);
    MIRA_CHECK(fourth != nullptr && fourth->content.size() == 3);
    const auto *note = std::get_if<TextPart>(&fourth->content.back());
    MIRA_CHECK(note != nullptr);
    MIRA_CHECK(note->sensitivity == Sensitivity::Internal);
    MIRA_CHECK(note->text ==
               "[System note: The screenshot above is the same as in the previous 2 turns. "
               "No need to re-analyze it unless the task explicitly requires it.]");

    const auto supplied = supplied_events(fixture);
    MIRA_CHECK(supplied.size() == 4);
    MIRA_CHECK(int_field(supplied[0], "reuse_count") == -1); // no identity on an empty offer
    MIRA_CHECK(int_field(supplied[1], "reuse_count") == 0);
    MIRA_CHECK(bool_field(supplied[1], "reuse_annotated") == std::optional<bool>{false});
    MIRA_CHECK(int_field(supplied[2], "reuse_count") == 1);
    MIRA_CHECK(bool_field(supplied[2], "reuse_annotated") == std::optional<bool>{false});
    const auto *digest = string_field(supplied[1], "content_digest");
    MIRA_CHECK(digest != nullptr && *digest == "shot-digest");
    MIRA_CHECK(int_field(supplied[3], "reuse_count") == 2);
    MIRA_CHECK(bool_field(supplied[3], "reuse_annotated") == std::optional<bool>{true});
    return 0;
}

int text_only_supply_never_annotates_reuse() {
    SupplyFixture fixture;
    fixture.use_provider({
        tool_call_response(fixture.echo_spec_, R"json({"message": "hi"})json", "call-1"),
        text_response("done"),
    });
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_model_input_supplier([](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
        ModelInputItem item;
        item.role = ModelRole::User;
        TextPart text;
        text.text = "plain host text";
        item.content.emplace_back(std::move(text));
        return std::vector<ModelInputItem>{std::move(item)};
    });
    // A qualifying count still annotates nothing without an image: text
    // re-offers are cheap and the note is reserved for pixel re-analysis.
    loop.set_model_input_identity([](const AgentLoopSpec &) {
        InputReuseMeta meta;
        meta.content_digest = "text-digest";
        meta.reuse_count = 5;
        meta.semantic_label = "screenshot";
        return meta;
    });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().supplied_input_reuse_annotations == 0);
    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 2);
    for (const auto &request : requests) {
        const auto *user = primary_user_item(request);
        MIRA_CHECK(user != nullptr);
        for (const auto &part : user->content) {
            const auto *text = std::get_if<TextPart>(&part);
            MIRA_CHECK(text == nullptr || text->text.find("[System note:") == std::string::npos);
        }
    }
    // The identity still rides the event for traceability, unannotated.
    const auto supplied = supplied_events(fixture);
    MIRA_CHECK(supplied.size() == 2);
    MIRA_CHECK(int_field(supplied[0], "reuse_count") == 5);
    MIRA_CHECK(bool_field(supplied[0], "reuse_annotated") == std::optional<bool>{false});
    return 0;
}

int identity_exception_skips_annotation_visibly() {
    SupplyFixture fixture;
    fixture.use_provider({
        tool_call_response(fixture.screenshot_spec_, R"json({})json", "call-1"),
        tool_call_response(fixture.screenshot_spec_, R"json({})json", "call-2"),
        text_response("answered once"),
    });
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_model_input_supplier(
        [&fixture](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
            const std::lock_guard lock(fixture.captured_mutex_);
            std::vector<ModelInputItem> items;
            for (const auto &reference : fixture.captured_) {
                items.push_back(user_image_item(reference));
            }
            return items;
        });
    // First resolution answers, the second one throws: the annotation degrades
    // visibly while the supply itself continues untouched.
    loop.set_model_input_identity(
        [first = true](const AgentLoopSpec &) mutable -> std::optional<InputReuseMeta> {
            if (first) {
                first = false;
                InputReuseMeta meta;
                meta.reuse_count = 0;
                return meta;
            }
            throw std::runtime_error("identity boom");
        });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    // The failing identity degraded the annotation, not the supply.
    MIRA_CHECK(result.value().supply_degradations == 1);
    MIRA_CHECK(result.value().supplied_input_reuse_annotations == 0);
    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 3);
    const auto *second = primary_user_item(requests[1]);
    MIRA_CHECK(second != nullptr && second->content.size() == 2);
    const auto *third = primary_user_item(requests[2]);
    MIRA_CHECK(third != nullptr && third->content.size() == 3);
    for (const auto &part : third->content) {
        const auto *text = std::get_if<TextPart>(&part);
        MIRA_CHECK(text == nullptr || text->text.find("[System note:") == std::string::npos);
    }
    const auto degraded = degraded_events(fixture);
    MIRA_CHECK(degraded.size() == 1);
    const auto *reason = string_field(degraded.front(), "reason");
    MIRA_CHECK(reason != nullptr && *reason == "identity-exception");
    return 0;
}

} // namespace

int main() {
    if (const int code = reuse_annotation_renders_from_third_identical_supply(); code != 0) {
        return code;
    }
    if (const int code = text_only_supply_never_annotates_reuse(); code != 0) {
        return code;
    }
    return identity_exception_skips_annotation_visibly();
}
