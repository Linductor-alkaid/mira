#pragma once

// Case declarations for the M27 world-state gate matrix (WS-G2..G6). The case
// table in m27_world_state_test.cpp drives every case; the WS-G3 rebuild and
// WS-G6 wire/error-domain cases live in m27_wire_rebuild.cpp (split by the
// max-file-lines budget along the M24/M26 precedent, assertions unchanged).

#include "m27_support.hpp"

namespace mira::m27 {

// WS-G2: purity, validation and update-operator matrix (m27_world_state_test.cpp).
int options_validate_rejections();
int value_validate_rejections();
int observation_operator_matrix();
int screen_state_operator_zero_conflict();
int navigation_operator_matrix();
int tool_settled_operator_matrix();
int verification_operator_matrix();
int expiry_operator_matrix();
int purity_clock_independence_and_strong_consistency();

// WS-G4: capacity and eviction (m27_world_state_test.cpp).
int eviction_order_grid();
int insert_overflow_eviction_replayable();
int ring_fifo_capacity();
int summary_bound_rejected_zero_change();
int options_enter_rebuild_recipe();

// WS-G5: Unknown/staleness/conflict fail-closed matrix and read-only face
// (m27_world_state_test.cpp).
int default_state_unknown_and_legal();
int no_supply_stays_unknown_zero_confidence();
int stale_never_promoted_without_new_evidence();
int read_only_face_no_authorization_outlet();

// WS-G3: rebuild and cross-instance determinism (m27_wire_rebuild.cpp).
int frozen_dataset_digest_anchor();
int rebuild_equals_incremental_advance();
int prefix_replay_equivalence();
int prune_within_capacity_noop_and_outside_recipe();
int cross_thread_pipeline_replay_byte_identical();

// WS-G6: wire schema and error domain (m27_wire_rebuild.cpp).
int wire_round_trip_byte_identical();
int wire_version_policy_dec002();
int wire_fail_closed_reader();
int wire_key_sets_exactly_frozen();
int error_domain_eight_codes_triple_reachable();

} // namespace mira::m27
