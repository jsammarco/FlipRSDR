#include "../fliprsdr_app.h"

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
        "Remote RF pulse capture for Flipper Zero.\n\n"
        "Captures raw demodulated pulse/gap timings with the async Sub-GHz receive API and streams"
        " them over USB CDC or BLE serial.\n\n"
        "Controls:\n"
        "OK start/stop\n"
        "Left clear buffered burst\n"
        "Right send buffered burst\n\n"
        "Designed for PC-side waveform reconstruction, replay, and analysis.");
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
