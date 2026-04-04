#pragma once

#include "fliprsdr.h"

struct FlipRSDRTransport;
typedef struct FlipRSDRTransport FlipRSDRTransport;

#ifdef __cplusplus
extern "C" {
#endif

bool fliprsdr_protocol_enqueue_burst_start(
    FlipRSDRTransport* transport,
    const FlipRSDRBurstBuffer* burst,
    const FlipRSDRSettings* settings);
bool fliprsdr_protocol_enqueue_timing_chunk(
    FlipRSDRTransport* transport,
    const FlipRSDRSettings* settings,
    uint32_t session_id,
    uint32_t burst_id,
    const uint32_t* timings,
    uint16_t timing_count);
bool fliprsdr_protocol_enqueue_burst_end(
    FlipRSDRTransport* transport,
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
