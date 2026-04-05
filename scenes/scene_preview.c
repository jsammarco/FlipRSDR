#include "../app/fliprsdr_app.h"

void fliprsdr_scene_preview_on_enter(void* context) {
    FlipRSDRApp* app = context;
    fliprsdr_preview_view_set_settings(app->preview_view, &app->settings);
    view_dispatcher_switch_to_view(app->view_dispatcher, FlipRSDRViewPreview);
}

bool fliprsdr_scene_preview_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void fliprsdr_scene_preview_on_exit(void* context) {
    FlipRSDRApp* app = context;
    if(app->settings_dirty) {
        fliprsdr_app_apply_settings(app, false);
        app->settings_dirty = false;
    }
}
