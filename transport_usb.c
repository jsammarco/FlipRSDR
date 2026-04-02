#include "transport_i.h"

#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>

typedef struct {
    FlipRSDRTransport* transport;
    FuriHalUsbInterface* previous_interface;
    FuriSemaphore* tx_semaphore;
    FuriMutex* usb_mutex;
    bool usb_was_locked;
    bool usb_connected;
    bool dtr_asserted;
} FlipRSDRTransportUsbContext;

static void fliprsdr_transport_usb_update_status(FlipRSDRTransportUsbContext* context) {
    const bool connected = context->usb_connected && context->dtr_asserted;
    fliprsdr_transport_set_state(
        context->transport,
        connected ? FlipRSDRTransportStateConnected : FlipRSDRTransportStateWaiting,
        connected,
        false);
}

static void fliprsdr_transport_usb_tx_callback(void* context) {
    FlipRSDRTransportUsbContext* usb = context;
    furi_semaphore_release(usb->tx_semaphore);
}

static void fliprsdr_transport_usb_rx_callback(void* context) {
    UNUSED(context);
}

static void fliprsdr_transport_usb_state_callback(void* context, CdcState state) {
    FlipRSDRTransportUsbContext* usb = context;
    usb->usb_connected = (state == CdcStateConnected);
    fliprsdr_transport_usb_update_status(usb);
}

static void fliprsdr_transport_usb_ctrl_line_callback(void* context, CdcCtrlLine ctrl_lines) {
    FlipRSDRTransportUsbContext* usb = context;
    usb->dtr_asserted = (ctrl_lines & CdcCtrlLineDTR) != 0;
    fliprsdr_transport_usb_update_status(usb);
}

static void fliprsdr_transport_usb_config_callback(
    void* context,
    struct usb_cdc_line_coding* config) {
    FlipRSDRTransportUsbContext* usb = context;
    fliprsdr_transport_set_usb_baud_rate(
        usb->transport, config ? (uint32_t)config->dwDTERate : 0U);
}

static CdcCallbacks fliprsdr_transport_usb_callbacks = {
    .tx_ep_callback = fliprsdr_transport_usb_tx_callback,
    .rx_ep_callback = fliprsdr_transport_usb_rx_callback,
    .state_callback = fliprsdr_transport_usb_state_callback,
    .ctrl_line_callback = fliprsdr_transport_usb_ctrl_line_callback,
    .config_callback = fliprsdr_transport_usb_config_callback,
};

static void* fliprsdr_transport_usb_init(FlipRSDRTransport* transport) {
    FlipRSDRTransportUsbContext* context = malloc(sizeof(FlipRSDRTransportUsbContext));
    context->transport = transport;
    context->previous_interface = furi_hal_usb_get_config();
    context->tx_semaphore = furi_semaphore_alloc(1, 1);
    context->usb_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    context->usb_was_locked = furi_hal_usb_is_locked();
    context->usb_connected = false;
    context->dtr_asserted = false;

    if(context->usb_was_locked) {
        furi_hal_usb_unlock();
    }

    /* TODO: Revisit if a future firmware exposes a dedicated user VCP helper.
       For now the app temporarily switches to dual CDC and uses channel 1. */
    if(context->previous_interface != &usb_cdc_dual) {
        if(!furi_hal_usb_set_config(&usb_cdc_dual, NULL)) {
            if(context->usb_was_locked) {
                furi_hal_usb_lock();
            }
            furi_mutex_free(context->usb_mutex);
            furi_semaphore_free(context->tx_semaphore);
            free(context);
            return NULL;
        }
    }

    context->dtr_asserted =
        (furi_hal_cdc_get_ctrl_line_state(FLIPRSDR_USB_VCP_CHANNEL) & CdcCtrlLineDTR) != 0;
    fliprsdr_transport_set_usb_baud_rate(
        transport,
        (uint32_t)furi_hal_cdc_get_port_settings(FLIPRSDR_USB_VCP_CHANNEL)->dwDTERate);
    furi_hal_cdc_set_callbacks(
        FLIPRSDR_USB_VCP_CHANNEL, &fliprsdr_transport_usb_callbacks, context);
    fliprsdr_transport_usb_update_status(context);
    return context;
}

static void fliprsdr_transport_usb_deinit(FlipRSDRTransport* transport, void* context_ptr) {
    UNUSED(transport);
    FlipRSDRTransportUsbContext* context = context_ptr;
    furi_hal_cdc_set_callbacks(FLIPRSDR_USB_VCP_CHANNEL, NULL, NULL);
    if(context->previous_interface && (context->previous_interface != &usb_cdc_dual)) {
        furi_hal_usb_set_config(context->previous_interface, NULL);
    }
    if(context->usb_was_locked) {
        furi_hal_usb_lock();
    }
    furi_mutex_free(context->usb_mutex);
    furi_semaphore_free(context->tx_semaphore);
    free(context);
}

static bool fliprsdr_transport_usb_send(
    FlipRSDRTransport* transport,
    void* context_ptr,
    const char* data,
    size_t length) {
    UNUSED(transport);
    FlipRSDRTransportUsbContext* context = context_ptr;
    if(!(context->usb_connected && context->dtr_asserted)) return false;

    size_t offset = 0U;
    while(offset < length) {
        const uint16_t chunk = MIN((size_t)CDC_DATA_SZ, length - offset);
        if(furi_semaphore_acquire(context->tx_semaphore, FLIPRSDR_USB_SEND_TIMEOUT_MS) !=
           FuriStatusOk) {
            return false;
        }

        furi_check(furi_mutex_acquire(context->usb_mutex, FuriWaitForever) == FuriStatusOk);
        const bool connected = context->usb_connected && context->dtr_asserted;
        if(connected) {
            furi_hal_cdc_send(FLIPRSDR_USB_VCP_CHANNEL, (uint8_t*)(data + offset), chunk);
        }
        furi_check(furi_mutex_release(context->usb_mutex) == FuriStatusOk);

        if(!connected) {
            furi_semaphore_release(context->tx_semaphore);
            return false;
        }
        offset += chunk;
    }

    return true;
}

const FlipRSDRTransportBackend fliprsdr_transport_usb_backend = {
    .init = fliprsdr_transport_usb_init,
    .deinit = fliprsdr_transport_usb_deinit,
    .send = fliprsdr_transport_usb_send,
};
