#include "../fliprsdr_app.h"

void fliprsdr_scene_capture_on_enter(void* context) {
    FlipRSDRApp* app = context;
    fliprsdr_app_refresh_capture_view(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, FlipRSDRViewCapture);
}

bool fliprsdr_scene_capture_on_event(void* context, SceneManagerEvent event) {
    FlipRSDRApp* app = context;

    if(event.type == SceneManagerEventTypeTick) {
        fliprsdr_app_refresh_capture_view(app);
        return true;
    }

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case FlipRSDRCustomEventCaptureToggle: {
            FlipRSDRCaptureSnapshot snapshot;
            fliprsdr_capture_copy_snapshot(app->capture, &snapshot);
            if(snapshot.running) {
                fliprsdr_capture_stop(app->capture);
            } else {
                fliprsdr_capture_start(app->capture);
            }
            fliprsdr_app_refresh_capture_view(app);
            return true;
        }
        case FlipRSDRCustomEventCaptureClear:
            fliprsdr_capture_clear_buffered(app->capture);
            fliprsdr_app_refresh_capture_view(app);
            return true;
        case FlipRSDRCustomEventCaptureSend:
            fliprsdr_capture_send_buffered(app->capture);
            fliprsdr_app_refresh_capture_view(app);
            return true;
        default:
            break;
        }
    }

    return false;
}

void fliprsdr_scene_capture_on_exit(void* context) {
    FlipRSDRApp* app = context;
    fliprsdr_capture_stop(app->capture);
    fliprsdr_app_refresh_capture_view(app);
}
