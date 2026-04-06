#pragma once

#include "fliprsdr.h"

#include <applications/services/expansion/expansion.h>
#include <lib/subghz/devices/devices.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FlipRSDRRadioOpenStatusClosed = 0,
    FlipRSDRRadioOpenStatusInternalReady,
    FlipRSDRRadioOpenStatusExternalReady,
    FlipRSDRRadioOpenStatusExternalDriverMissing,
    FlipRSDRRadioOpenStatusExternalNotDetected,
    FlipRSDRRadioOpenStatusExternalInitFailed,
    FlipRSDRRadioOpenStatusInternalUnavailable,
} FlipRSDRRadioOpenStatus;

typedef struct {
    const SubGhzDevice* device;
    Expansion* expansion;
    FlipRSDRRadioOpenStatus open_status;
    bool external_selected;
    bool external_requested;
    bool otg_enabled;
    bool devices_initialized;
    bool expansion_disabled;
} FlipRSDRRadio;

void fliprsdr_radio_init(FlipRSDRRadio* radio);
bool fliprsdr_radio_open(FlipRSDRRadio* radio, const FlipRSDRSettings* settings);
void fliprsdr_radio_close(FlipRSDRRadio* radio);
const SubGhzDevice* fliprsdr_radio_device(const FlipRSDRRadio* radio);
FlipRSDRRadioOpenStatus fliprsdr_radio_open_status(const FlipRSDRRadio* radio);
const char* fliprsdr_radio_open_status_label(FlipRSDRRadioOpenStatus status);
const char* fliprsdr_radio_data_pin_label(const FlipRSDRRadio* radio);
bool fliprsdr_radio_is_external_selected(const FlipRSDRRadio* radio);
bool fliprsdr_radio_is_frequency_valid(const FlipRSDRRadio* radio, uint32_t frequency);
void fliprsdr_radio_reset(const FlipRSDRRadio* radio);
void fliprsdr_radio_load_preset(const FlipRSDRRadio* radio, FuriHalSubGhzPreset preset);
uint32_t fliprsdr_radio_set_frequency(const FlipRSDRRadio* radio, uint32_t frequency);
void fliprsdr_radio_set_rx(const FlipRSDRRadio* radio);
bool fliprsdr_radio_set_tx(const FlipRSDRRadio* radio);
void fliprsdr_radio_idle(const FlipRSDRRadio* radio);
void fliprsdr_radio_sleep(const FlipRSDRRadio* radio);
void fliprsdr_radio_start_async_rx(const FlipRSDRRadio* radio, void* callback, void* context);
void fliprsdr_radio_stop_async_rx(const FlipRSDRRadio* radio);
bool fliprsdr_radio_start_async_tx(const FlipRSDRRadio* radio, void* callback, void* context);
bool fliprsdr_radio_is_async_tx_complete(const FlipRSDRRadio* radio);
void fliprsdr_radio_stop_async_tx(const FlipRSDRRadio* radio);
float fliprsdr_radio_get_rssi(const FlipRSDRRadio* radio);
void fliprsdr_radio_set_audio_mirror(const FlipRSDRRadio* radio, bool enabled);

#ifdef __cplusplus
}
#endif
