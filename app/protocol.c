#include "protocol.h"

#include "burst_buffer.h"
#include "transport.h"

#include <stdio.h>

static size_t fliprsdr_protocol_append_optional_timestamp(
    char* buffer,
    size_t size,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings) {
    if(!settings->include_timestamp) return 0U;
    return snprintf(buffer, size, ",\"timestamp\":%lu", (unsigned long)burst->timestamp_ms);
}

static size_t fliprsdr_protocol_append_optional_rssi(
    char* buffer,
    size_t size,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings) {
    if(!settings->include_rssi) return 0U;
    return snprintf(buffer, size, ",\"rssi\":%.1f", (double)burst->rssi);
}

size_t fliprsdr_protocol_format_burst_start(
    char* buffer,
    size_t size,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings) {
    furi_assert(buffer);
    furi_assert(burst);
    furi_assert(settings);

    size_t written = snprintf(
        buffer,
        size,
        "{\"type\":\"burst_start\",\"session\":%lu,\"burst\":%lu,\"freq\":%lu,\"first_level\":%u",
        (unsigned long)burst->session_id,
        (unsigned long)burst->burst_id,
        (unsigned long)burst->frequency_hz,
        burst->first_level ? 1U : 0U);
    if(written >= size) return size;

    written += fliprsdr_protocol_append_optional_timestamp(
        buffer + written, size - written, burst, settings);
    if(written >= size) return size;

    written += snprintf(buffer + written, size - written, "}\n");
    return MIN(written, size);
}

size_t fliprsdr_protocol_format_timing_chunk(
    char* buffer,
    size_t size,
    uint32_t session_id,
    uint32_t burst_id,
    const uint32_t* timings,
    uint16_t timing_count) {
    furi_assert(buffer);
    furi_assert(timings);

    size_t written = snprintf(
        buffer,
        size,
        "{\"type\":\"timing_chunk\",\"session\":%lu,\"burst\":%lu,\"timings\":[",
        (unsigned long)session_id,
        (unsigned long)burst_id);
    if(written >= size) return size;

    for(uint16_t i = 0; i < timing_count; i++) {
        written += snprintf(
            buffer + written,
            (written < size) ? (size - written) : 0U,
            "%s%lu",
            (i == 0) ? "" : ",",
            (unsigned long)timings[i]);
        if(written >= size) return size;
    }

    written += snprintf(buffer + written, size - written, "]}\n");
    return MIN(written, size);
}

size_t fliprsdr_protocol_format_burst_end(
    char* buffer,
    size_t size,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings) {
    furi_assert(buffer);
    furi_assert(burst);
    furi_assert(settings);

    size_t written = snprintf(
        buffer,
        size,
        "{\"type\":\"burst_end\",\"session\":%lu,\"burst\":%lu,\"count\":%lu",
        (unsigned long)burst->session_id,
        (unsigned long)burst->burst_id,
        (unsigned long)burst->total_count);
    if(written >= size) return size;

    written +=
        fliprsdr_protocol_append_optional_rssi(buffer + written, size - written, burst, settings);
    if(written >= size) return size;

    written += snprintf(
        buffer + written,
        size - written,
        ",\"truncated\":%s}\n",
        burst->truncated ? "true" : "false");
    return MIN(written, size);
}

bool fliprsdr_protocol_send_buffered_capture(
    FlipRSDRTransport* transport,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings) {
    furi_assert(transport);
    furi_assert(burst);
    furi_assert(settings);

    if(!burst->valid) return false;

    char buffer[FLIPRSDR_PROTOCOL_LINE_MAX];
    int written = snprintf(
        buffer,
        sizeof(buffer),
        "{\"type\":\"burst_capture\",\"session\":%lu,\"burst\":%lu,\"freq\":%lu,\"first_level\":%u",
        (unsigned long)burst->session_id,
        (unsigned long)burst->burst_id,
        (unsigned long)burst->frequency_hz,
        burst->first_level ? 1U : 0U);
    if((written < 0) || !fliprsdr_transport_send_direct(transport, buffer, (size_t)written)) {
        return false;
    }

    if(settings->include_timestamp) {
        written = snprintf(
            buffer, sizeof(buffer), ",\"timestamp\":%lu", (unsigned long)burst->timestamp_ms);
        if((written < 0) || !fliprsdr_transport_send_direct(transport, buffer, (size_t)written)) {
            return false;
        }
    }

    if(!fliprsdr_transport_send_direct_cstr(transport, ",\"timings\":[")) {
        return false;
    }

    for(uint32_t i = 0; i < burst->stored_count; i++) {
        written = snprintf(
            buffer,
            sizeof(buffer),
            "%s%lu",
            (i == 0U) ? "" : ",",
            (unsigned long)burst->timings[i]);
        if((written < 0) || !fliprsdr_transport_send_direct(transport, buffer, (size_t)written)) {
            return false;
        }
    }

    written = snprintf(buffer, sizeof(buffer), "],\"count\":%lu", (unsigned long)burst->total_count);
    if((written < 0) || !fliprsdr_transport_send_direct(transport, buffer, (size_t)written)) {
        return false;
    }

    if(settings->include_rssi) {
        written = snprintf(buffer, sizeof(buffer), ",\"rssi\":%.1f", (double)burst->rssi);
        if((written < 0) || !fliprsdr_transport_send_direct(transport, buffer, (size_t)written)) {
            return false;
        }
    }

    written = snprintf(
        buffer,
        sizeof(buffer),
        ",\"truncated\":%s}\n",
        burst->truncated ? "true" : "false");
    if(written < 0) return false;

    return fliprsdr_transport_send_direct(transport, buffer, (size_t)written);
}

bool fliprsdr_protocol_send_debug_capture(
    FlipRSDRTransport* transport,
    const FlipRSDRSettings* settings,
    uint32_t session_id,
    uint32_t burst_id,
    uint32_t frequency_hz) {
    static const uint32_t debug_timings[] = {
        360, 1090, 355, 360, 1095, 360, 355, 7300, 360, 1090, 355, 360, 1090, 360, 360, 350,
        360, 1100, 350, 360, 1090, 365, 350, 350, 365, 1090, 360, 360, 1095, 355, 350, 7800,
    };

    FlipRSDRBurstBuffer* burst = malloc(sizeof(FlipRSDRBurstBuffer));
    if(!burst) return false;

    fliprsdr_burst_buffer_start(burst, session_id, burst_id, frequency_hz, furi_get_tick(), true);
    for(size_t i = 0; i < COUNT_OF(debug_timings); i++) {
        fliprsdr_burst_buffer_append(
            burst, debug_timings[i], true, FLIPRSDR_BURST_TIMINGS_CAPACITY);
    }
    burst->complete = true;
    burst->rssi = -54.5f;

    const bool ok = fliprsdr_protocol_send_buffered_capture(transport, burst, settings);
    free(burst);
    return ok;
}
