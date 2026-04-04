#include "settings.h"

#include <furi_hal.h>
#include <lib/toolbox/strint.h>
#include <stdio.h>
#include <string.h>
#include <toolbox/saved_struct.h>

typedef struct {
    FlipRSDRSettings settings;
} FlipRSDRSettingsStore;

typedef struct {
    uint8_t frequency_preset;
    uint8_t transport_kind;
    uint8_t stream_mode;
    int16_t frequency_offset_khz;
    bool auto_send_after_burst;
    bool include_rssi;
    bool include_timestamp;
    uint16_t max_pulse_count;
    uint16_t capture_timeout_ms;
    uint16_t gap_threshold_ms;
    uint16_t preview_bandwidth_khz;
} FlipRSDRSettingsV3;

typedef struct {
    FlipRSDRSettingsV3 settings;
} FlipRSDRSettingsStoreV3;

typedef struct {
    uint32_t frequency_hz;
    uint8_t transport_kind;
    uint8_t stream_mode;
    bool auto_send_after_burst;
    bool include_rssi;
    bool include_timestamp;
    uint16_t max_pulse_count;
    uint16_t capture_timeout_ms;
    uint16_t gap_threshold_ms;
    uint16_t preview_bandwidth_khz;
} FlipRSDRSettingsV4;

typedef struct {
    FlipRSDRSettingsV4 settings;
} FlipRSDRSettingsStoreV4;

static const uint32_t fliprsdr_frequency_values[FlipRSDRFrequencyPresetCount] = {
    300000000UL,
    315000000UL,
    390000000UL,
    433920000UL,
    868350000UL,
    915000000UL,
};

static const char* fliprsdr_frequency_labels[FlipRSDRFrequencyPresetCount] = {
    "300M",
    "315M",
    "390M",
    "433.92M",
    "868.35M",
    "915M",
};

static const char* fliprsdr_transport_labels[FlipRSDRTransportKindCount] = {
    "USB",
    "Bluetooth",
};

static const char* fliprsdr_protocol_labels[FlipRSDRProtocolFormatCount] = {
    "fliprsdr",
    "JSON",
};

static const char* fliprsdr_stream_mode_labels[FlipRSDRStreamModeCount] = {
    "Live stream",
    "Buffered",
    "Live + buffer",
};

static const uint16_t fliprsdr_max_pulse_options[] = {256, 512, 1024, 2048, 4096};
static const char* fliprsdr_max_pulse_labels[] = {"256", "512", "1024", "2048", "4096"};

static const uint16_t fliprsdr_capture_timeout_options[] = {100, 250, 500, 1000, 2000, 5000};
static const char* fliprsdr_capture_timeout_labels[] = {
    "100 ms",
    "250 ms",
    "500 ms",
    "1000 ms",
    "2000 ms",
    "5000 ms",
};

static const uint16_t fliprsdr_gap_threshold_options[] = {5, 10, 20, 50, 100, 200};
static const char* fliprsdr_gap_threshold_labels[] = {
    "5 ms",
    "10 ms",
    "20 ms",
    "50 ms",
    "100 ms",
    "200 ms",
};

static const uint16_t fliprsdr_preview_bandwidth_options[] = {50, 100, 200, 400};
static const char* fliprsdr_preview_bandwidth_labels[] = {
    "50 kHz",
    "100 kHz",
    "200 kHz",
    "400 kHz",
};

static bool fliprsdr_settings_load_v3(FlipRSDRSettings* settings) {
    FlipRSDRSettingsStoreV3 store = {0};

    if(!saved_struct_load(
           FLIPRSDR_SETTINGS_PATH, &store, sizeof(store), FLIPRSDR_SETTINGS_MAGIC, 3U)) {
        return false;
    }

    settings->frequency_hz =
        fliprsdr_settings_frequency_value(store.settings.frequency_preset) +
        ((int32_t)store.settings.frequency_offset_khz * 1000L);
    settings->transport_kind = store.settings.transport_kind;
    settings->protocol_format = FlipRSDRProtocolFormatFlipRSDR;
    settings->stream_mode = store.settings.stream_mode;
    settings->auto_send_after_burst = store.settings.auto_send_after_burst;
    settings->include_rssi = store.settings.include_rssi;
    settings->include_timestamp = store.settings.include_timestamp;
    settings->max_pulse_count = store.settings.max_pulse_count;
    settings->capture_timeout_ms = store.settings.capture_timeout_ms;
    settings->gap_threshold_ms = store.settings.gap_threshold_ms;
    settings->preview_bandwidth_khz = store.settings.preview_bandwidth_khz;
    return true;
}

static bool fliprsdr_settings_load_v4(FlipRSDRSettings* settings) {
    FlipRSDRSettingsStoreV4 store = {0};

    if(!saved_struct_load(
           FLIPRSDR_SETTINGS_PATH, &store, sizeof(store), FLIPRSDR_SETTINGS_MAGIC, 4U)) {
        return false;
    }

    settings->frequency_hz = store.settings.frequency_hz;
    settings->transport_kind = store.settings.transport_kind;
    settings->protocol_format = FlipRSDRProtocolFormatFlipRSDR;
    settings->stream_mode = store.settings.stream_mode;
    settings->auto_send_after_burst = store.settings.auto_send_after_burst;
    settings->include_rssi = store.settings.include_rssi;
    settings->include_timestamp = store.settings.include_timestamp;
    settings->max_pulse_count = store.settings.max_pulse_count;
    settings->capture_timeout_ms = store.settings.capture_timeout_ms;
    settings->gap_threshold_ms = store.settings.gap_threshold_ms;
    settings->preview_bandwidth_khz = store.settings.preview_bandwidth_khz;
    return true;
}

void fliprsdr_settings_load_defaults(FlipRSDRSettings* settings) {
    furi_assert(settings);
    settings->frequency_hz = 433920000UL;
    settings->transport_kind = FlipRSDRTransportKindUsb;
    settings->protocol_format = FlipRSDRProtocolFormatFlipRSDR;
    settings->stream_mode = FlipRSDRStreamModeBuffered;
    settings->auto_send_after_burst = false;
    settings->include_rssi = true;
    settings->include_timestamp = true;
    settings->max_pulse_count = 2048;
    settings->capture_timeout_ms = 250;
    settings->gap_threshold_ms = 20;
    settings->preview_bandwidth_khz = 100;
}

void fliprsdr_settings_validate(FlipRSDRSettings* settings) {
    furi_assert(settings);

    if(!furi_hal_subghz_is_frequency_valid(settings->frequency_hz)) {
        settings->frequency_hz = 433920000UL;
    }
    if(settings->transport_kind >= FlipRSDRTransportKindCount) {
        settings->transport_kind = FlipRSDRTransportKindUsb;
    }
    if(settings->protocol_format >= FlipRSDRProtocolFormatCount) {
        settings->protocol_format = FlipRSDRProtocolFormatFlipRSDR;
    }
    if(settings->stream_mode >= FlipRSDRStreamModeCount) {
        settings->stream_mode = FlipRSDRStreamModeBuffered;
    }

    settings->max_pulse_count =
        fliprsdr_settings_max_pulse_count_value(
            fliprsdr_settings_max_pulse_count_index(settings->max_pulse_count));
    settings->capture_timeout_ms =
        fliprsdr_settings_capture_timeout_value(
            fliprsdr_settings_capture_timeout_index(settings->capture_timeout_ms));
    settings->gap_threshold_ms =
        fliprsdr_settings_gap_threshold_value(
            fliprsdr_settings_gap_threshold_index(settings->gap_threshold_ms));
    settings->preview_bandwidth_khz =
        fliprsdr_settings_preview_bandwidth_value(
            fliprsdr_settings_preview_bandwidth_index(settings->preview_bandwidth_khz));

    if(settings->capture_timeout_ms < settings->gap_threshold_ms) {
        settings->capture_timeout_ms = settings->gap_threshold_ms;
    }
}

bool fliprsdr_settings_load(FlipRSDRSettings* settings) {
    furi_assert(settings);

    FlipRSDRSettingsStore store = {0};
    uint8_t version = 0U;
    size_t payload_size = 0U;

    fliprsdr_settings_load_defaults(settings);

    if(!saved_struct_get_metadata(
           FLIPRSDR_SETTINGS_PATH, NULL, &version, &payload_size)) {
        return false;
    }

    if(version == FLIPRSDR_SETTINGS_VERSION && payload_size == sizeof(store) &&
       saved_struct_load(
           FLIPRSDR_SETTINGS_PATH,
           &store,
           sizeof(store),
           FLIPRSDR_SETTINGS_MAGIC,
           FLIPRSDR_SETTINGS_VERSION)) {
        *settings = store.settings;
        fliprsdr_settings_validate(settings);
        return true;
    }

    if(version == 3U && payload_size == sizeof(FlipRSDRSettingsStoreV3) &&
       fliprsdr_settings_load_v3(settings)) {
        fliprsdr_settings_validate(settings);
        return true;
    }

    if(version == 4U && payload_size == sizeof(FlipRSDRSettingsStoreV4) &&
       fliprsdr_settings_load_v4(settings)) {
        fliprsdr_settings_validate(settings);
        return true;
    }

    return false;
}

bool fliprsdr_settings_save(const FlipRSDRSettings* settings) {
    furi_assert(settings);
    FlipRSDRSettingsStore store = {.settings = *settings};
    return saved_struct_save(
        FLIPRSDR_SETTINGS_PATH,
        &store,
        sizeof(store),
        FLIPRSDR_SETTINGS_MAGIC,
        FLIPRSDR_SETTINGS_VERSION);
}

uint32_t fliprsdr_settings_frequency_value(uint8_t preset_index) {
    if(preset_index >= FlipRSDRFrequencyPresetCount) {
        preset_index = FlipRSDRFrequencyPreset43392;
    }
    return fliprsdr_frequency_values[preset_index];
}

uint32_t fliprsdr_settings_frequency_hz(const FlipRSDRSettings* settings) {
    furi_assert(settings);
    return settings->frequency_hz;
}

bool fliprsdr_settings_set_frequency_hz(FlipRSDRSettings* settings, uint32_t frequency_hz) {
    furi_assert(settings);
    if(!furi_hal_subghz_is_frequency_valid(frequency_hz)) {
        return false;
    }

    settings->frequency_hz = frequency_hz;
    return true;
}

bool fliprsdr_settings_parse_frequency_text(const char* text, uint32_t* frequency_hz) {
    furi_assert(text);
    furi_assert(frequency_hz);

    if(text[0] == '\0') {
        return false;
    }

    const char* dot = strchr(text, '.');
    if(dot) {
        uint32_t whole_mhz = 0U;
        if(dot == text) {
            return false;
        }

        if(strint_to_uint32(text, (char**)&dot, &whole_mhz, 10) != StrintParseNoError) {
            return false;
        }

        uint32_t fractional_hz = 0U;
        uint32_t scale = 100000U;
        for(const char* cursor = dot + 1; *cursor; cursor++) {
            if(*cursor < '0' || *cursor > '9') {
                return false;
            }

            if(scale > 0U) {
                fractional_hz += (uint32_t)(*cursor - '0') * scale;
                scale /= 10U;
            }
        }

        const uint64_t total_hz = ((uint64_t)whole_mhz * 1000000ULL) + fractional_hz;
        if(total_hz > UINT32_MAX) {
            return false;
        }

        *frequency_hz = (uint32_t)total_hz;
        return true;
    }

    uint32_t value = 0U;
    if(strint_to_uint32(text, NULL, &value, 10) != StrintParseNoError) {
        return false;
    }

    if(value < 1000U) {
        value *= 1000000U;
    }

    *frequency_hz = value;
    return true;
}

void fliprsdr_settings_frequency_text(uint32_t frequency_hz, char* buffer, size_t size) {
    furi_assert(buffer);
    snprintf(
        buffer,
        size,
        "%lu.%03luM",
        (unsigned long)(frequency_hz / 1000000UL),
        (unsigned long)((frequency_hz % 1000000UL) / 1000UL));
}

const char* fliprsdr_settings_frequency_label(uint8_t preset_index) {
    if(preset_index >= FlipRSDRFrequencyPresetCount) return "?";
    return fliprsdr_frequency_labels[preset_index];
}

const char* fliprsdr_settings_transport_label(uint8_t transport_index) {
    if(transport_index >= FlipRSDRTransportKindCount) return "?";
    return fliprsdr_transport_labels[transport_index];
}

const char* fliprsdr_settings_protocol_label(uint8_t protocol_index) {
    if(protocol_index >= FlipRSDRProtocolFormatCount) return "?";
    return fliprsdr_protocol_labels[protocol_index];
}

const char* fliprsdr_settings_stream_mode_label(uint8_t mode_index) {
    if(mode_index >= FlipRSDRStreamModeCount) return "?";
    return fliprsdr_stream_mode_labels[mode_index];
}

const char* fliprsdr_settings_bool_label(bool value) {
    return value ? "On" : "Off";
}

uint8_t fliprsdr_settings_max_pulse_count_options_count(void) {
    return COUNT_OF(fliprsdr_max_pulse_options);
}

uint16_t fliprsdr_settings_max_pulse_count_value(uint8_t index) {
    if(index >= COUNT_OF(fliprsdr_max_pulse_options)) {
        index = 3U;
    }
    return fliprsdr_max_pulse_options[index];
}

const char* fliprsdr_settings_max_pulse_count_label(uint8_t index) {
    if(index >= COUNT_OF(fliprsdr_max_pulse_labels)) return "?";
    return fliprsdr_max_pulse_labels[index];
}

uint8_t fliprsdr_settings_max_pulse_count_index(uint16_t value) {
    uint8_t best_index = 0U;
    for(uint8_t i = 0; i < COUNT_OF(fliprsdr_max_pulse_options); i++) {
        if(fliprsdr_max_pulse_options[i] == value) return i;
        if(fliprsdr_max_pulse_options[i] < value) best_index = i;
    }
    return best_index;
}

uint8_t fliprsdr_settings_capture_timeout_options_count(void) {
    return COUNT_OF(fliprsdr_capture_timeout_options);
}

uint16_t fliprsdr_settings_capture_timeout_value(uint8_t index) {
    if(index >= COUNT_OF(fliprsdr_capture_timeout_options)) {
        index = 1U;
    }
    return fliprsdr_capture_timeout_options[index];
}

const char* fliprsdr_settings_capture_timeout_label(uint8_t index) {
    if(index >= COUNT_OF(fliprsdr_capture_timeout_labels)) return "?";
    return fliprsdr_capture_timeout_labels[index];
}

uint8_t fliprsdr_settings_capture_timeout_index(uint16_t value) {
    uint8_t best_index = 0U;
    for(uint8_t i = 0; i < COUNT_OF(fliprsdr_capture_timeout_options); i++) {
        if(fliprsdr_capture_timeout_options[i] == value) return i;
        if(fliprsdr_capture_timeout_options[i] < value) best_index = i;
    }
    return best_index;
}

uint8_t fliprsdr_settings_gap_threshold_options_count(void) {
    return COUNT_OF(fliprsdr_gap_threshold_options);
}

uint16_t fliprsdr_settings_gap_threshold_value(uint8_t index) {
    if(index >= COUNT_OF(fliprsdr_gap_threshold_options)) {
        index = 2U;
    }
    return fliprsdr_gap_threshold_options[index];
}

const char* fliprsdr_settings_gap_threshold_label(uint8_t index) {
    if(index >= COUNT_OF(fliprsdr_gap_threshold_labels)) return "?";
    return fliprsdr_gap_threshold_labels[index];
}

uint8_t fliprsdr_settings_gap_threshold_index(uint16_t value) {
    uint8_t best_index = 0U;
    for(uint8_t i = 0; i < COUNT_OF(fliprsdr_gap_threshold_options); i++) {
        if(fliprsdr_gap_threshold_options[i] == value) return i;
        if(fliprsdr_gap_threshold_options[i] < value) best_index = i;
    }
    return best_index;
}

uint8_t fliprsdr_settings_preview_bandwidth_options_count(void) {
    return COUNT_OF(fliprsdr_preview_bandwidth_options);
}

uint16_t fliprsdr_settings_preview_bandwidth_value(uint8_t index) {
    if(index >= COUNT_OF(fliprsdr_preview_bandwidth_options)) {
        index = 1U;
    }
    return fliprsdr_preview_bandwidth_options[index];
}

const char* fliprsdr_settings_preview_bandwidth_label(uint8_t index) {
    if(index >= COUNT_OF(fliprsdr_preview_bandwidth_labels)) return "?";
    return fliprsdr_preview_bandwidth_labels[index];
}

uint8_t fliprsdr_settings_preview_bandwidth_index(uint16_t value) {
    uint8_t best_index = 0U;
    for(uint8_t i = 0; i < COUNT_OF(fliprsdr_preview_bandwidth_options); i++) {
        if(fliprsdr_preview_bandwidth_options[i] == value) return i;
        if(fliprsdr_preview_bandwidth_options[i] < value) best_index = i;
    }
    return best_index;
}
