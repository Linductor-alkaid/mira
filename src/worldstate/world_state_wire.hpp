#pragma once

// Private wire/digest helpers shared by the World State translation units
// (mira_core internal; not an installed header). The public contract is
// include/mira/world_state.hpp (M27 §4 frozen face); this header only factors
// the canonical-JSON builders, the input-record digests and the wire reader
// out of world_state.cpp to respect the architecture file-size budget.

#include <mira/json.hpp>
#include <mira/world_state.hpp>

namespace mira::ws_detail {

// Canonical JSON of one ElementRef (vocabulary §2.1 seven-field shape).
[[nodiscard]] JsonValue element_to_json(const ElementRef &element);

// Canonical JSON of the whole projection (mira.worldstate.v1 §4.4 key set).
[[nodiscard]] JsonValue world_state_to_json(const WorldState &state);

// M27 frozen source_event_digest semantics (§4.3 note): the canonical JSON
// digest of the input record itself; the EventEnvelope form arrives with the
// consumption milestone, entry schema unchanged.
[[nodiscard]] Sha256Digest input_digest(const WorldObservationInput &input);
[[nodiscard]] Sha256Digest input_digest(const WorldScreenStateInput &input);
[[nodiscard]] Sha256Digest input_digest(const WorldNavigationInput &input);
[[nodiscard]] Sha256Digest input_digest(const WorldToolSettledInput &input);
[[nodiscard]] Sha256Digest input_digest(const WorldVerificationInput &input);
[[nodiscard]] Sha256Digest input_digest(const WorldExpiryInput &input);

} // namespace mira::ws_detail
