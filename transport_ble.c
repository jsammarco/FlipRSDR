#include "transport_i.h"

#include <bt/bt_service/bt.h>
#include <furi_hal_bt.h>
#include <profiles/serial_profile.h>

typedef struct {
    FlipRSDRTransport* transport;
    Bt* bt;
    FuriHalBleProfileBase* profile;
    FuriEventFlag* events;
    bool connected;
    bool advertising;
} FlipRSDRTransportBleContext;

enum {
    FlipRSDRBleEventTxDone = (1U << 0),
    FlipRSDRBleEventDisconnected = (1U << 1),
};

static void fliprsdr_transport_ble_update_status(FlipRSDRTransportBleContext* context) {
    FlipRSDRTransportState state = FlipRSDRTransportStateDisconnected;
    if(context->connected) {
        state = FlipRSDRTransportStateConnected;
    } else if(context->advertising) {
        state = FlipRSDRTransportStateWaiting;
    }

    fliprsdr_transport_set_state(
        context->transport, state, context->connected, context->advertising);
}

static void fliprsdr_transport_ble_status_changed(BtStatus status, void* context_ptr) {
    FlipRSDRTransportBleContext* context = context_ptr;
    context->connected = (status == BtStatusConnected);
    context->advertising = (status == BtStatusAdvertising);
    if(!context->connected) {
        furi_event_flag_set(context->events, FlipRSDRBleEventDisconnected);
    }
    fliprsdr_transport_ble_update_status(context);
}

static uint16_t fliprsdr_transport_ble_serial_event(SerialServiceEvent event, void* context_ptr) {
    FlipRSDRTransportBleContext* context = context_ptr;
    if(event.event == SerialServiceEventTypeDataSent) {
        furi_event_flag_set(context->events, FlipRSDRBleEventTxDone);
    }
    return 0U;
}

static void* fliprsdr_transport_ble_init(FlipRSDRTransport* transport) {
    if(!furi_hal_bt_is_alive() || !furi_hal_bt_is_gatt_gap_supported()) {
        return NULL;
    }

    FlipRSDRTransportBleContext* context = malloc(sizeof(FlipRSDRTransportBleContext));
    context->transport = transport;
    context->bt = furi_record_open(RECORD_BT);
    context->events = furi_event_flag_alloc();
    context->connected = false;
    context->advertising = false;

    context->profile = bt_profile_start(context->bt, ble_profile_serial, NULL);
    if(!context->profile) {
        furi_event_flag_free(context->events);
        furi_record_close(RECORD_BT);
        free(context);
        return NULL;
    }

    bt_set_status_changed_callback(context->bt, fliprsdr_transport_ble_status_changed, context);
    ble_profile_serial_set_event_callback(
        context->profile, 0U, fliprsdr_transport_ble_serial_event, context);
    ble_profile_serial_set_rpc_active(context->profile, false);
    furi_hal_bt_start_advertising();

    /* TODO: If a future public BT API exposes the active profile template,
       preserve and restore that instead of falling back to the default serial profile. */
    context->advertising = furi_hal_bt_is_active();
    fliprsdr_transport_ble_update_status(context);
    return context;
}

static void fliprsdr_transport_ble_deinit(FlipRSDRTransport* transport, void* context_ptr) {
    UNUSED(transport);
    FlipRSDRTransportBleContext* context = context_ptr;
    if(context->profile) {
        ble_profile_serial_set_event_callback(context->profile, 0U, NULL, NULL);
    }
    bt_set_status_changed_callback(context->bt, NULL, NULL);
    bt_disconnect(context->bt);
    furi_delay_ms(100);
    bt_profile_restore_default(context->bt);
    furi_record_close(RECORD_BT);
    furi_event_flag_free(context->events);
    free(context);
}

static bool fliprsdr_transport_ble_send(
    FlipRSDRTransport* transport,
    void* context_ptr,
    const char* data,
    size_t length) {
    UNUSED(transport);
    FlipRSDRTransportBleContext* context = context_ptr;
    if(!context->connected || !context->profile) return false;

    size_t offset = 0U;
    while(offset < length) {
        const uint16_t chunk = MIN((size_t)BLE_PROFILE_SERIAL_PACKET_SIZE_MAX, length - offset);
        furi_event_flag_clear(
            context->events, FlipRSDRBleEventTxDone | FlipRSDRBleEventDisconnected);
        if(!ble_profile_serial_tx(context->profile, (uint8_t*)(data + offset), chunk)) {
            return false;
        }

        const uint32_t flags = furi_event_flag_wait(
            context->events,
            FlipRSDRBleEventTxDone | FlipRSDRBleEventDisconnected,
            FuriFlagWaitAny | FuriFlagNoClear,
            FLIPRSDR_BLE_SEND_TIMEOUT_MS);
        if((flags & FlipRSDRBleEventDisconnected) || !(flags & FlipRSDRBleEventTxDone)) {
            return false;
        }

        offset += chunk;
    }

    return true;
}

const FlipRSDRTransportBackend fliprsdr_transport_ble_backend = {
    .init = fliprsdr_transport_ble_init,
    .deinit = fliprsdr_transport_ble_deinit,
    .send = fliprsdr_transport_ble_send,
};
