#pragma once

#include "fliprsdr.h"

typedef struct FlipRSDRTransport FlipRSDRTransport;

typedef struct {
    void* (*init)(FlipRSDRTransport* transport);
    void (*deinit)(FlipRSDRTransport* transport, void* context);
    bool (*send)(FlipRSDRTransport* transport, void* context, const char* data, size_t length);
} FlipRSDRTransportBackend;

extern const FlipRSDRTransportBackend fliprsdr_transport_usb_backend;
extern const FlipRSDRTransportBackend fliprsdr_transport_ble_backend;

void fliprsdr_transport_set_state(
    FlipRSDRTransport* transport,
    FlipRSDRTransportState state,
    bool connected,
    bool advertising);
void fliprsdr_transport_set_last_send_ok(FlipRSDRTransport* transport, bool ok);
void fliprsdr_transport_set_usb_baud_rate(FlipRSDRTransport* transport, uint32_t baud_rate);
