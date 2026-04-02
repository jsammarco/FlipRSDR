#include "../app/fliprsdr_app.h"

enum {
    FlipRSDRSettingsItemFrequency = 0,
    FlipRSDRSettingsItemFrequencyOffset,
    FlipRSDRSettingsItemTransport,
    FlipRSDRSettingsItemStreamMode,
    FlipRSDRSettingsItemAutoSend,
    FlipRSDRSettingsItemIncludeRssi,
    FlipRSDRSettingsItemIncludeTimestamp,
    FlipRSDRSettingsItemMaxPulses,
    FlipRSDRSettingsItemTimeout,
    FlipRSDRSettingsItemGapThreshold,
    FlipRSDRSettingsItemPreviewBand,
    FlipRSDRSettingsItemDebugSend,
};

static void fliprsdr_scene_settings_frequency_changed(VariableItem* item) {
    FlipRSDRApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    app->settings.frequency_preset = index;
    variable_item_set_current_value_text(item, fliprsdr_settings_frequency_label(index));
    app->settings_dirty = true;
}

static void fliprsdr_scene_settings_frequency_offset_changed(VariableItem* item) {
    FlipRSDRApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    char label[16];

    app->settings.frequency_offset_khz = fliprsdr_settings_frequency_offset_value(index);
    fliprsdr_settings_frequency_offset_label(
        app->settings.frequency_offset_khz, label, sizeof(label));
    variable_item_set_current_value_text(item, label);
    app->settings_dirty = true;
}

static void fliprsdr_scene_settings_transport_changed(VariableItem* item) {
    FlipRSDRApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    app->settings.transport_kind = index;
    variable_item_set_current_value_text(item, fliprsdr_settings_transport_label(index));
    app->settings_dirty = true;
    app->transport_dirty = true;
}

static void fliprsdr_scene_settings_stream_mode_changed(VariableItem* item) {
    FlipRSDRApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    app->settings.stream_mode = index;
    variable_item_set_current_value_text(item, fliprsdr_settings_stream_mode_label(index));
    app->settings_dirty = true;
}

static void fliprsdr_scene_settings_auto_send_changed(VariableItem* item) {
    FlipRSDRApp* app = variable_item_get_context(item);
    app->settings.auto_send_after_burst = variable_item_get_current_value_index(item) != 0U;
    variable_item_set_current_value_text(
        item, fliprsdr_settings_bool_label(app->settings.auto_send_after_burst));
    app->settings_dirty = true;
}

static void fliprsdr_scene_settings_include_rssi_changed(VariableItem* item) {
    FlipRSDRApp* app = variable_item_get_context(item);
    app->settings.include_rssi = variable_item_get_current_value_index(item) != 0U;
    variable_item_set_current_value_text(
        item, fliprsdr_settings_bool_label(app->settings.include_rssi));
    app->settings_dirty = true;
}

static void fliprsdr_scene_settings_include_timestamp_changed(VariableItem* item) {
    FlipRSDRApp* app = variable_item_get_context(item);
    app->settings.include_timestamp = variable_item_get_current_value_index(item) != 0U;
    variable_item_set_current_value_text(
        item, fliprsdr_settings_bool_label(app->settings.include_timestamp));
    app->settings_dirty = true;
}

static void fliprsdr_scene_settings_max_pulses_changed(VariableItem* item) {
    FlipRSDRApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    app->settings.max_pulse_count = fliprsdr_settings_max_pulse_count_value(index);
    variable_item_set_current_value_text(item, fliprsdr_settings_max_pulse_count_label(index));
    app->settings_dirty = true;
}

static void fliprsdr_scene_settings_timeout_changed(VariableItem* item) {
    FlipRSDRApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    app->settings.capture_timeout_ms = fliprsdr_settings_capture_timeout_value(index);
    variable_item_set_current_value_text(item, fliprsdr_settings_capture_timeout_label(index));
    app->settings_dirty = true;
}

static void fliprsdr_scene_settings_gap_threshold_changed(VariableItem* item) {
    FlipRSDRApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    app->settings.gap_threshold_ms = fliprsdr_settings_gap_threshold_value(index);
    variable_item_set_current_value_text(item, fliprsdr_settings_gap_threshold_label(index));
    app->settings_dirty = true;
}

static void fliprsdr_scene_settings_preview_bandwidth_changed(VariableItem* item) {
    FlipRSDRApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    app->settings.preview_bandwidth_khz = fliprsdr_settings_preview_bandwidth_value(index);
    variable_item_set_current_value_text(item, fliprsdr_settings_preview_bandwidth_label(index));
    app->settings_dirty = true;
}

static void fliprsdr_scene_settings_enter_callback(void* context, uint32_t index) {
    FlipRSDRApp* app = context;
    if(index == FlipRSDRSettingsItemDebugSend) {
        fliprsdr_capture_send_debug_burst(app->capture);
    }
}

void fliprsdr_scene_settings_on_enter(void* context) {
    FlipRSDRApp* app = context;
    VariableItem* item = NULL;
    app->settings_dirty = false;
    app->transport_dirty = false;
    variable_item_list_reset(app->variable_item_list);
    variable_item_list_set_enter_callback(
        app->variable_item_list, fliprsdr_scene_settings_enter_callback, app);

    item = variable_item_list_add(
        app->variable_item_list,
        "Freq",
        FlipRSDRFrequencyPresetCount,
        fliprsdr_scene_settings_frequency_changed,
        app);
    variable_item_set_current_value_index(item, app->settings.frequency_preset);
    variable_item_set_current_value_text(
        item, fliprsdr_settings_frequency_label(app->settings.frequency_preset));

    item = variable_item_list_add(
        app->variable_item_list,
        "Tune",
        fliprsdr_settings_frequency_offset_options_count(),
        fliprsdr_scene_settings_frequency_offset_changed,
        app);
    variable_item_set_current_value_index(
        item, fliprsdr_settings_frequency_offset_index(app->settings.frequency_offset_khz));
    {
        char label[16];
        fliprsdr_settings_frequency_offset_label(
            app->settings.frequency_offset_khz, label, sizeof(label));
        variable_item_set_current_value_text(item, label);
    }

    item = variable_item_list_add(
        app->variable_item_list,
        "Transport",
        FlipRSDRTransportKindCount,
        fliprsdr_scene_settings_transport_changed,
        app);
    variable_item_set_current_value_index(item, app->settings.transport_kind);
    variable_item_set_current_value_text(
        item, fliprsdr_settings_transport_label(app->settings.transport_kind));

    item = variable_item_list_add(
        app->variable_item_list,
        "Streaming",
        FlipRSDRStreamModeCount,
        fliprsdr_scene_settings_stream_mode_changed,
        app);
    variable_item_set_current_value_index(item, app->settings.stream_mode);
    variable_item_set_current_value_text(
        item, fliprsdr_settings_stream_mode_label(app->settings.stream_mode));

    item = variable_item_list_add(
        app->variable_item_list, "Auto send", 2, fliprsdr_scene_settings_auto_send_changed, app);
    variable_item_set_current_value_index(item, app->settings.auto_send_after_burst ? 1U : 0U);
    variable_item_set_current_value_text(
        item, fliprsdr_settings_bool_label(app->settings.auto_send_after_burst));

    item = variable_item_list_add(
        app->variable_item_list,
        "Include RSSI",
        2,
        fliprsdr_scene_settings_include_rssi_changed,
        app);
    variable_item_set_current_value_index(item, app->settings.include_rssi ? 1U : 0U);
    variable_item_set_current_value_text(
        item, fliprsdr_settings_bool_label(app->settings.include_rssi));

    item = variable_item_list_add(
        app->variable_item_list,
        "Timestamp",
        2,
        fliprsdr_scene_settings_include_timestamp_changed,
        app);
    variable_item_set_current_value_index(item, app->settings.include_timestamp ? 1U : 0U);
    variable_item_set_current_value_text(
        item, fliprsdr_settings_bool_label(app->settings.include_timestamp));

    item = variable_item_list_add(
        app->variable_item_list,
        "Max pulses",
        fliprsdr_settings_max_pulse_count_options_count(),
        fliprsdr_scene_settings_max_pulses_changed,
        app);
    variable_item_set_current_value_index(
        item, fliprsdr_settings_max_pulse_count_index(app->settings.max_pulse_count));
    variable_item_set_current_value_text(
        item,
        fliprsdr_settings_max_pulse_count_label(
            fliprsdr_settings_max_pulse_count_index(app->settings.max_pulse_count)));

    item = variable_item_list_add(
        app->variable_item_list,
        "Timeout",
        fliprsdr_settings_capture_timeout_options_count(),
        fliprsdr_scene_settings_timeout_changed,
        app);
    variable_item_set_current_value_index(
        item, fliprsdr_settings_capture_timeout_index(app->settings.capture_timeout_ms));
    variable_item_set_current_value_text(
        item,
        fliprsdr_settings_capture_timeout_label(
            fliprsdr_settings_capture_timeout_index(app->settings.capture_timeout_ms)));

    item = variable_item_list_add(
        app->variable_item_list,
        "Burst gap",
        fliprsdr_settings_gap_threshold_options_count(),
        fliprsdr_scene_settings_gap_threshold_changed,
        app);
    variable_item_set_current_value_index(
        item, fliprsdr_settings_gap_threshold_index(app->settings.gap_threshold_ms));
    variable_item_set_current_value_text(
        item,
        fliprsdr_settings_gap_threshold_label(
            fliprsdr_settings_gap_threshold_index(app->settings.gap_threshold_ms)));

    item = variable_item_list_add(
        app->variable_item_list,
        "Band span",
        fliprsdr_settings_preview_bandwidth_options_count(),
        fliprsdr_scene_settings_preview_bandwidth_changed,
        app);
    variable_item_set_current_value_index(
        item,
        fliprsdr_settings_preview_bandwidth_index(app->settings.preview_bandwidth_khz));
    variable_item_set_current_value_text(
        item,
        fliprsdr_settings_preview_bandwidth_label(
            fliprsdr_settings_preview_bandwidth_index(app->settings.preview_bandwidth_khz)));

    item = variable_item_list_add(app->variable_item_list, "Debug send", 1, NULL, app);
    variable_item_set_current_value_index(item, 0U);
    variable_item_set_current_value_text(item, "Run");

    view_dispatcher_switch_to_view(app->view_dispatcher, FlipRSDRViewSettings);
}

bool fliprsdr_scene_settings_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void fliprsdr_scene_settings_on_exit(void* context) {
    FlipRSDRApp* app = context;
    if(app->settings_dirty) {
        fliprsdr_app_apply_settings(app, app->transport_dirty);
    }
    app->settings_dirty = false;
    app->transport_dirty = false;
    variable_item_list_reset(app->variable_item_list);
}
