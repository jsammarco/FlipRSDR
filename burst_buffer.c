#include "burst_buffer.h"

#include <string.h>

void fliprsdr_burst_buffer_reset(FlipRSDRBurstBuffer* buffer) {
    furi_assert(buffer);
    memset(buffer, 0, sizeof(FlipRSDRBurstBuffer));
}

void fliprsdr_burst_buffer_start(
    FlipRSDRBurstBuffer* buffer,
    uint32_t session_id,
    uint32_t burst_id,
    uint32_t frequency_hz,
    uint32_t timestamp_ms,
    bool first_level) {
    furi_assert(buffer);
    fliprsdr_burst_buffer_reset(buffer);
    buffer->valid = true;
    buffer->session_id = session_id;
    buffer->burst_id = burst_id;
    buffer->frequency_hz = frequency_hz;
    buffer->timestamp_ms = timestamp_ms;
    buffer->first_level = first_level;
}

void fliprsdr_burst_buffer_append(
    FlipRSDRBurstBuffer* buffer,
    uint32_t duration_us,
    bool store_timing,
    uint16_t max_store_count) {
    furi_assert(buffer);
    if(!buffer->valid) return;

    buffer->total_count++;

    if(!store_timing) return;

    const uint32_t store_limit = MIN((uint32_t)max_store_count, FLIPRSDR_BURST_TIMINGS_CAPACITY);
    if(buffer->stored_count < store_limit) {
        buffer->timings[buffer->stored_count++] = duration_us;
    } else {
        buffer->truncated = true;
    }
}

void fliprsdr_burst_buffer_complete(FlipRSDRBurstBuffer* buffer, float rssi) {
    furi_assert(buffer);
    if(!buffer->valid) return;
    buffer->complete = true;
    buffer->rssi = rssi;
}

void fliprsdr_burst_buffer_copy(FlipRSDRBurstBuffer* target, const FlipRSDRBurstBuffer* source) {
    furi_assert(target);
    furi_assert(source);
    memcpy(target, source, sizeof(FlipRSDRBurstBuffer));
}
