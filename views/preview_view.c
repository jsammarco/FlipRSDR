#include "preview_view.h"

#include "../app/settings.h"

#include <furi_hal.h>
#include <furi_hal_resources.h>
#include <gui/elements.h>
#include <input/input.h>
#include <lib/subghz/devices/cc1101_configs.h>

#define FLIPRSDR_PREVIEW_TIMER_HZ 20U
#define FLIPRSDR_PREVIEW_RSSI_MIN (-110.0f)
#define FLIPRSDR_PREVIEW_RSSI_MAX (-35.0f)

struct FlipRSDRPreviewView {
    View* view;
    FuriTimer* timer;
};

typedef struct {
    FlipRSDRSettings settings;
    uint8_t rssi[FLIPRSDR_PREVIEW_GRAPH_POINTS];
    uint8_t active_points;
    uint8_t sweep_index;
    bool running;
    bool valid;
    uint32_t current_frequency_hz;
    uint32_t center_frequency_hz;
    uint32_t start_frequency_hz;
    uint32_t end_frequency_hz;
    float last_rssi;
} FlipRSDRPreviewViewModel;

static uint32_t fliprsdr_preview_band_min(uint32_t center_frequency_hz) {
    if(center_frequency_hz < 348000336UL) return 299999755UL;
    if(center_frequency_hz < 464000001UL) return 386999938UL;
    return 778999847UL;
}

static uint32_t fliprsdr_preview_band_max(uint32_t center_frequency_hz) {
    if(center_frequency_hz < 348000336UL) return 348000335UL;
    if(center_frequency_hz < 464000001UL) return 464000000UL;
    return 928000000UL;
}

static uint8_t fliprsdr_preview_rssi_to_height(float rssi) {
    if(rssi <= FLIPRSDR_PREVIEW_RSSI_MIN) return 0U;
    if(rssi >= FLIPRSDR_PREVIEW_RSSI_MAX) return 28U;

    const float normalized =
        (rssi - FLIPRSDR_PREVIEW_RSSI_MIN) / (FLIPRSDR_PREVIEW_RSSI_MAX - FLIPRSDR_PREVIEW_RSSI_MIN);
    return (uint8_t)(normalized * 28.0f);
}

static uint32_t fliprsdr_preview_frequency_for_index(const FlipRSDRPreviewViewModel* model, uint8_t index) {
    if(model->active_points <= 1U) return model->center_frequency_hz;

    const uint32_t span_hz = model->end_frequency_hz - model->start_frequency_hz;
    return model->start_frequency_hz + ((uint64_t)span_hz * index) / (model->active_points - 1U);
}

static void fliprsdr_preview_setup_model(FlipRSDRPreviewViewModel* model) {
    memset(model->rssi, 0, sizeof(model->rssi));
    model->sweep_index = 0U;
    model->last_rssi = FLIPRSDR_PREVIEW_RSSI_MIN;
    model->center_frequency_hz = fliprsdr_settings_frequency_hz(&model->settings);

    if(!furi_hal_subghz_is_frequency_valid(model->center_frequency_hz)) {
        model->valid = false;
        model->active_points = 0U;
        model->start_frequency_hz = model->center_frequency_hz;
        model->end_frequency_hz = model->center_frequency_hz;
        model->current_frequency_hz = model->center_frequency_hz;
        return;
    }

    const uint32_t half_span_hz = ((uint32_t)model->settings.preview_bandwidth_khz * 1000UL) / 2UL;
    const uint32_t band_min_hz = fliprsdr_preview_band_min(model->center_frequency_hz);
    const uint32_t band_max_hz = fliprsdr_preview_band_max(model->center_frequency_hz);

    model->start_frequency_hz =
        (model->center_frequency_hz > half_span_hz) ? model->center_frequency_hz - half_span_hz : band_min_hz;
    model->end_frequency_hz = model->center_frequency_hz + half_span_hz;

    if(model->start_frequency_hz < band_min_hz) model->start_frequency_hz = band_min_hz;
    if(model->end_frequency_hz > band_max_hz) model->end_frequency_hz = band_max_hz;
    if(model->end_frequency_hz < model->start_frequency_hz) model->end_frequency_hz = model->start_frequency_hz;

    model->active_points = FLIPRSDR_PREVIEW_GRAPH_POINTS;
    model->current_frequency_hz = fliprsdr_preview_frequency_for_index(model, 0U);
    model->valid = true;
}

static void fliprsdr_preview_start(FlipRSDRPreviewView* preview_view) {
    furi_hal_subghz_reset();
    furi_hal_subghz_load_custom_preset(subghz_device_cc1101_preset_ook_650khz_async_regs);
    furi_hal_gpio_init(&gpio_cc1101_g0, GpioModeInput, GpioPullNo, GpioSpeedLow);
    furi_hal_power_suppress_charge_enter();

    with_view_model(
        preview_view->view,
        FlipRSDRPreviewViewModel * model,
        {
            fliprsdr_preview_setup_model(model);
            model->running = model->valid;
            if(model->valid) {
                model->current_frequency_hz =
                    furi_hal_subghz_set_frequency_and_path(model->current_frequency_hz);
                furi_hal_subghz_rx();
            }
        },
        true);

    furi_timer_start(
        preview_view->timer, furi_kernel_get_tick_frequency() / FLIPRSDR_PREVIEW_TIMER_HZ);
}

static void fliprsdr_preview_stop(FlipRSDRPreviewView* preview_view) {
    furi_timer_stop(preview_view->timer);
    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
    furi_hal_power_suppress_charge_exit();

    with_view_model(
        preview_view->view,
        FlipRSDRPreviewViewModel * model,
        { model->running = false; },
        true);
}

static void fliprsdr_preview_draw(Canvas* canvas, void* model_ptr) {
    FlipRSDRPreviewViewModel* model = model_ptr;
    char buffer[32];
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 9, "Signal Preview");

    canvas_set_font(canvas, FontSecondary);
    snprintf(
        buffer,
        sizeof(buffer),
        "%lu.%03luM",
        (unsigned long)(model->center_frequency_hz / 1000000UL),
        (unsigned long)((model->center_frequency_hz % 1000000UL) / 1000UL));
    canvas_draw_str(canvas, 2, 20, buffer);

    snprintf(buffer, sizeof(buffer), "Span %u kHz", model->settings.preview_bandwidth_khz);
    canvas_draw_str_aligned(canvas, 126, 20, AlignRight, AlignBottom, buffer);

    canvas_draw_frame(canvas, 2, 23, 124, 29);
    canvas_draw_line(canvas, 2, 52, 126, 52);

    if(model->valid && model->active_points > 0U) {
        for(uint8_t i = 0; i < model->active_points; i++) {
            const uint8_t bar_height = model->rssi[i];
            const uint8_t x = 4U + (uint8_t)(((uint16_t)i * 118U) / model->active_points);
            canvas_draw_line(canvas, x, 50, x, 50 - bar_height);
        }

        const uint8_t cursor_x =
            4U + (uint8_t)(((uint16_t)model->sweep_index * 118U) / model->active_points);
        canvas_draw_line(canvas, cursor_x, 24, cursor_x, 51);

        snprintf(
            buffer,
            sizeof(buffer),
            "Now %lu.%03lu",
            (unsigned long)(model->current_frequency_hz / 1000000UL),
            (unsigned long)((model->current_frequency_hz % 1000000UL) / 1000UL));
        canvas_draw_str(canvas, 2, 61, buffer);

        snprintf(buffer, sizeof(buffer), "RSSI %.1f", (double)model->last_rssi);
        canvas_draw_str_aligned(canvas, 126, 61, AlignRight, AlignBottom, buffer);
    } else {
        canvas_draw_str(canvas, 7, 39, "Selected frequency is out of range.");
    }

    elements_button_center(canvas, model->running ? "Live" : "Idle");
}

static bool fliprsdr_preview_input(InputEvent* event, void* context) {
    UNUSED(context);
    return event->key != InputKeyBack;
}

static void fliprsdr_preview_tick(void* context) {
    FlipRSDRPreviewView* preview_view = context;

    with_view_model(
        preview_view->view,
        FlipRSDRPreviewViewModel * model,
        {
            if(!model->running || !model->valid || (model->active_points == 0U)) {
                return;
            }

            const uint8_t sample_index = model->sweep_index;
            model->last_rssi = furi_hal_subghz_get_rssi();
            model->rssi[sample_index] = fliprsdr_preview_rssi_to_height(model->last_rssi);

            model->sweep_index = (sample_index + 1U) % model->active_points;
            model->current_frequency_hz =
                fliprsdr_preview_frequency_for_index(model, model->sweep_index);

            furi_hal_subghz_idle();
            model->current_frequency_hz =
                furi_hal_subghz_set_frequency_and_path(model->current_frequency_hz);
            furi_hal_subghz_rx();
        },
        true);
}

static void fliprsdr_preview_enter(void* context) {
    fliprsdr_preview_start(context);
}

static void fliprsdr_preview_exit(void* context) {
    fliprsdr_preview_stop(context);
}

FlipRSDRPreviewView* fliprsdr_preview_view_alloc(void) {
    FlipRSDRPreviewView* preview_view = malloc(sizeof(FlipRSDRPreviewView));
    preview_view->view = view_alloc();
    preview_view->timer =
        furi_timer_alloc(fliprsdr_preview_tick, FuriTimerTypePeriodic, preview_view);

    view_allocate_model(
        preview_view->view, ViewModelTypeLocking, sizeof(FlipRSDRPreviewViewModel));
    view_set_context(preview_view->view, preview_view);
    view_set_draw_callback(preview_view->view, fliprsdr_preview_draw);
    view_set_input_callback(preview_view->view, fliprsdr_preview_input);
    view_set_enter_callback(preview_view->view, fliprsdr_preview_enter);
    view_set_exit_callback(preview_view->view, fliprsdr_preview_exit);

    with_view_model(
        preview_view->view,
        FlipRSDRPreviewViewModel * model,
        { fliprsdr_settings_load_defaults(&model->settings); },
        true);

    return preview_view;
}

void fliprsdr_preview_view_free(FlipRSDRPreviewView* preview_view) {
    furi_assert(preview_view);
    furi_timer_free(preview_view->timer);
    view_free(preview_view->view);
    free(preview_view);
}

View* fliprsdr_preview_view_get_view(FlipRSDRPreviewView* preview_view) {
    furi_assert(preview_view);
    return preview_view->view;
}

void fliprsdr_preview_view_set_settings(
    FlipRSDRPreviewView* preview_view,
    const FlipRSDRSettings* settings) {
    furi_assert(preview_view);
    furi_assert(settings);

    with_view_model(
        preview_view->view,
        FlipRSDRPreviewViewModel * model,
        { model->settings = *settings; },
        true);
}
