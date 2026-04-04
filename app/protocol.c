#include "protocol.h"

#include "burst_buffer.h"
#include "transport.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    FlipRSDRPacketVersion = 0x01,
    FlipRSDRPacketHeaderSize = 8,
    FlipRSDRPacketTypeBurstStart = 0x01,
    FlipRSDRPacketTypeTimingChunk = 0x02,
    FlipRSDRPacketTypeBurstEnd = 0x03,
    FlipRSDRPacketTypeBurstCapture = 0x04,
    FlipRSDRPacketFlagTimestamp = (1U << 0),
    FlipRSDRPacketFlagRssi = (1U << 1),
    FlipRSDRPacketFlagTruncated = (1U << 2),
    FlipRSDRPacketFlagOverflow = (1U << 3),
};

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

static size_t fliprsdr_protocol_format_json_burst_start(
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

static size_t fliprsdr_protocol_format_json_timing_chunk(
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

static size_t fliprsdr_protocol_format_json_burst_end(
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

static bool fliprsdr_protocol_send_json_buffered_capture(
    FlipRSDRTransport* transport,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings) {
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

static void fliprsdr_protocol_write_u16_le(uint8_t* buffer, uint16_t value) {
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void fliprsdr_protocol_write_u32_le(uint8_t* buffer, uint32_t value) {
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static void fliprsdr_protocol_write_i16_le(uint8_t* buffer, int16_t value) {
    fliprsdr_protocol_write_u16_le(buffer, (uint16_t)value);
}

static size_t fliprsdr_protocol_uvarint_size(uint32_t value) {
    size_t size = 1U;
    while(value >= 0x80U) {
        value >>= 7U;
        size++;
    }
    return size;
}

static size_t fliprsdr_protocol_write_uvarint(uint8_t* out, uint32_t value) {
    size_t i = 0U;
    while(value >= 0x80U) {
        out[i++] = (uint8_t)((value & 0x7FU) | 0x80U);
        value >>= 7U;
    }
    out[i++] = (uint8_t)(value & 0x7FU);
    return i;
}

static uint16_t fliprsdr_protocol_crc16_xmodem(const uint8_t* data, size_t length) {
    uint16_t crc = 0x0000U;
    for(size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8U;
        for(uint8_t bit = 0; bit < 8U; bit++) {
            if(crc & 0x8000U) {
                crc = (uint16_t)((crc << 1U) ^ 0x1021U);
            } else {
                crc <<= 1U;
            }
        }
    }
    return crc;
}

static size_t fliprsdr_protocol_cobs_max_encoded_size(size_t length) {
    return length + (length / 254U) + 1U;
}

static size_t fliprsdr_protocol_cobs_encode(
    const uint8_t* input,
    size_t length,
    uint8_t* output,
    size_t output_size) {
    if(output_size == 0U) return 0U;

    size_t read_index = 0U;
    size_t write_index = 1U;
    size_t code_index = 0U;
    uint8_t code = 1U;

    while(read_index < length) {
        if(write_index >= output_size) return 0U;

        if(input[read_index] == 0U) {
            output[code_index] = code;
            code_index = write_index++;
            code = 1U;
            read_index++;
            continue;
        }

        output[write_index++] = input[read_index++];
        code++;

        if(code == 0xFFU) {
            output[code_index] = code;
            code_index = write_index++;
            code = 1U;
        }
    }

    if(code_index >= output_size) return 0U;
    output[code_index] = code;
    return write_index;
}

static uint8_t fliprsdr_protocol_binary_flags(
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings,
    bool include_timestamp,
    bool include_rssi) {
    uint8_t flags = 0U;
    if(include_timestamp && settings->include_timestamp) {
        flags |= FlipRSDRPacketFlagTimestamp;
    }
    if(include_rssi && settings->include_rssi) {
        flags |= FlipRSDRPacketFlagRssi;
    }
    if(burst->truncated) {
        flags |= FlipRSDRPacketFlagTruncated;
    }
    if(burst->overflow) {
        flags |= FlipRSDRPacketFlagOverflow;
    }
    return flags;
}

static bool fliprsdr_protocol_send_binary_packet(
    FlipRSDRTransport* transport,
    uint8_t type,
    uint8_t flags,
    const uint8_t* payload,
    size_t payload_length,
    bool enqueue) {
    furi_assert(transport);
    furi_assert(payload || (payload_length == 0U));

    const size_t raw_length =
        FlipRSDRPacketHeaderSize + payload_length + sizeof(uint16_t);
    const size_t encoded_capacity = fliprsdr_protocol_cobs_max_encoded_size(raw_length) + 1U;
    if(enqueue && (encoded_capacity > FLIPRSDR_TRANSPORT_MESSAGE_MAX)) {
        return false;
    }

    uint8_t* raw = malloc(raw_length);
    uint8_t* encoded = malloc(encoded_capacity);
    if(!raw || !encoded) {
        if(raw) free(raw);
        if(encoded) free(encoded);
        return false;
    }

    raw[0] = FlipRSDRPacketVersion;
    raw[1] = type;
    raw[2] = flags;
    raw[3] = FlipRSDRPacketHeaderSize;
    fliprsdr_protocol_write_u16_le(raw + 4U, (uint16_t)payload_length);
    fliprsdr_protocol_write_u16_le(raw + 6U, fliprsdr_transport_next_sequence(transport));
    if(payload_length > 0U) {
        memcpy(raw + FlipRSDRPacketHeaderSize, payload, payload_length);
    }

    const uint16_t crc =
        fliprsdr_protocol_crc16_xmodem(raw, FlipRSDRPacketHeaderSize + payload_length);
    fliprsdr_protocol_write_u16_le(raw + FlipRSDRPacketHeaderSize + payload_length, crc);

    const size_t encoded_length =
        fliprsdr_protocol_cobs_encode(raw, raw_length, encoded, encoded_capacity - 1U);
    bool ok = false;
    if(encoded_length > 0U) {
        encoded[encoded_length] = 0U;
        ok = enqueue ?
                 fliprsdr_transport_enqueue_bytes(transport, encoded, encoded_length + 1U) :
                 fliprsdr_transport_send_direct(
                     transport, (const char*)encoded, encoded_length + 1U);
    }

    free(raw);
    free(encoded);
    return ok;
}

static size_t fliprsdr_protocol_build_burst_start_payload(
    uint8_t* payload,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings) {
    size_t offset = 0U;
    fliprsdr_protocol_write_u16_le(payload + offset, (uint16_t)burst->session_id);
    offset += 2U;
    fliprsdr_protocol_write_u16_le(payload + offset, (uint16_t)burst->burst_id);
    offset += 2U;
    fliprsdr_protocol_write_u32_le(payload + offset, burst->frequency_hz);
    offset += 4U;
    payload[offset++] = burst->first_level ? 1U : 0U;
    payload[offset++] = 0U;
    if(settings->include_timestamp) {
        fliprsdr_protocol_write_u32_le(payload + offset, burst->timestamp_ms * 1000UL);
        offset += 4U;
    }
    return offset;
}

static size_t fliprsdr_protocol_build_timing_chunk_payload(
    uint8_t* payload,
    uint32_t session_id,
    uint32_t burst_id,
    const uint32_t* timings,
    uint16_t timing_count) {
    size_t offset = 0U;
    fliprsdr_protocol_write_u16_le(payload + offset, (uint16_t)session_id);
    offset += 2U;
    fliprsdr_protocol_write_u16_le(payload + offset, (uint16_t)burst_id);
    offset += 2U;
    fliprsdr_protocol_write_u16_le(payload + offset, timing_count);
    offset += 2U;
    for(uint16_t i = 0; i < timing_count; i++) {
        offset += fliprsdr_protocol_write_uvarint(payload + offset, timings[i]);
    }
    return offset;
}

static size_t fliprsdr_protocol_build_burst_end_payload(
    uint8_t* payload,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings) {
    size_t offset = 0U;
    fliprsdr_protocol_write_u16_le(payload + offset, (uint16_t)burst->session_id);
    offset += 2U;
    fliprsdr_protocol_write_u16_le(payload + offset, (uint16_t)burst->burst_id);
    offset += 2U;
    fliprsdr_protocol_write_u16_le(payload + offset, (uint16_t)burst->total_count);
    offset += 2U;
    if(settings->include_rssi) {
        int32_t rssi_cdbm = (int32_t)(burst->rssi * 100.0f);
        if(rssi_cdbm < INT16_MIN) rssi_cdbm = INT16_MIN;
        if(rssi_cdbm > INT16_MAX) rssi_cdbm = INT16_MAX;
        fliprsdr_protocol_write_i16_le(payload + offset, (int16_t)rssi_cdbm);
        offset += 2U;
    }
    return offset;
}

static size_t fliprsdr_protocol_measure_burst_capture_payload(
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings) {
    size_t payload_length = 12U;
    if(settings->include_timestamp) payload_length += 4U;
    if(settings->include_rssi) payload_length += 2U;
    for(uint32_t i = 0; i < burst->stored_count; i++) {
        payload_length += fliprsdr_protocol_uvarint_size(burst->timings[i]);
    }
    return payload_length;
}

static size_t fliprsdr_protocol_build_burst_capture_payload(
    uint8_t* payload,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings) {
    size_t offset = 0U;
    fliprsdr_protocol_write_u16_le(payload + offset, (uint16_t)burst->session_id);
    offset += 2U;
    fliprsdr_protocol_write_u16_le(payload + offset, (uint16_t)burst->burst_id);
    offset += 2U;
    fliprsdr_protocol_write_u32_le(payload + offset, burst->frequency_hz);
    offset += 4U;
    payload[offset++] = burst->first_level ? 1U : 0U;
    payload[offset++] = 0U;
    fliprsdr_protocol_write_u16_le(payload + offset, (uint16_t)burst->total_count);
    offset += 2U;
    if(settings->include_timestamp) {
        fliprsdr_protocol_write_u32_le(payload + offset, burst->timestamp_ms * 1000UL);
        offset += 4U;
    }
    if(settings->include_rssi) {
        int32_t rssi_cdbm = (int32_t)(burst->rssi * 100.0f);
        if(rssi_cdbm < INT16_MIN) rssi_cdbm = INT16_MIN;
        if(rssi_cdbm > INT16_MAX) rssi_cdbm = INT16_MAX;
        fliprsdr_protocol_write_i16_le(payload + offset, (int16_t)rssi_cdbm);
        offset += 2U;
    }
    for(uint32_t i = 0; i < burst->stored_count; i++) {
        offset += fliprsdr_protocol_write_uvarint(payload + offset, burst->timings[i]);
    }
    return offset;
}

static bool fliprsdr_protocol_send_burst_start_internal(
    FlipRSDRTransport* transport,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings,
    bool enqueue) {
    if(settings->protocol_format == FlipRSDRProtocolFormatJson) {
        char line[FLIPRSDR_PROTOCOL_LINE_MAX];
        const size_t length =
            fliprsdr_protocol_format_json_burst_start(line, sizeof(line), burst, settings);
        return length < sizeof(line) && fliprsdr_transport_enqueue_line(transport, line);
    }

    uint8_t payload[16U];
    const size_t payload_length =
        fliprsdr_protocol_build_burst_start_payload(payload, burst, settings);
    return fliprsdr_protocol_send_binary_packet(
        transport,
        FlipRSDRPacketTypeBurstStart,
        fliprsdr_protocol_binary_flags(burst, settings, true, false),
        payload,
        payload_length,
        enqueue);
}

static bool fliprsdr_protocol_send_timing_chunk_internal(
    FlipRSDRTransport* transport,
    const FlipRSDRSettings* settings,
    uint32_t session_id,
    uint32_t burst_id,
    const uint32_t* timings,
    uint16_t timing_count,
    bool enqueue) {
    if(settings->protocol_format == FlipRSDRProtocolFormatJson) {
        char line[FLIPRSDR_PROTOCOL_LINE_MAX];
        const size_t length = fliprsdr_protocol_format_json_timing_chunk(
            line, sizeof(line), session_id, burst_id, timings, timing_count);
        return length < sizeof(line) && fliprsdr_transport_enqueue_line(transport, line);
    }

    uint8_t payload[6U + (5U * FLIPRSDR_PROTOCOL_CHUNK_TIMINGS)];
    const size_t payload_length = fliprsdr_protocol_build_timing_chunk_payload(
        payload, session_id, burst_id, timings, timing_count);
    return fliprsdr_protocol_send_binary_packet(
        transport,
        FlipRSDRPacketTypeTimingChunk,
        0U,
        payload,
        payload_length,
        enqueue);
}

static bool fliprsdr_protocol_send_burst_end_internal(
    FlipRSDRTransport* transport,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings,
    bool enqueue) {
    if(settings->protocol_format == FlipRSDRProtocolFormatJson) {
        char line[FLIPRSDR_PROTOCOL_LINE_MAX];
        const size_t length =
            fliprsdr_protocol_format_json_burst_end(line, sizeof(line), burst, settings);
        return length < sizeof(line) && fliprsdr_transport_enqueue_line(transport, line);
    }

    uint8_t payload[8U];
    const size_t payload_length =
        fliprsdr_protocol_build_burst_end_payload(payload, burst, settings);
    return fliprsdr_protocol_send_binary_packet(
        transport,
        FlipRSDRPacketTypeBurstEnd,
        fliprsdr_protocol_binary_flags(burst, settings, false, true),
        payload,
        payload_length,
        enqueue);
}

static bool fliprsdr_protocol_send_binary_capture(
    FlipRSDRTransport* transport,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings) {
    const bool full_capture = !burst->truncated && (burst->stored_count == burst->total_count);
    const size_t payload_length = fliprsdr_protocol_measure_burst_capture_payload(burst, settings);
    const size_t raw_length =
        FlipRSDRPacketHeaderSize + payload_length + sizeof(uint16_t);

    if(full_capture && (raw_length <= FLIPRSDR_PROTOCOL_CAPTURE_MAX)) {
        uint8_t* payload = malloc(payload_length);
        if(!payload) return false;

        const size_t built =
            fliprsdr_protocol_build_burst_capture_payload(payload, burst, settings);
        const bool ok = fliprsdr_protocol_send_binary_packet(
            transport,
            FlipRSDRPacketTypeBurstCapture,
            fliprsdr_protocol_binary_flags(burst, settings, true, true),
            payload,
            built,
            false);
        free(payload);
        return ok;
    }

    if(!fliprsdr_protocol_send_burst_start_internal(transport, burst, settings, false)) {
        return false;
    }

    uint32_t offset = 0U;
    while(offset < burst->stored_count) {
        const uint16_t chunk_count = MIN(
            (uint32_t)FLIPRSDR_PROTOCOL_CHUNK_TIMINGS, burst->stored_count - offset);
        if(!fliprsdr_protocol_send_timing_chunk_internal(
               transport,
               settings,
               burst->session_id,
               burst->burst_id,
               burst->timings + offset,
               chunk_count,
               false)) {
            return false;
        }
        offset += chunk_count;
    }

    return fliprsdr_protocol_send_burst_end_internal(transport, burst, settings, false);
}

bool fliprsdr_protocol_enqueue_burst_start(
    FlipRSDRTransport* transport,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings) {
    furi_assert(transport);
    furi_assert(burst);
    furi_assert(settings);
    return fliprsdr_protocol_send_burst_start_internal(transport, burst, settings, true);
}

bool fliprsdr_protocol_enqueue_timing_chunk(
    FlipRSDRTransport* transport,
    const FlipRSDRSettings* settings,
    uint32_t session_id,
    uint32_t burst_id,
    const uint32_t* timings,
    uint16_t timing_count) {
    furi_assert(transport);
    furi_assert(settings);
    furi_assert(timings);
    return fliprsdr_protocol_send_timing_chunk_internal(
        transport, settings, session_id, burst_id, timings, timing_count, true);
}

bool fliprsdr_protocol_enqueue_burst_end(
    FlipRSDRTransport* transport,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings) {
    furi_assert(transport);
    furi_assert(burst);
    furi_assert(settings);
    return fliprsdr_protocol_send_burst_end_internal(transport, burst, settings, true);
}

bool fliprsdr_protocol_send_buffered_capture(
    FlipRSDRTransport* transport,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings) {
    furi_assert(transport);
    furi_assert(burst);
    furi_assert(settings);

    if(!burst->valid) return false;
    if(settings->protocol_format == FlipRSDRProtocolFormatJson) {
        return fliprsdr_protocol_send_json_buffered_capture(transport, burst, settings);
    }
    return fliprsdr_protocol_send_binary_capture(transport, burst, settings);
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

    fliprsdr_burst_buffer_start(burst, session_id, burst_id, frequency_hz, 0U, true);
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
