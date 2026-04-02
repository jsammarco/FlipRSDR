#include "../fliprsdr_app.h"

enum {
    FlipRSDRStartMenuCapture = 0,
    FlipRSDRStartMenuSettings,
    FlipRSDRStartMenuAbout,
};

static void fliprsdr_scene_start_submenu_callback(void* context, uint32_t index) {
    FlipRSDRApp* app = context;
    switch(index) {
    case FlipRSDRStartMenuCapture:
        view_dispatcher_send_custom_event(app->view_dispatcher, FlipRSDRCustomEventMenuCapture);
        break;
    case FlipRSDRStartMenuSettings:
        view_dispatcher_send_custom_event(app->view_dispatcher, FlipRSDRCustomEventMenuSettings);
        break;
    case FlipRSDRStartMenuAbout:
        view_dispatcher_send_custom_event(app->view_dispatcher, FlipRSDRCustomEventMenuAbout);
        break;
    default:
        break;
    }
}

void fliprsdr_scene_start_on_enter(void* context) {
    FlipRSDRApp* app = context;
    submenu_reset(app->submenu);
    submenu_add_item(
        app->submenu,
        "Start Capture",
        FlipRSDRStartMenuCapture,
        fliprsdr_scene_start_submenu_callback,
        app);
    submenu_add_item(
        app->submenu,
        "Settings",
        FlipRSDRStartMenuSettings,
        fliprsdr_scene_start_submenu_callback,
        app);
    submenu_add_item(
        app->submenu, "About", FlipRSDRStartMenuAbout, fliprsdr_scene_start_submenu_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, FlipRSDRViewSubmenu);
}

bool fliprsdr_scene_start_on_event(void* context, SceneManagerEvent event) {
    FlipRSDRApp* app = context;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case FlipRSDRCustomEventMenuCapture:
            scene_manager_next_scene(app->scene_manager, FlipRSDRSceneCapture);
            return true;
        case FlipRSDRCustomEventMenuSettings:
            scene_manager_next_scene(app->scene_manager, FlipRSDRSceneSettings);
            return true;
        case FlipRSDRCustomEventMenuAbout:
            scene_manager_next_scene(app->scene_manager, FlipRSDRSceneAbout);
            return true;
        default:
            break;
        }
    }

    return false;
}

void fliprsdr_scene_start_on_exit(void* context) {
    FlipRSDRApp* app = context;
    submenu_reset(app->submenu);
}
