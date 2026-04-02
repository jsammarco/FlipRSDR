#include "transport.h"

#include "transport_i.h"

typedef struct {
    size_t length;
    char data[FLIPRSDR_PROTOCOL_LINE_MAX];
} FlipRSDRTransportMessage;

struct FlipRSDRTransport {
    FlipRSDRTransportKind kind;
    const FlipRSDRTransportBackend* backend;
    void* backend_context;
    FuriMessageQueue* queue;
    FuriThread* thread;
    FuriMutex* mutex;
    FlipRSDRTransportSnapshot snapshot;
};

static int32_t fliprsdr_transport_tx_worker(void* context) {
    FlipRSDRTransport* transport = context;
    FlipRSDRTransportMessage message;

    while(furi_message_queue_get(transport->queue, &message, FuriWaitForever) == FuriStatusOk) {
        if(message.length == 0U) {
            break;
        }

        furi_check(furi_mutex_acquire(transport->mutex, FuriWaitForever) == FuriStatusOk);
        bool ok = false;
        if(transport->backend && transport->backend_context) {
            ok = transport->backend->send(
                transport, transport->backend_context, message.data, message.length);
        }
        transport->snapshot.last_send_ok = ok;
        furi_check(furi_mutex_release(transport->mutex) == FuriStatusOk);
    }

    return 0;
}

void fliprsdr_transport_set_state(
    FlipRSDRTransport* transport,
    FlipRSDRTransportState state,
    bool connected,
    bool advertising) {
    furi_assert(transport);
    transport->snapshot.state = state;
    transport->snapshot.connected = connected;
    transport->snapshot.advertising = advertising;
}

void fliprsdr_transport_set_last_send_ok(FlipRSDRTransport* transport, bool ok) {
    furi_assert(transport);
    transport->snapshot.last_send_ok = ok;
}

void fliprsdr_transport_set_usb_baud_rate(FlipRSDRTransport* transport, uint32_t baud_rate) {
    furi_assert(transport);
    transport->snapshot.usb_baud_rate = baud_rate;
}

static const FlipRSDRTransportBackend* fliprsdr_transport_backend_for_kind(
    FlipRSDRTransportKind kind) {
    switch(kind) {
    case FlipRSDRTransportKindUsb:
        return &fliprsdr_transport_usb_backend;
    case FlipRSDRTransportKindBle:
        return &fliprsdr_transport_ble_backend;
    default:
        return NULL;
    }
}

FlipRSDRTransport* fliprsdr_transport_alloc(void) {
    FlipRSDRTransport* transport = malloc(sizeof(FlipRSDRTransport));
    transport->kind = FlipRSDRTransportKindUsb;
    transport->backend = NULL;
    transport->backend_context = NULL;
    transport->queue =
        furi_message_queue_alloc(FLIPRSDR_TRANSPORT_QUEUE_DEPTH, sizeof(FlipRSDRTransportMessage));
    transport->thread =
        furi_thread_alloc_ex("FlipRSDRTx", 1536, fliprsdr_transport_tx_worker, transport);
    transport->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    transport->snapshot = (FlipRSDRTransportSnapshot){
        .kind = FlipRSDRTransportKindUsb,
        .state = FlipRSDRTransportStateDisconnected,
        .configured = false,
        .connected = false,
        .advertising = false,
        .last_send_ok = false,
        .usb_baud_rate = 0U,
    };
    furi_thread_start(transport->thread);
    return transport;
}

static void fliprsdr_transport_deinit_locked(FlipRSDRTransport* transport) {
    if(transport->backend && transport->backend_context) {
        transport->backend->deinit(transport, transport->backend_context);
        transport->backend_context = NULL;
        transport->backend = NULL;
    }
    transport->snapshot.configured = false;
    transport->snapshot.connected = false;
    transport->snapshot.advertising = false;
    transport->snapshot.state = FlipRSDRTransportStateDisconnected;
    transport->snapshot.usb_baud_rate = 0U;
}

void fliprsdr_transport_free(FlipRSDRTransport* transport) {
    furi_assert(transport);

    furi_check(furi_mutex_acquire(transport->mutex, FuriWaitForever) == FuriStatusOk);
    fliprsdr_transport_deinit_locked(transport);
    furi_check(furi_mutex_release(transport->mutex) == FuriStatusOk);

    FlipRSDRTransportMessage stop_message = {.length = 0U};
    furi_message_queue_put(transport->queue, &stop_message, FuriWaitForever);
    furi_thread_join(transport->thread);

    furi_thread_free(transport->thread);
    furi_message_queue_free(transport->queue);
    furi_mutex_free(transport->mutex);
    free(transport);
}

bool fliprsdr_transport_apply_settings(
    FlipRSDRTransport* transport,
    const FlipRSDRSettings* settings) {
    furi_assert(transport);
    furi_assert(settings);

    const FlipRSDRTransportBackend* backend =
        fliprsdr_transport_backend_for_kind((FlipRSDRTransportKind)settings->transport_kind);
    if(!backend) return false;

    bool ok = false;
    furi_check(furi_mutex_acquire(transport->mutex, FuriWaitForever) == FuriStatusOk);

    fliprsdr_transport_deinit_locked(transport);
    transport->kind = settings->transport_kind;
    transport->snapshot.kind = settings->transport_kind;
    transport->backend = backend;
    transport->backend_context = backend->init(transport);
    transport->snapshot.configured = transport->backend_context != NULL;
    ok = transport->backend_context != NULL;

    if(!ok) {
        transport->snapshot.state = FlipRSDRTransportStateError;
    }

    furi_check(furi_mutex_release(transport->mutex) == FuriStatusOk);
    return ok;
}

bool fliprsdr_transport_enqueue_line(FlipRSDRTransport* transport, const char* line) {
    furi_assert(transport);
    furi_assert(line);

    const size_t length = strlen(line);
    if(length >= FLIPRSDR_PROTOCOL_LINE_MAX) return false;

    FlipRSDRTransportMessage message = {.length = length};
    memcpy(message.data, line, length);
    return furi_message_queue_put(transport->queue, &message, 0) == FuriStatusOk;
}

bool fliprsdr_transport_send_direct(
    FlipRSDRTransport* transport,
    const char* data,
    size_t length) {
    furi_assert(transport);
    furi_assert(data);

    bool ok = false;
    furi_check(furi_mutex_acquire(transport->mutex, FuriWaitForever) == FuriStatusOk);
    if(transport->backend && transport->backend_context) {
        ok = transport->backend->send(transport, transport->backend_context, data, length);
        transport->snapshot.last_send_ok = ok;
    }
    furi_check(furi_mutex_release(transport->mutex) == FuriStatusOk);
    return ok;
}

bool fliprsdr_transport_send_direct_cstr(FlipRSDRTransport* transport, const char* text) {
    furi_assert(text);
    return fliprsdr_transport_send_direct(transport, text, strlen(text));
}

void fliprsdr_transport_copy_snapshot(
    FlipRSDRTransport* transport,
    FlipRSDRTransportSnapshot* snapshot) {
    furi_assert(transport);
    furi_assert(snapshot);

    furi_check(furi_mutex_acquire(transport->mutex, FuriWaitForever) == FuriStatusOk);
    *snapshot = transport->snapshot;
    furi_check(furi_mutex_release(transport->mutex) == FuriStatusOk);
}
