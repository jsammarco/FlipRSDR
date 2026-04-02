#include "capture_view.h"

#include "../settings.h"

#include <gui/elements.h>
#include <input/input.h>

struct FlipRSDRCaptureView {
    View* view;
    FlipRSDRCaptureViewActionCallback callback;
    void* context;
};

typedef struct {
    char frequency[16];
    char link[16];
    char capture_state[16];
    char burst[16];
    char count[16];
    char rssi[16];
    char flags[24];
    char mode[16];
    bool running;
    bool can_send;
} FlipRSDRCaptureViewModel;

static const char* fliprsdr_capture_view_state_text(FlipRSDRCaptureState state) {
    switch(state) {
    case FlipRSDRCaptureStateIdle:
        return "Idle";
    case FlipRSDRCaptureStateListening:
        return "Listen";
    case FlipRSDRCaptureStateReceiving:
        return "Recv";
    case FlipRSDRCaptureStateStreaming:
        return "Stream";
    case FlipRSDRCaptureStateComplete:
        return "Done";
    default:
        return "?";
    }
}

static const char* fliprsdr_capture_view_mode_text(uint8_t stream_mode) {
    switch(stream_mode) {
    case FlipRSDRStreamModeLive:
        return "Live";
    case FlipRSDRStreamModeBuffered:
        return "Buffer";
    case FlipRSDRStreamModeLiveBuffered:
        return "Both";
    default:
        return "?";
    }
}

static const char* fliprsdr_capture_view_link_text(const FlipRSDRTransportSnapshot* transport) {
    switch(transport->kind) {
    case FlipRSDRTransportKindUsb:
        if(transport->connected) return "USB Conn";
        if(transport->configured) return "USB Wait";
        return "USB Down";
    case FlipRSDRTransportKindBle:
        if(transport->connected) return "BLE Conn";
        if(transport->advertising) return "BLE Adv";
        if(transport->configured) return "BLE Wait";
        return "BLE Down";
    default:
        return "Link ?";
    }
}

static void fliprsdr_capture_view_format_link(
    char* buffer,
    size_t size,
    const FlipRSDRTransportSnapshot* transport) {
    if((transport->kind == FlipRSDRTransportKindUsb) && transport->usb_baud_rate) {
        if(transport->usb_baud_rate >= 1000000UL) {
            snprintf(
                buffer,
                size,
                "USB %lum",
                (unsigned long)(transport->usb_baud_rate / 1000000UL));
        } else if(transport->usb_baud_rate >= 1000UL) {
            snprintf(
                buffer,
                size,
                "USB %luk",
                (unsigned long)(transport->usb_baud_rate / 1000UL));
        } else {
            snprintf(buffer, size, "USB %lu", (unsigned long)transport->usb_baud_rate);
        }
    } else {
        snprintf(buffer, size, "%s", fliprsdr_capture_view_link_text(transport));
    }
}

static void fliprsdr_capture_view_draw(Canvas* canvas, void* model_ptr) {
    FlipRSDRCaptureViewModel* model = model_ptr;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 9, "FlipRSDR");
    canvas_draw_str_aligned(canvas, 126, 9, AlignRight, AlignBottom, model->capture_state);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 20, model->frequency);
    canvas_draw_str_aligned(canvas, 126, 20, AlignRight, AlignBottom, model->link);

    canvas_draw_str(canvas, 2, 30, model->mode);
    canvas_draw_str_aligned(canvas, 126, 30, AlignRight, AlignBottom, model->burst);

    canvas_draw_str(canvas, 2, 40, model->count);
    canvas_draw_str_aligned(canvas, 126, 40, AlignRight, AlignBottom, model->rssi);

    canvas_draw_str(canvas, 2, 50, model->flags);

    elements_button_left(canvas, "Clear");
    elements_button_center(canvas, model->running ? "Stop" : "Start");
    elements_button_right(canvas, model->can_send ? "Send" : "Hold");
}

static bool fliprsdr_capture_view_input(InputEvent* event, void* context) {
    FlipRSDRCaptureView* capture_view = context;
    if(event->type != InputTypeShort) return false;
    if(!capture_view->callback) return false;

    switch(event->key) {
    case InputKeyOk:
        capture_view->callback(FlipRSDRCaptureViewActionToggle, capture_view->context);
        return true;
    case InputKeyLeft:
        capture_view->callback(FlipRSDRCaptureViewActionClear, capture_view->context);
        return true;
    case InputKeyRight:
        capture_view->callback(FlipRSDRCaptureViewActionSend, capture_view->context);
        return true;
    default:
        return false;
    }
}

FlipRSDRCaptureView* fliprsdr_capture_view_alloc(void) {
    FlipRSDRCaptureView* capture_view = malloc(sizeof(FlipRSDRCaptureView));
    capture_view->view = view_alloc();
    capture_view->callback = NULL;
    capture_view->context = NULL;

    view_allocate_model(
        capture_view->view, ViewModelTypeLocking, sizeof(FlipRSDRCaptureViewModel));
    view_set_context(capture_view->view, capture_view);
    view_set_draw_callback(capture_view->view, fliprsdr_capture_view_draw);
    view_set_input_callback(capture_view->view, fliprsdr_capture_view_input);
    return capture_view;
}

void fliprsdr_capture_view_free(FlipRSDRCaptureView* capture_view) {
    furi_assert(capture_view);
    view_free(capture_view->view);
    free(capture_view);
}

View* fliprsdr_capture_view_get_view(FlipRSDRCaptureView* capture_view) {
    furi_assert(capture_view);
    return capture_view->view;
}

void fliprsdr_capture_view_set_action_callback(
    FlipRSDRCaptureView* capture_view,
    FlipRSDRCaptureViewActionCallback callback,
    void* context) {
    furi_assert(capture_view);
    capture_view->callback = callback;
    capture_view->context = context;
}

void fliprsdr_capture_view_set_snapshot(
    FlipRSDRCaptureView* capture_view,
    const FlipRSDRSettings* settings,
    const FlipRSDRTransportSnapshot* transport,
    const FlipRSDRCaptureSnapshot* capture) {
    furi_assert(capture_view);
    furi_assert(settings);
    furi_assert(transport);
    furi_assert(capture);

    with_view_model(
        capture_view->view,
        FlipRSDRCaptureViewModel * model,
        {
            const uint32_t frequency_hz = fliprsdr_settings_frequency_hz(settings);
            snprintf(
                model->frequency,
                sizeof(model->frequency),
                "F %lu.%03luM",
                (unsigned long)(frequency_hz / 1000000UL),
                (unsigned long)((frequency_hz % 1000000UL) / 1000UL));
            fliprsdr_capture_view_format_link(model->link, sizeof(model->link), transport);
            snprintf(
                model->capture_state,
                sizeof(model->capture_state),
                "%s",
                fliprsdr_capture_view_state_text(capture->state));
            snprintf(
                model->mode,
                sizeof(model->mode),
                "Mode %s",
                fliprsdr_capture_view_mode_text(settings->stream_mode));
            if(capture->in_burst) {
                snprintf(
                    model->burst,
                    sizeof(model->burst),
                    "Burst %s",
                    capture->first_level ? "Pulse" : "Gap");
            } else if(capture->buffered_valid) {
                snprintf(model->burst, sizeof(model->burst), "Burst Ready");
            } else {
                snprintf(model->burst, sizeof(model->burst), "Burst None");
            }
            snprintf(
                model->count,
                sizeof(model->count),
                "Count %lu",
                (unsigned long)(capture->in_burst ? capture->current_total_count :
                                                   capture->buffered_total_count));
            snprintf(model->rssi, sizeof(model->rssi), "RSSI %.1f", (double)capture->last_rssi);
            snprintf(
                model->flags,
                sizeof(model->flags),
                "Flags %s %s %s",
                capture->buffered_valid ? "BUF" : "NO-BUF",
                capture->buffered_truncated ? "TRUNC" : "OK",
                capture->overflow ? "OVF" : "CLR");
            model->running = capture->running;
            model->can_send = capture->buffered_valid;
        },
        true);
}
