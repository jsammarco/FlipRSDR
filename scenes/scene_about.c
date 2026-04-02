#include "../app/fliprsdr_app.h"

void fliprsdr_scene_about_on_enter(void* context) {
    FlipRSDRApp* app = context;
    widget_reset(app->about_widget);
    widget_add_text_scroll_element(
        app->about_widget,
        0,
        0,
        128,
        64,
        "FlipRSDR\n\n"
        "Sub-GHz pulse capture and preview for Flipper Zero.\n\n"
        "Created by ConsultingJoe.com\n\n"
        "Use Signal Preview to scan the current band, then Start Capture to stream bursts to your"
        " PC.");
    view_dispatcher_switch_to_view(app->view_dispatcher, FlipRSDRViewAbout);
}

bool fliprsdr_scene_about_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void fliprsdr_scene_about_on_exit(void* context) {
    FlipRSDRApp* app = context;
    widget_reset(app->about_widget);
}
