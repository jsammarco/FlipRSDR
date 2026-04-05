#include "preview_view.h"

#include "../app/radio.h"
#include "../app/settings.h"

#include <furi_hal.h>
#include <gui/elements.h>
#include <input/input.h>
#include <string.h>

#define FLIPRSDR_PREVIEW_TIMER_HZ 40U
#define FLIPRSDR_PREVIEW_RSSI_MIN (-110.0f)
#define FLIPRSDR_PREVIEW_RSSI_MAX (-35.0f)
#define FLIPRSDR_PREVIEW_FINE_STEP_HZ      100000UL
#define FLIPRSDR_PREVIEW_COARSE_STEP_HZ    1000000UL

struct FlipRSDRPreviewView {
    View* view;
    FuriTimer* timer;
    FlipRSDRRadio radio;
    bool charge_suppressed;
    bool async_rx_running;
    bool audio_speaker_owned;
    FlipRSDRPreviewViewActionCallback callback;
    void* context;
};

typedef struct {
    FlipRSDRSettings settings;
    uint8_t rssi[FLIPRSDR_PREVIEW_GRAPH_POINTS];
    uint8_t active_points;
    uint8_t sweep_index;
    bool running;
    bool valid;
    bool radio_available;
    bool external_requested;
    bool external_active;
    FlipRSDRRadioOpenStatus radio_status;
    const char* data_pin_label;
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

static void fliprsdr_preview_rx_callback(bool level, uint32_t duration, void* context) {
    UNUSED(level);
    UNUSED(duration);
    UNUSED(context);
}

static uint32_t
    fliprsdr_preview_frequency_for_index(const FlipRSDRPreviewViewModel* model, uint8_t index) {
    if(model->active_points <= 1U) return model->center_frequency_hz;

    const uint32_t span_hz = model->end_frequency_hz - model->start_frequency_hz;
    return model->start_frequency_hz + ((uint64_t)span_hz * index) / (model->active_points - 1U);
}

static bool fliprsdr_preview_start_rx_at(
    FlipRSDRPreviewView* preview_view,
    FlipRSDRPreviewViewModel* model,
    uint32_t frequency_hz) {
    if(!fliprsdr_radio_device(&preview_view->radio) ||
       !fliprsdr_radio_is_frequency_valid(&preview_view->radio, frequency_hz)) {
        return false;
    }

    const uint32_t tuned_frequency_hz =
        fliprsdr_radio_set_frequency(&preview_view->radio, frequency_hz);
    if(!fliprsdr_radio_is_frequency_valid(&preview_view->radio, tuned_frequency_hz)) {
        return false;
    }

    model->current_frequency_hz = tuned_frequency_hz;
    fliprsdr_radio_start_async_rx(
        &preview_view->radio, fliprsdr_preview_rx_callback, preview_view);
    preview_view->async_rx_running = true;
    return true;
}

static void fliprsdr_preview_stop_async_rx(FlipRSDRPreviewView* preview_view) {
    if(!preview_view->async_rx_running) {
        return;
    }

    fliprsdr_radio_stop_async_rx(&preview_view->radio);
    preview_view->async_rx_running = false;
}

static void fliprsdr_preview_set_audio_enabled(FlipRSDRPreviewView* preview_view, bool enabled) {
    if(enabled) {
        if(preview_view->audio_speaker_owned) {
            fliprsdr_radio_set_audio_mirror(&preview_view->radio, true);
            return;
        }

        preview_view->audio_speaker_owned = furi_hal_speaker_acquire(100);
        if(preview_view->audio_speaker_owned) {
            fliprsdr_radio_set_audio_mirror(&preview_view->radio, true);
        }
        return;
    }

    fliprsdr_radio_set_audio_mirror(&preview_view->radio, false);
    if(preview_view->audio_speaker_owned) {
        furi_hal_speaker_release();
        preview_view->audio_speaker_owned = false;
    }
}

static void fliprsdr_preview_setup_model(
    FlipRSDRPreviewViewModel* model,
    const FlipRSDRRadio* radio,
    const SubGhzDevice* radio_device) {
    memset(model->rssi, 0, sizeof(model->rssi));
    model->sweep_index = 0U;
    model->last_rssi = FLIPRSDR_PREVIEW_RSSI_MIN;
    model->center_frequency_hz = fliprsdr_settings_frequency_hz(&model->settings);
    model->radio_available = radio_device != NULL;
    model->external_requested = model->settings.external_radio_module;
    model->external_active = model->radio_available && fliprsdr_radio_is_external_selected(radio);
    model->radio_status = fliprsdr_radio_open_status(radio);
    model->data_pin_label = fliprsdr_radio_data_pin_label(radio);

    if(!radio_device) {
        model->valid = false;
        model->active_points = 0U;
        model->start_frequency_hz = model->center_frequency_hz;
        model->end_frequency_hz = model->center_frequency_hz;
        model->current_frequency_hz = model->center_frequency_hz;
        return;
    }

    if(!fliprsdr_radio_is_frequency_valid(radio, model->center_frequency_hz)) {
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
    bool audio_enabled = false;
    bool preview_running = false;
    FlipRSDRSettings settings;

    with_view_model(
        preview_view->view,
        FlipRSDRPreviewViewModel * model,
        { settings = model->settings; },
        false);

    const bool radio_ready = fliprsdr_radio_open(&preview_view->radio, &settings);
    const SubGhzDevice* radio_device = fliprsdr_radio_device(&preview_view->radio);
    preview_view->charge_suppressed = false;
    preview_view->async_rx_running = false;
    if(radio_ready && radio_device) {
        furi_hal_power_suppress_charge_enter();
        preview_view->charge_suppressed = true;
        fliprsdr_radio_reset(&preview_view->radio);
        fliprsdr_radio_load_preset(&preview_view->radio, FuriHalSubGhzPresetOok650Async);
    }

    with_view_model(
        preview_view->view,
        FlipRSDRPreviewViewModel * model,
        {
            fliprsdr_preview_setup_model(model, &preview_view->radio, radio_device);
            model->running = model->valid;
            if(model->valid) {
                model->running =
                    fliprsdr_preview_start_rx_at(preview_view, model, model->current_frequency_hz);
            }
            preview_running = model->running;
            audio_enabled = model->settings.preview_audio && model->running;
        },
        true);

    fliprsdr_preview_set_audio_enabled(preview_view, audio_enabled);

    if(radio_ready && radio_device && preview_running) {
        furi_timer_start(
            preview_view->timer, furi_kernel_get_tick_frequency() / FLIPRSDR_PREVIEW_TIMER_HZ);
    }
}

static void fliprsdr_preview_stop(FlipRSDRPreviewView* preview_view) {
    furi_timer_stop(preview_view->timer);
    fliprsdr_preview_stop_async_rx(preview_view);
    fliprsdr_preview_set_audio_enabled(preview_view, false);
    fliprsdr_radio_close(&preview_view->radio);
    if(preview_view->charge_suppressed) {
        furi_hal_power_suppress_charge_exit();
        preview_view->charge_suppressed = false;
    }

    with_view_model(
        preview_view->view,
        FlipRSDRPreviewViewModel * model,
        {
            model->running = false;
            model->radio_available = false;
            model->external_active = false;
        },
        true);
}

static bool fliprsdr_preview_shift_frequency(FlipRSDRPreviewView* preview_view, int32_t delta_hz) {
    FlipRSDRSettings settings;

    with_view_model(
        preview_view->view,
        FlipRSDRPreviewViewModel * model,
        { settings = model->settings; },
        false);

    const int64_t next_frequency_hz =
        (int64_t)fliprsdr_settings_frequency_hz(&settings) + (int64_t)delta_hz;
    if((next_frequency_hz <= 0) || (next_frequency_hz > UINT32_MAX) ||
       !fliprsdr_settings_set_frequency_hz(&settings, (uint32_t)next_frequency_hz)) {
        return false;
    }

    fliprsdr_preview_stop(preview_view);
    fliprsdr_preview_view_set_settings(preview_view, &settings);
    fliprsdr_preview_start(preview_view);

    if(preview_view->callback) {
        preview_view->callback(
            FlipRSDRPreviewViewActionFrequencyChanged,
            settings.frequency_hz,
            preview_view->context);
    }

    return true;
}

static void fliprsdr_preview_draw(Canvas* canvas, void* model_ptr) {
    FlipRSDRPreviewViewModel* model = model_ptr;
    char buffer[32];
    const char* module_status = model->external_active ? "EXT" :
                                (model->external_requested ? "EXT?" : "INT");
    const char* radio_status = fliprsdr_radio_open_status_label(model->radio_status);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 9, "Signal Preview");
    canvas_draw_str_aligned(canvas, 126, 9, AlignRight, AlignBottom, module_status);

    canvas_set_font(canvas, FontSecondary);
    snprintf(
        buffer,
        sizeof(buffer),
        "%lu.%03luM",
        (unsigned long)(model->center_frequency_hz / 1000000UL),
        (unsigned long)((model->center_frequency_hz % 1000000UL) / 1000UL));
    canvas_draw_str(canvas, 2, 20, buffer);

    snprintf(
        buffer,
        sizeof(buffer),
        "%s %s",
        model->external_requested ? "GDO0" : "Span",
        model->external_requested ? model->data_pin_label : "");
    if(model->external_requested) {
        canvas_draw_str_aligned(canvas, 126, 20, AlignRight, AlignBottom, buffer);
    } else {
        snprintf(buffer, sizeof(buffer), "Span %u kHz", model->settings.preview_bandwidth_khz);
        canvas_draw_str_aligned(canvas, 126, 20, AlignRight, AlignBottom, buffer);
    }
    if(model->external_requested) {
        snprintf(buffer, sizeof(buffer), "Span %u kHz", model->settings.preview_bandwidth_khz);
        canvas_draw_str(canvas, 2, 20, buffer);
    }

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
    } else if(model->external_requested && !model->radio_available) {
        canvas_draw_str(canvas, 7, 35, radio_status);
        canvas_draw_str(canvas, 7, 45, "Check power/SPI/GDO0.");
    } else if(!model->radio_available) {
        canvas_draw_str(canvas, 7, 39, radio_status);
    } else {
        canvas_draw_str(canvas, 7, 39, "Selected frequency is out of range.");
    }

    elements_button_left(canvas, "-100k");
    elements_button_right(canvas, "+100k");
}

static bool fliprsdr_preview_input(InputEvent* event, void* context) {
    FlipRSDRPreviewView* preview_view = context;
    if(event->type != InputTypeShort) {
        return false;
    }

    switch(event->key) {
    case InputKeyLeft:
        return fliprsdr_preview_shift_frequency(preview_view, -(int32_t)FLIPRSDR_PREVIEW_FINE_STEP_HZ);
    case InputKeyRight:
        return fliprsdr_preview_shift_frequency(preview_view, (int32_t)FLIPRSDR_PREVIEW_FINE_STEP_HZ);
    case InputKeyUp:
        return fliprsdr_preview_shift_frequency(preview_view, (int32_t)FLIPRSDR_PREVIEW_COARSE_STEP_HZ);
    case InputKeyDown:
        return fliprsdr_preview_shift_frequency(preview_view, -(int32_t)FLIPRSDR_PREVIEW_COARSE_STEP_HZ);
    case InputKeyBack:
        return false;
    default:
        return false;
    }
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
            const SubGhzDevice* radio_device = fliprsdr_radio_device(&preview_view->radio);
            model->last_rssi = radio_device ? fliprsdr_radio_get_rssi(&preview_view->radio) :
                                             FLIPRSDR_PREVIEW_RSSI_MIN;
            model->rssi[sample_index] = fliprsdr_preview_rssi_to_height(model->last_rssi);

            model->sweep_index = (sample_index + 1U) % model->active_points;
            fliprsdr_preview_stop_async_rx(preview_view);
            if(radio_device) {
                fliprsdr_radio_idle(&preview_view->radio);
            }
            if(!fliprsdr_preview_start_rx_at(
                   preview_view,
                   model,
                   fliprsdr_preview_frequency_for_index(model, model->sweep_index))) {
                model->running = false;
                model->valid = false;
                fliprsdr_preview_stop_async_rx(preview_view);
                fliprsdr_preview_set_audio_enabled(preview_view, false);
                furi_timer_stop(preview_view->timer);
            }
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
    fliprsdr_radio_init(&preview_view->radio);
    preview_view->charge_suppressed = false;
    preview_view->async_rx_running = false;
    preview_view->audio_speaker_owned = false;
    preview_view->callback = NULL;
    preview_view->context = NULL;

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
    fliprsdr_preview_stop(preview_view);
    fliprsdr_radio_close(&preview_view->radio);
    furi_timer_free(preview_view->timer);
    view_free(preview_view->view);
    free(preview_view);
}

View* fliprsdr_preview_view_get_view(FlipRSDRPreviewView* preview_view) {
    furi_assert(preview_view);
    return preview_view->view;
}

void fliprsdr_preview_view_set_action_callback(
    FlipRSDRPreviewView* preview_view,
    FlipRSDRPreviewViewActionCallback callback,
    void* context) {
    furi_assert(preview_view);
    preview_view->callback = callback;
    preview_view->context = context;
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
