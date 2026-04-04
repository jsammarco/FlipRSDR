#pragma once

#include "fliprsdr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FlipRSDRTransport FlipRSDRTransport;

FlipRSDRTransport* fliprsdr_transport_alloc(void);
void fliprsdr_transport_free(FlipRSDRTransport* transport);

bool fliprsdr_transport_apply_settings(
    FlipRSDRTransport* transport,
    const FlipRSDRSettings* settings);
bool fliprsdr_transport_enqueue_bytes(
    FlipRSDRTransport* transport,
    const uint8_t* data,
    size_t length);
bool fliprsdr_transport_enqueue_line(FlipRSDRTransport* transport, const char* line);
bool fliprsdr_transport_send_direct(
    FlipRSDRTransport* transport,
    const char* data,
    size_t length);
bool fliprsdr_transport_send_direct_cstr(FlipRSDRTransport* transport, const char* text);
uint16_t fliprsdr_transport_next_sequence(FlipRSDRTransport* transport);
void fliprsdr_transport_copy_snapshot(
    FlipRSDRTransport* transport,
    FlipRSDRTransportSnapshot* snapshot);

#ifdef __cplusplus
}
#endif
