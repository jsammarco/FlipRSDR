#pragma once

#include "fliprsdr.h"

#ifdef __cplusplus
extern "C" {
#endif

void fliprsdr_burst_buffer_reset(FlipRSDRBurstBuffer* buffer);
void fliprsdr_burst_buffer_start(
    FlipRSDRBurstBuffer* buffer,
    uint32_t session_id,
    uint32_t burst_id,
    uint32_t frequency_hz,
    uint32_t timestamp_ms,
    bool first_level);
void fliprsdr_burst_buffer_append(
    FlipRSDRBurstBuffer* buffer,
    uint32_t duration_us,
    bool store_timing,
    uint16_t max_store_count);
void fliprsdr_burst_buffer_complete(FlipRSDRBurstBuffer* buffer, float rssi);
void fliprsdr_burst_buffer_copy(FlipRSDRBurstBuffer* target, const FlipRSDRBurstBuffer* source);

#ifdef __cplusplus
}
#endif
