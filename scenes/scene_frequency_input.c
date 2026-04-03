#include "../app/fliprsdr_app.h"

void fliprsdr_scene_frequency_input_on_enter(void* context) {
    FlipRSDRApp* app = context;
    fliprsdr_frequency_editor_view_set_frequency(
        app->frequency_editor_view, app->settings.frequency_hz);
    view_dispatcher_switch_to_view(app->view_dispatcher, FlipRSDRViewFrequencyEditor);
}

bool fliprsdr_scene_frequency_input_on_event(void* context, SceneManagerEvent event) {
    FlipRSDRApp* app = context;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == FlipRSDRCustomEventFrequencyEditorSave) {
            fliprsdr_app_apply_settings(app, false);
            scene_manager_previous_scene(app->scene_manager);
            return true;
        }

        if(event.event == FlipRSDRCustomEventFrequencyEditorCancel) {
            scene_manager_previous_scene(app->scene_manager);
            return true;
        }
    }

    return false;
}

void fliprsdr_scene_frequency_input_on_exit(void* context) {
    UNUSED(context);
}
