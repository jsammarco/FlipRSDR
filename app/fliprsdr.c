#include "fliprsdr_app.h"

static bool fliprsdr_custom_event_callback(void* context, uint32_t event) {
    FlipRSDRApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool fliprsdr_back_event_callback(void* context) {
    FlipRSDRApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void fliprsdr_tick_event_callback(void* context) {
    FlipRSDRApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static void fliprsdr_capture_view_action_callback(
    FlipRSDRCaptureViewAction action,
    void* context) {
    FlipRSDRApp* app = context;
    uint32_t event = 0U;

    switch(action) {
    case FlipRSDRCaptureViewActionToggle:
        event = FlipRSDRCustomEventCaptureToggle;
        break;
    case FlipRSDRCaptureViewActionClear:
        event = FlipRSDRCustomEventCaptureClear;
        break;
    case FlipRSDRCaptureViewActionSend:
        event = FlipRSDRCustomEventCaptureSend;
        break;
    default:
        break;
    }

    if(event) {
        view_dispatcher_send_custom_event(app->view_dispatcher, event);
    }
}

void fliprsdr_app_apply_settings(FlipRSDRApp* app, bool reinit_transport) {
    furi_assert(app);
    fliprsdr_settings_validate(&app->settings);
    fliprsdr_settings_save(&app->settings);
    if(reinit_transport) {
        fliprsdr_transport_apply_settings(app->transport, &app->settings);
    }
    fliprsdr_capture_apply_settings(app->capture, &app->settings);
    fliprsdr_app_refresh_capture_view(app);
}

void fliprsdr_app_refresh_capture_view(FlipRSDRApp* app) {
    furi_assert(app);
    FlipRSDRCaptureSnapshot capture_snapshot;
    FlipRSDRTransportSnapshot transport_snapshot;
    fliprsdr_capture_copy_snapshot(app->capture, &capture_snapshot);
    fliprsdr_transport_copy_snapshot(app->transport, &transport_snapshot);
    fliprsdr_capture_view_set_snapshot(
        app->capture_view, &app->settings, &transport_snapshot, &capture_snapshot);
}

static FlipRSDRApp* fliprsdr_app_alloc(void) {
    FlipRSDRApp* app = malloc(sizeof(FlipRSDRApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&fliprsdr_scene_handlers, app);
    app->submenu = submenu_alloc();
    app->variable_item_list = variable_item_list_alloc();
    app->about_widget = widget_alloc();
    app->capture_view = fliprsdr_capture_view_alloc();
    app->preview_view = fliprsdr_preview_view_alloc();
    app->transport = fliprsdr_transport_alloc();
    app->capture = fliprsdr_capture_alloc(app->transport);
    app->settings_dirty = false;
    app->transport_dirty = false;

    fliprsdr_settings_load(&app->settings);
    fliprsdr_app_apply_settings(app, true);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, fliprsdr_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, fliprsdr_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, fliprsdr_tick_event_callback, 100U);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    view_dispatcher_add_view(
        app->view_dispatcher, FlipRSDRViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_add_view(
        app->view_dispatcher,
        FlipRSDRViewSettings,
        variable_item_list_get_view(app->variable_item_list));
    view_dispatcher_add_view(
        app->view_dispatcher, FlipRSDRViewAbout, widget_get_view(app->about_widget));
    view_dispatcher_add_view(
        app->view_dispatcher,
        FlipRSDRViewCapture,
        fliprsdr_capture_view_get_view(app->capture_view));
    view_dispatcher_add_view(
        app->view_dispatcher,
        FlipRSDRViewPreview,
        fliprsdr_preview_view_get_view(app->preview_view));

    fliprsdr_capture_view_set_action_callback(
        app->capture_view, fliprsdr_capture_view_action_callback, app);

    return app;
}

static void fliprsdr_app_free(FlipRSDRApp* app) {
    furi_assert(app);
    fliprsdr_capture_stop(app->capture);

    view_dispatcher_remove_view(app->view_dispatcher, FlipRSDRViewCapture);
    view_dispatcher_remove_view(app->view_dispatcher, FlipRSDRViewPreview);
    view_dispatcher_remove_view(app->view_dispatcher, FlipRSDRViewAbout);
    view_dispatcher_remove_view(app->view_dispatcher, FlipRSDRViewSettings);
    view_dispatcher_remove_view(app->view_dispatcher, FlipRSDRViewSubmenu);

    fliprsdr_capture_view_free(app->capture_view);
    fliprsdr_preview_view_free(app->preview_view);
    widget_free(app->about_widget);
    variable_item_list_free(app->variable_item_list);
    submenu_free(app->submenu);
    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);
    fliprsdr_capture_free(app->capture);
    fliprsdr_transport_free(app->transport);

    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t fliprsdr_app(void* p) {
    UNUSED(p);

    FlipRSDRApp* app = fliprsdr_app_alloc();
    scene_manager_next_scene(app->scene_manager, FlipRSDRSceneStart);
    view_dispatcher_run(app->view_dispatcher);
    fliprsdr_app_free(app);
    return 0;
}
