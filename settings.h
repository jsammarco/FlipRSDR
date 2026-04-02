#pragma once

#include "fliprsdr.h"

#ifdef __cplusplus
extern "C" {
#endif

void fliprsdr_settings_load_defaults(FlipRSDRSettings* settings);
bool fliprsdr_settings_load(FlipRSDRSettings* settings);
bool fliprsdr_settings_save(const FlipRSDRSettings* settings);
void fliprsdr_settings_validate(FlipRSDRSettings* settings);

uint32_t fliprsdr_settings_frequency_value(uint8_t preset_index);
const char* fliprsdr_settings_frequency_label(uint8_t preset_index);
const char* fliprsdr_settings_transport_label(uint8_t transport_index);
const char* fliprsdr_settings_stream_mode_label(uint8_t mode_index);
const char* fliprsdr_settings_bool_label(bool value);

uint8_t fliprsdr_settings_max_pulse_count_options_count(void);
uint16_t fliprsdr_settings_max_pulse_count_value(uint8_t index);
const char* fliprsdr_settings_max_pulse_count_label(uint8_t index);
uint8_t fliprsdr_settings_max_pulse_count_index(uint16_t value);

uint8_t fliprsdr_settings_capture_timeout_options_count(void);
uint16_t fliprsdr_settings_capture_timeout_value(uint8_t index);
const char* fliprsdr_settings_capture_timeout_label(uint8_t index);
uint8_t fliprsdr_settings_capture_timeout_index(uint16_t value);

uint8_t fliprsdr_settings_gap_threshold_options_count(void);
uint16_t fliprsdr_settings_gap_threshold_value(uint8_t index);
const char* fliprsdr_settings_gap_threshold_label(uint8_t index);
uint8_t fliprsdr_settings_gap_threshold_index(uint16_t value);

#ifdef __cplusplus
}
#endif
