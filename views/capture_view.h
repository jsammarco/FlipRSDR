#pragma once

#include "../app/fliprsdr.h"
#include <gui/view.h>

typedef enum {
    FlipRSDRCaptureViewActionToggle,
    FlipRSDRCaptureViewActionClear,
    FlipRSDRCaptureViewActionSend,
} FlipRSDRCaptureViewAction;

typedef void (*FlipRSDRCaptureViewActionCallback)(
    FlipRSDRCaptureViewAction action,
    void* context);

typedef struct FlipRSDRCaptureView FlipRSDRCaptureView;

FlipRSDRCaptureView* fliprsdr_capture_view_alloc(void);
void fliprsdr_capture_view_free(FlipRSDRCaptureView* capture_view);
View* fliprsdr_capture_view_get_view(FlipRSDRCaptureView* capture_view);
void fliprsdr_capture_view_set_action_callback(
    FlipRSDRCaptureView* capture_view,
    FlipRSDRCaptureViewActionCallback callback,
    void* context);
void fliprsdr_capture_view_set_snapshot(
    FlipRSDRCaptureView* capture_view,
    const FlipRSDRSettings* settings,
    const FlipRSDRTransportSnapshot* transport,
    const FlipRSDRCaptureSnapshot* capture);
