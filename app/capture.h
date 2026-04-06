#pragma once

#include "fliprsdr.h"

typedef struct FlipRSDRCapture FlipRSDRCapture;
typedef struct FlipRSDRTransport FlipRSDRTransport;

#ifdef __cplusplus
extern "C" {
#endif

FlipRSDRCapture* fliprsdr_capture_alloc(FlipRSDRTransport* transport);
void fliprsdr_capture_free(FlipRSDRCapture* capture);

void fliprsdr_capture_apply_settings(FlipRSDRCapture* capture, const FlipRSDRSettings* settings);
bool fliprsdr_capture_start(FlipRSDRCapture* capture);
void fliprsdr_capture_stop(FlipRSDRCapture* capture);
void fliprsdr_capture_clear_buffered(FlipRSDRCapture* capture);
bool fliprsdr_capture_send_buffered(FlipRSDRCapture* capture);
bool fliprsdr_capture_send_debug_burst(FlipRSDRCapture* capture);
bool fliprsdr_capture_prepare_replay(
    FlipRSDRCapture* capture,
    uint32_t frequency_hz,
    bool first_level,
    uint32_t total_count);
bool fliprsdr_capture_append_replay_timings(
    FlipRSDRCapture* capture,
    uint32_t offset,
    const uint32_t* timings,
    uint16_t timing_count);
void fliprsdr_capture_cancel_replay(FlipRSDRCapture* capture);
bool fliprsdr_capture_replay(FlipRSDRCapture* capture);
void fliprsdr_capture_copy_snapshot(
    FlipRSDRCapture* capture,
    FlipRSDRCaptureSnapshot* snapshot);

#ifdef __cplusplus
}
#endif
