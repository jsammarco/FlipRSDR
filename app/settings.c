#include "settings.h"

#include <toolbox/saved_struct.h>
#include <stdio.h>

typedef struct {
    FlipRSDRSettings settings;
} FlipRSDRSettingsStore;

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

static const char* fliprsdr_stream_mode_labels[FlipRSDRStreamModeCount] = {
    "Live stream",
    "Buffered",
    "Live + buffer",
};

static const uint16_t fliprsdr_max_pulse_options[] = {256, 512, 1024, 2048, 4096, 8192};
static const char* fliprsdr_max_pulse_labels[] = {"256", "512", "1024", "2048", "4096", "8192"};

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

#define FLIPRSDR_FREQUENCY_OFFSET_OPTIONS_COUNT \
    (((FLIPRSDR_FREQUENCY_OFFSET_MAX_KHZ - FLIPRSDR_FREQUENCY_OFFSET_MIN_KHZ) / \
      FLIPRSDR_FREQUENCY_OFFSET_STEP_KHZ) + \
     1)

void fliprsdr_settings_load_defaults(FlipRSDRSettings* settings) {
    furi_assert(settings);
    settings->frequency_preset = FlipRSDRFrequencyPreset43392;
    settings->transport_kind = FlipRSDRTransportKindUsb;
    settings->stream_mode = FlipRSDRStreamModeBuffered;
    settings->frequency_offset_khz = 0;
    settings->auto_send_after_burst = false;
    settings->include_rssi = true;
    settings->include_timestamp = true;
    settings->max_pulse_count = 2048;
    settings->capture_timeout_ms = 250;
    settings->gap_threshold_ms = 20;
}

void fliprsdr_settings_validate(FlipRSDRSettings* settings) {
    furi_assert(settings);

    if(settings->frequency_preset >= FlipRSDRFrequencyPresetCount) {
        settings->frequency_preset = FlipRSDRFrequencyPreset43392;
    }
    if(settings->transport_kind >= FlipRSDRTransportKindCount) {
        settings->transport_kind = FlipRSDRTransportKindUsb;
    }
    if(settings->stream_mode >= FlipRSDRStreamModeCount) {
        settings->stream_mode = FlipRSDRStreamModeBuffered;
    }
    if(settings->frequency_offset_khz < FLIPRSDR_FREQUENCY_OFFSET_MIN_KHZ) {
        settings->frequency_offset_khz = FLIPRSDR_FREQUENCY_OFFSET_MIN_KHZ;
    } else if(settings->frequency_offset_khz > FLIPRSDR_FREQUENCY_OFFSET_MAX_KHZ) {
        settings->frequency_offset_khz = FLIPRSDR_FREQUENCY_OFFSET_MAX_KHZ;
    } else {
        const int16_t step = FLIPRSDR_FREQUENCY_OFFSET_STEP_KHZ;
        int16_t delta = settings->frequency_offset_khz - FLIPRSDR_FREQUENCY_OFFSET_MIN_KHZ;
        settings->frequency_offset_khz =
            FLIPRSDR_FREQUENCY_OFFSET_MIN_KHZ + ((delta + (step / 2)) / step) * step;
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

    if(settings->capture_timeout_ms < settings->gap_threshold_ms) {
        settings->capture_timeout_ms = settings->gap_threshold_ms;
    }
}

bool fliprsdr_settings_load(FlipRSDRSettings* settings) {
    furi_assert(settings);

    FlipRSDRSettingsStore store = {0};
    fliprsdr_settings_load_defaults(settings);

    if(saved_struct_load(
           FLIPRSDR_SETTINGS_PATH,
           &store,
           sizeof(store),
           FLIPRSDR_SETTINGS_MAGIC,
           FLIPRSDR_SETTINGS_VERSION)) {
        *settings = store.settings;
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
    const int64_t base_hz = (int64_t)fliprsdr_settings_frequency_value(settings->frequency_preset);
    const int64_t offset_hz = (int64_t)settings->frequency_offset_khz * 1000LL;
    const int64_t tuned_hz = base_hz + offset_hz;

    if(tuned_hz < 0) {
        return 0U;
    }

    return (uint32_t)tuned_hz;
}

const char* fliprsdr_settings_frequency_label(uint8_t preset_index) {
    if(preset_index >= FlipRSDRFrequencyPresetCount) return "?";
    return fliprsdr_frequency_labels[preset_index];
}

uint8_t fliprsdr_settings_frequency_offset_options_count(void) {
    return FLIPRSDR_FREQUENCY_OFFSET_OPTIONS_COUNT;
}

int16_t fliprsdr_settings_frequency_offset_value(uint8_t index) {
    if(index >= FLIPRSDR_FREQUENCY_OFFSET_OPTIONS_COUNT) {
        index = fliprsdr_settings_frequency_offset_index(0);
    }

    return (int16_t)(FLIPRSDR_FREQUENCY_OFFSET_MIN_KHZ +
                     ((int16_t)index * FLIPRSDR_FREQUENCY_OFFSET_STEP_KHZ));
}

uint8_t fliprsdr_settings_frequency_offset_index(int16_t value) {
    if(value <= FLIPRSDR_FREQUENCY_OFFSET_MIN_KHZ) {
        return 0U;
    }
    if(value >= FLIPRSDR_FREQUENCY_OFFSET_MAX_KHZ) {
        return FLIPRSDR_FREQUENCY_OFFSET_OPTIONS_COUNT - 1U;
    }

    return (uint8_t)((value - FLIPRSDR_FREQUENCY_OFFSET_MIN_KHZ) /
                     FLIPRSDR_FREQUENCY_OFFSET_STEP_KHZ);
}

void fliprsdr_settings_frequency_offset_label(int16_t value, char* buffer, size_t size) {
    furi_assert(buffer);
    if(value == 0) {
        snprintf(buffer, size, "0 kHz");
    } else {
        snprintf(buffer, size, "%+d kHz", value);
    }
}

const char* fliprsdr_settings_transport_label(uint8_t transport_index) {
    if(transport_index >= FlipRSDRTransportKindCount) return "?";
    return fliprsdr_transport_labels[transport_index];
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
