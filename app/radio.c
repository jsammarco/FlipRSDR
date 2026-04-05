#include "radio.h"

#include <applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h>
#include <applications/services/power/power_service/power.h>
#include <furi_hal.h>
#include <furi_hal_resources.h>
#include <lib/subghz/devices/cc1101_configs.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>

static const char* fliprsdr_radio_gpio_name(const GpioPin* pin) {
    if(pin == &gpio_ext_pb2) return "PB2";
    if(pin == &gpio_ext_pb3) return "PB3";
    if(pin == &gpio_ext_pa4) return "PA4";
    if(pin == &gpio_ext_pa6) return "PA6";
    if(pin == &gpio_ext_pa7) return "PA7";
    if(pin == &gpio_ext_pc3) return "PC3";
    if(pin == &gpio_ext_pc1) return "PC1";
    if(pin == &gpio_ext_pc0) return "PC0";
    if(pin == &gpio_cc1101_g0) return "INT";
    return "?";
}

static void fliprsdr_radio_set_expansion_disabled(FlipRSDRRadio* radio, bool disabled) {
    if(radio->expansion_disabled == disabled) {
        return;
    }

    if(disabled) {
        radio->expansion = furi_record_open(RECORD_EXPANSION);
        expansion_disable(radio->expansion);
        radio->expansion_disabled = true;
    } else if(radio->expansion) {
        expansion_enable(radio->expansion);
        furi_record_close(RECORD_EXPANSION);
        radio->expansion = NULL;
        radio->expansion_disabled = false;
    }
}

static void fliprsdr_radio_set_otg_enabled(FlipRSDRRadio* radio, bool enabled) {
    if(radio->otg_enabled == enabled) {
        return;
    }

    Power* power = furi_record_open(RECORD_POWER);
    power_enable_otg(power, enabled);
    furi_record_close(RECORD_POWER);
    radio->otg_enabled = enabled;
}

static bool fliprsdr_radio_internal_load_preset(FuriHalSubGhzPreset preset) {
    switch(preset) {
    case FuriHalSubGhzPresetOok650Async:
        furi_hal_subghz_load_custom_preset(subghz_device_cc1101_preset_ook_650khz_async_regs);
        return true;
    case FuriHalSubGhzPresetOok270Async:
        furi_hal_subghz_load_custom_preset(subghz_device_cc1101_preset_ook_270khz_async_regs);
        return true;
    default:
        return false;
    }
}

void fliprsdr_radio_init(FlipRSDRRadio* radio) {
    furi_assert(radio);
    radio->device = NULL;
    radio->expansion = NULL;
    radio->open_status = FlipRSDRRadioOpenStatusClosed;
    radio->external_selected = false;
    radio->external_requested = false;
    radio->otg_enabled = false;
    radio->devices_initialized = false;
    radio->expansion_disabled = false;
}

bool fliprsdr_radio_open(FlipRSDRRadio* radio, const FlipRSDRSettings* settings) {
    furi_assert(radio);
    furi_assert(settings);

    fliprsdr_radio_close(radio);
    radio->external_requested = settings->external_radio_module;

    subghz_devices_init();
    radio->devices_initialized = true;

    if(settings->external_radio_module) {
        fliprsdr_radio_set_expansion_disabled(radio, true);
        fliprsdr_radio_set_otg_enabled(radio, true);
        const SubGhzDevice* external_device =
            subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_EXT_NAME);
        if(!external_device) {
            radio->open_status = FlipRSDRRadioOpenStatusExternalDriverMissing;
        } else if(!subghz_devices_is_connect(external_device)) {
            radio->open_status = FlipRSDRRadioOpenStatusExternalNotDetected;
        } else {
            if(subghz_devices_begin(external_device)) {
                radio->device = external_device;
                radio->open_status = FlipRSDRRadioOpenStatusExternalReady;
                radio->external_selected = true;
                return true;
            }

            radio->open_status = FlipRSDRRadioOpenStatusExternalInitFailed;
            subghz_devices_end(external_device);
        }
        subghz_devices_deinit();
        radio->devices_initialized = false;
        fliprsdr_radio_set_otg_enabled(radio, false);
        fliprsdr_radio_set_expansion_disabled(radio, false);
        radio->external_requested = false;
        return false;
    }

    radio->device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    radio->external_selected = false;
    radio->open_status = radio->device ? FlipRSDRRadioOpenStatusInternalReady :
                                         FlipRSDRRadioOpenStatusInternalUnavailable;
    return radio->device != NULL;
}

void fliprsdr_radio_close(FlipRSDRRadio* radio) {
    furi_assert(radio);

    if(radio->external_selected && radio->device) {
        subghz_devices_sleep(radio->device);
        subghz_devices_end(radio->device);
    } else if(radio->device) {
        furi_hal_subghz_sleep();
        furi_hal_subghz_shutdown();
    }

    radio->device = NULL;
    radio->open_status = FlipRSDRRadioOpenStatusClosed;

    if(radio->devices_initialized) {
        subghz_devices_deinit();
        radio->devices_initialized = false;
    }

    fliprsdr_radio_set_otg_enabled(radio, false);
    fliprsdr_radio_set_expansion_disabled(radio, false);
    radio->external_selected = false;
    radio->external_requested = false;
}

const SubGhzDevice* fliprsdr_radio_device(const FlipRSDRRadio* radio) {
    furi_assert(radio);
    return radio->device;
}

FlipRSDRRadioOpenStatus fliprsdr_radio_open_status(const FlipRSDRRadio* radio) {
    furi_assert(radio);
    return radio->open_status;
}

const char* fliprsdr_radio_open_status_label(FlipRSDRRadioOpenStatus status) {
    switch(status) {
    case FlipRSDRRadioOpenStatusInternalReady:
        return "Internal ready";
    case FlipRSDRRadioOpenStatusExternalReady:
        return "External ready";
    case FlipRSDRRadioOpenStatusExternalDriverMissing:
        return "External driver missing";
    case FlipRSDRRadioOpenStatusExternalNotDetected:
        return "External module not detected";
    case FlipRSDRRadioOpenStatusExternalInitFailed:
        return "External module init failed";
    case FlipRSDRRadioOpenStatusInternalUnavailable:
        return "Internal radio unavailable";
    default:
        return "Radio closed";
    }
}

const char* fliprsdr_radio_data_pin_label(const FlipRSDRRadio* radio) {
    furi_assert(radio);
    if(!radio->device) {
        return "-";
    }

    if(radio->external_selected) {
        return fliprsdr_radio_gpio_name(subghz_devices_get_data_gpio(radio->device));
    }

    return fliprsdr_radio_gpio_name(&gpio_cc1101_g0);
}

bool fliprsdr_radio_is_external_selected(const FlipRSDRRadio* radio) {
    furi_assert(radio);
    return radio->external_selected;
}

bool fliprsdr_radio_is_frequency_valid(const FlipRSDRRadio* radio, uint32_t frequency) {
    furi_assert(radio);
    if(radio->external_selected && radio->device) {
        return subghz_devices_is_frequency_valid(radio->device, frequency);
    }
    return furi_hal_subghz_is_frequency_valid(frequency);
}

void fliprsdr_radio_reset(const FlipRSDRRadio* radio) {
    furi_assert(radio);
    if(radio->external_selected && radio->device) {
        subghz_devices_reset(radio->device);
    } else {
        furi_hal_subghz_reset();
    }
}

void fliprsdr_radio_load_preset(const FlipRSDRRadio* radio, FuriHalSubGhzPreset preset) {
    furi_assert(radio);
    if(radio->external_selected && radio->device) {
        subghz_devices_load_preset(radio->device, preset, NULL);
    } else {
        furi_check(fliprsdr_radio_internal_load_preset(preset));
    }
}

uint32_t fliprsdr_radio_set_frequency(const FlipRSDRRadio* radio, uint32_t frequency) {
    furi_assert(radio);
    if(radio->external_selected && radio->device) {
        return subghz_devices_set_frequency(radio->device, frequency);
    }
    return furi_hal_subghz_set_frequency_and_path(frequency);
}

void fliprsdr_radio_set_rx(const FlipRSDRRadio* radio) {
    furi_assert(radio);
    if(radio->external_selected && radio->device) {
        subghz_devices_set_rx(radio->device);
    } else {
        furi_hal_subghz_rx();
    }
}

void fliprsdr_radio_idle(const FlipRSDRRadio* radio) {
    furi_assert(radio);
    if(radio->external_selected && radio->device) {
        subghz_devices_idle(radio->device);
    } else {
        furi_hal_subghz_idle();
    }
}

void fliprsdr_radio_sleep(const FlipRSDRRadio* radio) {
    furi_assert(radio);
    if(radio->external_selected && radio->device) {
        subghz_devices_sleep(radio->device);
    } else {
        furi_hal_subghz_sleep();
    }
}

void fliprsdr_radio_start_async_rx(const FlipRSDRRadio* radio, void* callback, void* context) {
    furi_assert(radio);
    if(radio->external_selected && radio->device) {
        subghz_devices_start_async_rx(radio->device, callback, context);
    } else {
        furi_hal_gpio_init(&gpio_cc1101_g0, GpioModeInput, GpioPullNo, GpioSpeedLow);
        furi_hal_subghz_start_async_rx((FuriHalSubGhzCaptureCallback)callback, context);
    }
}

void fliprsdr_radio_stop_async_rx(const FlipRSDRRadio* radio) {
    furi_assert(radio);
    if(radio->external_selected && radio->device) {
        subghz_devices_stop_async_rx(radio->device);
    } else {
        furi_hal_subghz_stop_async_rx();
    }
}

float fliprsdr_radio_get_rssi(const FlipRSDRRadio* radio) {
    furi_assert(radio);
    if(radio->external_selected && radio->device) {
        return subghz_devices_get_rssi(radio->device);
    }
    return furi_hal_subghz_get_rssi();
}

void fliprsdr_radio_set_audio_mirror(const FlipRSDRRadio* radio, bool enabled) {
    furi_assert(radio);
    const GpioPin* pin = enabled ? &gpio_speaker : NULL;

    if(radio->external_selected && radio->device) {
        subghz_devices_set_async_mirror_pin(radio->device, pin);
    } else {
        furi_hal_subghz_set_async_mirror_pin(pin);
    }
}
