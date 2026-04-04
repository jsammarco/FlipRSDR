#include "../app/fliprsdr_app.h"

enum {
    FlipRSDRSettingsItemPreset = 0,
    FlipRSDRSettingsItemFrequency,
    FlipRSDRSettingsItemTransport,
    FlipRSDRSettingsItemProtocol,
    FlipRSDRSettingsItemStreamMode,
    FlipRSDRSettingsItemAutoSend,
    FlipRSDRSettingsItemIncludeRssi,
    FlipRSDRSettingsItemIncludeTimestamp,
    FlipRSDRSettingsItemMaxPulses,
    FlipRSDRSettingsItemTimeout,
    FlipRSDRSettingsItemGapThreshold,
    FlipRSDRSettingsItemRssiThreshold,
    FlipRSDRSettingsItemPreviewBand,
};

static void fliprsdr_scene_settings_frequency_preset_changed(VariableItem* item) {
    FlipRSDRApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);

    if(index == 0U) {
        variable_item_set_current_value_text(item, "Custom");
        return;
    }

    app->settings.frequency_hz = fliprsdr_settings_frequency_value(index - 1U);
    variable_item_set_current_value_text(item, fliprsdr_settings_frequency_label(index - 1U));
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

static void fliprsdr_scene_settings_protocol_changed(VariableItem* item) {
    FlipRSDRApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    app->settings.protocol_format = index;
    variable_item_set_current_value_text(item, fliprsdr_settings_protocol_label(index));
    app->settings_dirty = true;
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

static void fliprsdr_scene_settings_rssi_threshold_changed(VariableItem* item) {
    FlipRSDRApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    app->settings.rssi_threshold_dbm = fliprsdr_settings_rssi_threshold_value(index);
    variable_item_set_current_value_text(item, fliprsdr_settings_rssi_threshold_label(index));
    app->settings_dirty = true;
}

static void fliprsdr_scene_settings_enter_callback(void* context, uint32_t index) {
    FlipRSDRApp* app = context;
    scene_manager_set_scene_state(app->scene_manager, FlipRSDRSceneSettings, index);

    if(index == FlipRSDRSettingsItemFrequency) {
        scene_manager_next_scene(app->scene_manager, FlipRSDRSceneFrequencyInput);
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
        "Preset",
        FlipRSDRFrequencyPresetCount + 1U,
        fliprsdr_scene_settings_frequency_preset_changed,
        app);
    {
        uint8_t preset_index = 0U;
        for(uint8_t i = 0; i < FlipRSDRFrequencyPresetCount; i++) {
            if(app->settings.frequency_hz == fliprsdr_settings_frequency_value(i)) {
                preset_index = i + 1U;
                break;
            }
        }

        variable_item_set_current_value_index(item, preset_index);
        variable_item_set_current_value_text(
            item,
            preset_index == 0U ? "Custom" :
                                 fliprsdr_settings_frequency_label(preset_index - 1U));
    }

    item = variable_item_list_add(app->variable_item_list, "Freq", 1, NULL, app);
    variable_item_set_current_value_index(item, 0U);
    {
        char label[16];
        fliprsdr_settings_frequency_text(app->settings.frequency_hz, label, sizeof(label));
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
        "Protocol",
        FlipRSDRProtocolFormatCount,
        fliprsdr_scene_settings_protocol_changed,
        app);
    variable_item_set_current_value_index(item, app->settings.protocol_format);
    variable_item_set_current_value_text(
        item, fliprsdr_settings_protocol_label(app->settings.protocol_format));

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
        "RSSI min",
        fliprsdr_settings_rssi_threshold_options_count(),
        fliprsdr_scene_settings_rssi_threshold_changed,
        app);
    variable_item_set_current_value_index(
        item, fliprsdr_settings_rssi_threshold_index(app->settings.rssi_threshold_dbm));
    variable_item_set_current_value_text(
        item,
        fliprsdr_settings_rssi_threshold_label(
            fliprsdr_settings_rssi_threshold_index(app->settings.rssi_threshold_dbm)));

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

    variable_item_list_set_selected_item(
        app->variable_item_list,
        scene_manager_get_scene_state(app->scene_manager, FlipRSDRSceneSettings));
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
