#include "fliprsdr_app.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool fliprsdr_app_command_is_word(const char* value, const char* word) {
    while(*value && *word) {
        if(tolower((unsigned char)*value) != tolower((unsigned char)*word)) {
            return false;
        }
        value++;
        word++;
    }
    return (*value == '\0') && (*word == '\0');
}

static const char* fliprsdr_app_command_skip_delimiters(const char* text) {
    while(*text == ' ' || *text == '\t' || *text == '=') {
        text++;
    }
    return text;
}

static bool fliprsdr_app_parse_u32(const char* text, uint32_t* value) {
    char* end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if(!text[0] || !end || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool fliprsdr_app_parse_bool01(const char* text, bool* value) {
    uint32_t parsed = 0U;
    if(!fliprsdr_app_parse_u32(text, &parsed) || parsed > 1U) {
        return false;
    }
    *value = parsed != 0U;
    return true;
}

static bool fliprsdr_app_parse_replay_begin(FlipRSDRApp* app, char* argument) {
    char* frequency_text = argument;
    char* first_level_text = NULL;
    char* count_text = NULL;

    while(*argument && *argument != ' ' && *argument != '\t' && *argument != '=') {
        argument++;
    }
    if(*argument) {
        *argument++ = '\0';
        first_level_text = argument = (char*)fliprsdr_app_command_skip_delimiters(argument);
    }

    while(*argument && *argument != ' ' && *argument != '\t' && *argument != '=') {
        argument++;
    }
    if(*argument) {
        *argument++ = '\0';
        count_text = (char*)fliprsdr_app_command_skip_delimiters(argument);
    }

    if(!frequency_text[0] || !first_level_text || !count_text || !count_text[0]) {
        return false;
    }

    uint32_t frequency_hz = 0U;
    uint32_t total_count = 0U;
    bool first_level = false;
    if(!fliprsdr_app_parse_u32(frequency_text, &frequency_hz) ||
       !fliprsdr_app_parse_bool01(first_level_text, &first_level) ||
       !fliprsdr_app_parse_u32(count_text, &total_count)) {
        return false;
    }

    return fliprsdr_capture_prepare_replay(app->capture, frequency_hz, first_level, total_count);
}

static bool fliprsdr_app_parse_replay_chunk(FlipRSDRApp* app, char* argument) {
    uint32_t timings[96];
    uint16_t timing_count = 0U;
    char* offset_text = argument;
    char* timings_text = NULL;

    while(*argument && *argument != ' ' && *argument != '\t' && *argument != '=') {
        argument++;
    }
    if(*argument) {
        *argument++ = '\0';
        timings_text = (char*)fliprsdr_app_command_skip_delimiters(argument);
    }

    if(!offset_text[0] || !timings_text || !timings_text[0]) {
        return false;
    }

    uint32_t offset = 0U;
    if(!fliprsdr_app_parse_u32(offset_text, &offset)) {
        return false;
    }

    char* cursor = timings_text;
    while(*cursor) {
        char* next = cursor;
        while(*next && *next != ',') {
            next++;
        }
        if(*next == ',') {
            *next++ = '\0';
        }
        if(timing_count >= COUNT_OF(timings) || !fliprsdr_app_parse_u32(cursor, &timings[timing_count])) {
            return false;
        }
        timing_count++;
        cursor = next;
    }

    return fliprsdr_capture_append_replay_timings(app->capture, offset, timings, timing_count);
}

static bool fliprsdr_app_handle_command_line(FlipRSDRApp* app, const char* line) {
    char command[FLIPRSDR_COMMAND_LINE_MAX];
    strncpy(command, line, sizeof(command) - 1U);
    command[sizeof(command) - 1U] = '\0';

    char* argument = command;
    while(*argument && *argument != ' ' && *argument != '\t' && *argument != '=') {
        argument++;
    }
    if(*argument) {
        *argument++ = '\0';
        argument = (char*)fliprsdr_app_command_skip_delimiters(argument);
    }

    if(fliprsdr_app_command_is_word(command, "start_scan")) {
        fliprsdr_capture_start(app->capture);
        fliprsdr_app_refresh_capture_view(app);
        return true;
    }

    if(fliprsdr_app_command_is_word(command, "stop_scan")) {
        fliprsdr_capture_stop(app->capture);
        fliprsdr_app_refresh_capture_view(app);
        return true;
    }

    if(fliprsdr_app_command_is_word(command, "set_frequency")) {
        uint32_t frequency_hz = 0U;
        if(!argument[0] ||
           !fliprsdr_settings_parse_frequency_text(argument, &frequency_hz) ||
           !fliprsdr_settings_set_frequency_hz(&app->settings, frequency_hz)) {
            return false;
        }

        FlipRSDRCaptureSnapshot snapshot;
        fliprsdr_capture_copy_snapshot(app->capture, &snapshot);
        if(snapshot.running) {
            fliprsdr_capture_stop(app->capture);
        }
        fliprsdr_app_apply_settings(app, false);
        if(snapshot.running) {
            fliprsdr_capture_start(app->capture);
        }
        fliprsdr_app_refresh_capture_view(app);
        return true;
    }

    if(fliprsdr_app_command_is_word(command, "set_rssi_threshold")) {
        if(!argument[0]) {
            return false;
        }

        if(fliprsdr_app_command_is_word(argument, "off")) {
            app->settings.rssi_threshold_dbm = FLIPRSDR_RSSI_THRESHOLD_OFF;
        } else {
            int32_t threshold = 0;
            bool negative = false;
            if(*argument == '-') {
                negative = true;
                argument++;
            }
            if(!isdigit((unsigned char)*argument)) {
                return false;
            }
            while(isdigit((unsigned char)*argument)) {
                threshold = (threshold * 10) + (*argument - '0');
                argument++;
            }
            if(*argument != '\0') {
                return false;
            }
            if(!negative) {
                return false;
            }
            app->settings.rssi_threshold_dbm = (int16_t)(-threshold);
        }

        fliprsdr_settings_validate(&app->settings);
        fliprsdr_app_apply_settings(app, false);
        return true;
    }

    if(fliprsdr_app_command_is_word(command, "replay_begin")) {
        return argument[0] ? fliprsdr_app_parse_replay_begin(app, argument) : false;
    }

    if(fliprsdr_app_command_is_word(command, "replay_chunk")) {
        return argument[0] ? fliprsdr_app_parse_replay_chunk(app, argument) : false;
    }

    if(fliprsdr_app_command_is_word(command, "replay_commit")) {
        const bool ok = fliprsdr_capture_replay(app->capture);
        fliprsdr_app_refresh_capture_view(app);
        return ok;
    }

    if(fliprsdr_app_command_is_word(command, "replay_cancel")) {
        fliprsdr_capture_cancel_replay(app->capture);
        return true;
    }

    if(fliprsdr_app_command_is_word(command, "replay_wait")) {
        uint32_t wait_ms = 0U;
        if(!argument[0] || !fliprsdr_app_parse_u32(argument, &wait_ms)) {
            return false;
        }
        furi_delay_ms(wait_ms);
        return true;
    }

    return false;
}

static void fliprsdr_app_handle_pending_commands(FlipRSDRApp* app) {
    FlipRSDRCommandMessage message;
    while(
        furi_message_queue_get(app->command_queue, &message, 0U) == FuriStatusOk) {
        fliprsdr_app_handle_command_line(app, message.line);
    }
}

static void fliprsdr_transport_command_callback(const char* line, void* context) {
    FlipRSDRApp* app = context;
    FlipRSDRCommandMessage message = {0};
    strncpy(message.line, line, sizeof(message.line) - 1U);
    furi_message_queue_put(app->command_queue, &message, 0U);
}

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
    fliprsdr_app_handle_pending_commands(app);
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

static void fliprsdr_frequency_editor_action_callback(
    FlipRSDRFrequencyEditorAction action,
    uint32_t frequency_hz,
    void* context) {
    FlipRSDRApp* app = context;

    if(action == FlipRSDRFrequencyEditorActionSave) {
        if(fliprsdr_settings_set_frequency_hz(&app->settings, frequency_hz)) {
            view_dispatcher_send_custom_event(
                app->view_dispatcher, FlipRSDRCustomEventFrequencyEditorSave);
        }
    } else {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, FlipRSDRCustomEventFrequencyEditorCancel);
    }
}

static void fliprsdr_preview_view_action_callback(
    FlipRSDRPreviewViewAction action,
    uint32_t frequency_hz,
    void* context) {
    FlipRSDRApp* app = context;

    if((action == FlipRSDRPreviewViewActionFrequencyChanged) &&
       fliprsdr_settings_set_frequency_hz(&app->settings, frequency_hz)) {
        app->settings_dirty = true;
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
    app->frequency_editor_view = fliprsdr_frequency_editor_view_alloc();
    app->preview_view = fliprsdr_preview_view_alloc();
    app->transport = fliprsdr_transport_alloc();
    app->capture = fliprsdr_capture_alloc(app->transport);
    app->settings_dirty = false;
    app->transport_dirty = false;
    app->command_queue =
        furi_message_queue_alloc(FLIPRSDR_COMMAND_QUEUE_DEPTH, sizeof(FlipRSDRCommandMessage));

    fliprsdr_settings_load(&app->settings);
    fliprsdr_app_apply_settings(app, true);
    fliprsdr_transport_set_command_callback(
        app->transport, fliprsdr_transport_command_callback, app);

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
        app->view_dispatcher,
        FlipRSDRViewFrequencyEditor,
        fliprsdr_frequency_editor_view_get_view(app->frequency_editor_view));
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
    fliprsdr_frequency_editor_view_set_action_callback(
        app->frequency_editor_view, fliprsdr_frequency_editor_action_callback, app);
    fliprsdr_preview_view_set_action_callback(
        app->preview_view, fliprsdr_preview_view_action_callback, app);

    return app;
}

static void fliprsdr_app_free(FlipRSDRApp* app) {
    furi_assert(app);
    fliprsdr_capture_stop(app->capture);

    view_dispatcher_remove_view(app->view_dispatcher, FlipRSDRViewCapture);
    view_dispatcher_remove_view(app->view_dispatcher, FlipRSDRViewPreview);
    view_dispatcher_remove_view(app->view_dispatcher, FlipRSDRViewAbout);
    view_dispatcher_remove_view(app->view_dispatcher, FlipRSDRViewFrequencyEditor);
    view_dispatcher_remove_view(app->view_dispatcher, FlipRSDRViewSettings);
    view_dispatcher_remove_view(app->view_dispatcher, FlipRSDRViewSubmenu);

    fliprsdr_capture_view_free(app->capture_view);
    fliprsdr_frequency_editor_view_free(app->frequency_editor_view);
    fliprsdr_preview_view_free(app->preview_view);
    widget_free(app->about_widget);
    variable_item_list_free(app->variable_item_list);
    submenu_free(app->submenu);
    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);
    fliprsdr_transport_set_command_callback(app->transport, NULL, NULL);
    fliprsdr_capture_free(app->capture);
    fliprsdr_transport_free(app->transport);
    furi_message_queue_free(app->command_queue);

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
