#pragma once

#include "fliprsdr.h"

struct FlipRSDRTransport;
typedef struct FlipRSDRTransport FlipRSDRTransport;

#ifdef __cplusplus
extern "C" {
#endif

size_t fliprsdr_protocol_format_burst_start(
    char* buffer,
    size_t size,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings);
size_t fliprsdr_protocol_format_timing_chunk(
    char* buffer,
    size_t size,
    uint32_t session_id,
    uint32_t burst_id,
    const uint32_t* timings,
    uint16_t timing_count);
size_t fliprsdr_protocol_format_burst_end(
    char* buffer,
    size_t size,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings);
bool fliprsdr_protocol_send_buffered_capture(
    FlipRSDRTransport* transport,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings);
bool fliprsdr_protocol_send_debug_capture(
    FlipRSDRTransport* transport,
    const FlipRSDRSettings* settings,
    uint32_t session_id,
    uint32_t burst_id,
    uint32_t frequency_hz);

#ifdef __cplusplus
}
#endif
