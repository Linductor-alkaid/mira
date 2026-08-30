/*
 * Mira Android Host ABI, version 1.
 *
 * This header freezes the versioned C ABI between an Android Host (the app,
 * service and JNI bridge owning permissions, lifecycle, MediaProjection,
 * Accessibility and platform dispatchers) and the Native Android Adapter
 * inside the Mira NDK library. It is pure C: no STL, no exceptions, no RTTI
 * and no Android or JNI types cross this boundary.
 *
 * Conventions (mandatory for both sides):
 *
 *  - Every struct starts with `struct_size` in bytes. Receivers validate
 *    `struct_size` and `abi_version` and safely ignore unknown trailing
 *    extension fields; a struct smaller than the versioned prefix is
 *    rejected with MIRA_HOST_ERR_INVALID_ARGUMENT.
 *  - Strings are UTF-8 pointer plus length and are never NUL terminated.
 *    All arrays carry an explicit count bounded by a MIRA_MAX_* constant.
 *  - Every async operation returns an operation handle. Exactly one
 *    terminal callback is delivered per handle; a second terminal callback
 *    for the same handle is a contract violation that must be counted and
 *    ignored, never applied twice.
 *  - Callbacks run on host-declared threads. The native bridge may only do
 *    bounded validation, bounded copies and completion submission inside a
 *    callback; it must not run Core state machines or block.
 *  - Buffer memory is handed over through MiraHostBufferLeaseV1. The native
 *    side calls lease->release(lease_id, user_data) exactly once per
 *    delivered lease, on success, failure, cancellation and host stop alike.
 *  - `stop` and `destroy` are idempotent. `destroy` must not complete while
 *    outstanding leases or terminal callbacks exist; hosts report that state
 *    instead of freeing memory native may still touch.
 *  - Hosts never call back after `destroy` returns. Late callbacks are
 *    contract violations and must be dropped by the native bridge.
 *
 * Mapping to Mira core errors: MIRA_HOST_ERR_PERMISSION_DENIED ->
 * PermissionDenied, MIRA_HOST_ERR_UNAVAILABLE -> Unavailable (retryable),
 * MIRA_HOST_ERR_UNSUPPORTED_VERSION -> UnsupportedVersion,
 * MIRA_HOST_ERR_CANCELLED -> Cancelled, MIRA_HOST_ERR_DEADLINE_EXCEEDED ->
 * DeadlineExceeded, MIRA_HOST_ERR_EXECUTION_UNCERTAIN ->
 * ExecutionUncertain, MIRA_HOST_ERR_PLATFORM_ERROR -> PlatformError. Core
 * never branches on host message text; diagnostics travel redacted.
 */

#ifndef MIRA_ANDROID_HOST_ABI_V1_H
#define MIRA_ANDROID_HOST_ABI_V1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIRA_ANDROID_ABI_VERSION 1
#define MIRA_MAX_PLANES 4
#define MIRA_MAX_INPUT_EVENTS 64
#define MIRA_MAX_TOPOLOGY_DISPLAYS 8

/* Status codes are stable values; new codes may only be appended. */
typedef enum MiraHostStatus {
    MIRA_HOST_OK = 0,
    MIRA_HOST_ERR_INVALID_ARGUMENT = 1,
    MIRA_HOST_ERR_UNSUPPORTED_VERSION = 2,
    MIRA_HOST_ERR_UNAVAILABLE = 3,
    MIRA_HOST_ERR_PERMISSION_DENIED = 4,
    MIRA_HOST_ERR_CANCELLED = 5,
    MIRA_HOST_ERR_DEADLINE_EXCEEDED = 6,
    MIRA_HOST_ERR_INVALID_STATE = 7,
    MIRA_HOST_ERR_INVALID_BUFFER = 8,
    MIRA_HOST_ERR_INVALID_TOPOLOGY = 9,
    MIRA_HOST_ERR_EXECUTION_UNCERTAIN = 10,
    MIRA_HOST_ERR_CAPACITY = 11,
    MIRA_HOST_ERR_PLATFORM_ERROR = 12
} MiraHostStatus;

/* Pixel formats mirror mira::PixelFormat; values are frozen. */
typedef enum MiraHostPixelFormat {
    MIRA_HOST_PIXEL_RGBA8888 = 0,
    MIRA_HOST_PIXEL_BGRA8888 = 1,
    MIRA_HOST_PIXEL_RGBX8888 = 2,
    MIRA_HOST_PIXEL_NV12 = 3,
    MIRA_HOST_PIXEL_YUV420P = 4,
    MIRA_HOST_PIXEL_GRAY8 = 5
} MiraHostPixelFormat;

/* Rotations mirror mira::Rotation; values are frozen. */
typedef enum MiraHostRotation {
    MIRA_HOST_ROTATION_0 = 0,
    MIRA_HOST_ROTATION_90 = 1,
    MIRA_HOST_ROTATION_180 = 2,
    MIRA_HOST_ROTATION_270 = 3
} MiraHostRotation;

typedef enum MiraHostOperationKind {
    MIRA_HOST_OP_CAPTURE_FRAME = 1,
    MIRA_HOST_OP_GET_UI_TREE = 2,
    MIRA_HOST_OP_DISPATCH_INPUT = 3
} MiraHostOperationKind;

/* Input event kinds; payload coordinates are canonical [0, 1] doubles. */
typedef enum MiraHostInputKind {
    MIRA_HOST_INPUT_TAP = 1,
    MIRA_HOST_INPUT_LONG_PRESS = 2,
    MIRA_HOST_INPUT_SWIPE = 3,
    MIRA_HOST_INPUT_TYPE = 4,
    MIRA_HOST_INPUT_BACK = 5,
    MIRA_HOST_INPUT_HOME = 6,
    MIRA_HOST_INPUT_RELEASE_ALL = 7
} MiraHostInputKind;

/* Receipt of a dispatch input operation, mirroring mira::ExecutionStatus. */
typedef enum MiraHostInputReceipt {
    MIRA_HOST_INPUT_RECEIPT_DISPATCHED = 1,
    MIRA_HOST_INPUT_RECEIPT_COMPLETED = 2,
    MIRA_HOST_INPUT_RECEIPT_REJECTED = 3,
    MIRA_HOST_INPUT_RECEIPT_UNKNOWN = 4
} MiraHostInputReceipt;

typedef struct MiraPlaneV1 {
    uint64_t offset;
    uint32_t row_stride;
    uint32_t pixel_stride;
    uint32_t width;
    uint32_t height;
} MiraPlaneV1;

/*
 * Buffer lease: temporary access to host memory. The lease struct and its
 * pointers are valid only during the callback that delivered it; the native
 * bridge copies the struct synchronously and must then call release()
 * exactly once. `data` stays readable until release() is called; size and
 * plane ranges must be validated by the consumer before reading. release()
 * is exactly once and thread safe.
 */
typedef struct MiraHostBufferLeaseV1 {
    uint64_t struct_size;
    uint64_t lease_id;
    const uint8_t* data;
    uint64_t size;
    uint32_t plane_count;
    MiraPlaneV1 planes[MIRA_MAX_PLANES];
    void (*release)(uint64_t lease_id, void* user_data);
    void* user_data;
} MiraHostBufferLeaseV1;

typedef struct MiraAndroidHostConfigV1 {
    uint64_t struct_size;
    uint32_t abi_version;
    uint32_t reserved0;
} MiraAndroidHostConfigV1;

/*
 * Terminal result of one async operation. `correlation` echoes the value
 * supplied by the native side in its request, so results can be matched
 * even when the host completes an operation before the submit call returns.
 */
typedef struct MiraHostOperationResultV1 {
    uint64_t struct_size;
    uint64_t correlation;
    uint64_t host_generation;
    MiraHostStatus status;
    uint32_t kind;
    /* Frame payload for MIRA_HOST_OP_CAPTURE_FRAME; lease is NULL
       otherwise. The native side releases it exactly once. */
    MiraHostBufferLeaseV1* lease;
    uint64_t frame_id;
    uint64_t display_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t rotation;
    uint64_t capture_begin_ns;
    uint64_t capture_end_ns;
    uint64_t environment_epoch;
    /* UI tree payload for MIRA_HOST_OP_GET_UI_TREE, delivered as a frame
       lease the native side copies from and releases. */
    uint32_t input_receipt;  /* MiraHostInputReceipt for input operations. */
    uint32_t side_effect_may_have_occurred;
} MiraHostOperationResultV1;

typedef struct MiraHostCapabilitiesV1 {
    uint64_t struct_size;
    uint32_t abi_version;
    /* Bit masks over MiraHostPixelFormat values. */
    uint32_t screenshot_pixel_formats_mask;
    uint32_t screenshot_backends; /* bit 0: virtual display, others reserved. */
    uint32_t max_frame_width;
    uint32_t max_frame_height;
    uint32_t accessibility_completeness; /* 0 = none, 1 = partial, 2 = full. */
    uint32_t supported_node_actions_mask;
    uint32_t input_capabilities_mask; /* bits over MiraHostInputKind. */
    uint32_t max_gesture_duration_ms;
    uint32_t max_pointers;
    /* 0 = callbacks on an internal host thread, 1 = caller thread,
       2 = platform main/service thread. */
    uint32_t callback_thread_model;
    uint32_t lifecycle_state;  /* 0 = created, 1 = started, 2 = stopped. */
    uint32_t permission_state; /* 0 = granted, 1 = revoked, 2 = unknown. */
    uint32_t secure_surface_policy; /* 0 = masked, 1 = dropped, 2 = unknown. */
    uint64_t topology_version;
    uint64_t environment_epoch;
    uint64_t host_sequence;   /* Monotonic capability change counter. */
    uint64_t host_generation; /* Changes when the host process is recreated. */
} MiraHostCapabilitiesV1;

typedef struct MiraHostDisplayV1 {
    uint64_t display_id;
    uint32_t native_width;
    uint32_t native_height;
    uint32_t rotation;
    double pixels_per_logical;
    double inset_left;
    double inset_top;
    double inset_right;
    double inset_bottom;
    uint32_t active;
} MiraHostDisplayV1;

typedef struct MiraHostTopologyV1 {
    uint64_t struct_size;
    uint64_t environment_epoch;
    uint64_t topology_version;
    uint32_t display_count;
    MiraHostDisplayV1 displays[MIRA_MAX_TOPOLOGY_DISPLAYS];
} MiraHostTopologyV1;

typedef struct MiraHostFrameRequestV1 {
    uint64_t struct_size;
    uint64_t correlation;
    uint64_t display_id;
    uint64_t deadline_ns; /* Monotonic deadline; 0 means no host deadline. */
} MiraHostFrameRequestV1;

typedef struct MiraHostTreeRequestV1 {
    uint64_t struct_size;
    uint64_t correlation;
    uint64_t display_id;
    uint64_t max_bytes;
    uint64_t deadline_ns;
} MiraHostTreeRequestV1;

typedef struct MiraHostInputEventV1 {
    uint32_t kind; /* MiraHostInputKind */
    double x;
    double y;
    double x2;
    double y2;
    uint32_t duration_ms;
    const char* text;
    uint32_t text_length;
} MiraHostInputEventV1;

typedef struct MiraHostInputRequestV1 {
    uint64_t struct_size;
    uint64_t correlation;
    uint64_t display_id;
    uint32_t event_count;
    MiraHostInputEventV1 events[MIRA_MAX_INPUT_EVENTS];
    uint64_t deadline_ns;
} MiraHostInputRequestV1;

/*
 * Callback table supplied at create time. Unknown extensions are ignored by
 * comparing struct_size; a callback pointer may be NULL only for
 * on_capabilities_changed, which is then simply not delivered.
 */
typedef struct MiraHostCallbacksV1 {
    uint64_t struct_size;
    void* user_data;
    void (*on_operation_complete)(void* user_data,
                                  const MiraHostOperationResultV1* result);
    void (*on_capabilities_changed)(void* user_data,
                                    const MiraHostCapabilitiesV1* capabilities);
} MiraHostCallbacksV1;

typedef struct MiraAndroidHostV1 MiraAndroidHostV1;

/*
 * Lifecycle. In production the host is usually created from Java/Kotlin and
 * registered with native; these entry points express the stable semantics
 * the native adapter depends on regardless of the creation direction.
 *
 * `create` validates struct_size/abi_version on both input structs. `start`
 * makes operation submission possible; submissions before start or after
 * stop fail with MIRA_HOST_ERR_INVALID_STATE. `stop` cancels in-flight
 * operations (their callbacks still fire with MIRA_HOST_ERR_CANCELLED),
 * waits for outstanding leases up to a host deadline and reports
 * MIRA_HOST_ERR_EXECUTION_UNCERTAIN when leases remain. `destroy` fails
 * with MIRA_HOST_ERR_INVALID_STATE while leases or callbacks are
 * outstanding and is otherwise idempotent.
 */
MiraHostStatus mira_android_host_create_v1(const MiraAndroidHostConfigV1* config,
                                           const MiraHostCallbacksV1* callbacks,
                                           MiraAndroidHostV1** out_host);
MiraHostStatus mira_android_host_start_v1(MiraAndroidHostV1* host);
MiraHostStatus mira_android_host_stop_v1(MiraAndroidHostV1* host);
MiraHostStatus mira_android_host_destroy_v1(MiraAndroidHostV1* host);

MiraHostStatus mira_android_host_get_capabilities_v1(MiraAndroidHostV1* host,
                                                     MiraHostCapabilitiesV1* out);
MiraHostStatus mira_android_host_get_topology_v1(MiraAndroidHostV1* host,
                                                 MiraHostTopologyV1* out);

/*
 * Async operations. On success the host returns MIRA_HOST_OK and sets
 * *out_correlation/out_operation; exactly one terminal callback with the
 * same correlation follows. Requests with a wrong struct_size or unknown
 * abi_version fail fast with MIRA_HOST_ERR_INVALID_ARGUMENT and never
 * call back.
 */
MiraHostStatus mira_android_host_capture_frame_v1(MiraAndroidHostV1* host,
                                                  const MiraHostFrameRequestV1* request,
                                                  uint64_t* out_operation);
MiraHostStatus mira_android_host_get_ui_tree_v1(MiraAndroidHostV1* host,
                                                const MiraHostTreeRequestV1* request,
                                                uint64_t* out_operation);
MiraHostStatus mira_android_host_dispatch_input_v1(MiraAndroidHostV1* host,
                                                   const MiraHostInputRequestV1* request,
                                                   uint64_t* out_operation);

/*
 * Cooperative cancellation. Atomic platform calls that already reached the
 * input pipeline cannot be unsafely interrupted; their callbacks then
 * report MIRA_HOST_ERR_EXECUTION_UNCERTAIN with
 * side_effect_may_have_occurred = 1 instead of MIRA_HOST_ERR_CANCELLED.
 * Cancelling an unknown or settled operation is a successful no-op.
 */
MiraHostStatus mira_android_host_cancel_operation_v1(MiraAndroidHostV1* host,
                                                     uint64_t operation);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MIRA_ANDROID_HOST_ABI_V1_H */
