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
void fliprsdr_capture_copy_snapshot(
    FlipRSDRCapture* capture,
    FlipRSDRCaptureSnapshot* snapshot);

#ifdef __cplusplus
}
#endif
