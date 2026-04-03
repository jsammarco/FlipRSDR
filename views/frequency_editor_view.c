#include "frequency_editor_view.h"

#include <furi.h>
#include <furi_hal.h>
#include <gui/elements.h>
#include <input/input.h>
#include <stdio.h>

struct FlipRSDRFrequencyEditorView {
    View* view;
    FlipRSDRFrequencyEditorActionCallback callback;
    void* context;
};

typedef struct {
    uint8_t digits[6];
    uint8_t selected_digit;
    bool valid;
} FlipRSDRFrequencyEditorViewModel;

static uint32_t fliprsdr_frequency_editor_digits_to_hz(const uint8_t digits[6]) {
    return ((uint32_t)digits[0] * 100000000UL) + ((uint32_t)digits[1] * 10000000UL) +
           ((uint32_t)digits[2] * 1000000UL) + ((uint32_t)digits[3] * 100000UL) +
           ((uint32_t)digits[4] * 10000UL) + ((uint32_t)digits[5] * 1000UL);
}

static void fliprsdr_frequency_editor_hz_to_digits(uint32_t frequency_hz, uint8_t digits[6]) {
    uint32_t mhz_thousandths = frequency_hz / 1000UL;
    digits[0] = (mhz_thousandths / 100000U) % 10U;
    digits[1] = (mhz_thousandths / 10000U) % 10U;
    digits[2] = (mhz_thousandths / 1000U) % 10U;
    digits[3] = (mhz_thousandths / 100U) % 10U;
    digits[4] = (mhz_thousandths / 10U) % 10U;
    digits[5] = mhz_thousandths % 10U;
}

static void fliprsdr_frequency_editor_update_valid(FlipRSDRFrequencyEditorViewModel* model) {
    model->valid = furi_hal_subghz_is_frequency_valid(
        fliprsdr_frequency_editor_digits_to_hz(model->digits));
}

static void fliprsdr_frequency_editor_change_digit(
    FlipRSDRFrequencyEditorViewModel* model,
    int8_t delta) {
    const uint8_t index = model->selected_digit;
    int8_t next = (int8_t)model->digits[index] + delta;
    if(next < 0) {
        next = 9;
    } else if(next > 9) {
        next = 0;
    }
    model->digits[index] = (uint8_t)next;
    fliprsdr_frequency_editor_update_valid(model);
}

static void fliprsdr_frequency_editor_draw_digit(
    Canvas* canvas,
    uint8_t x,
    uint8_t y,
    uint8_t digit,
    bool selected) {
    char str[2] = {(char)('0' + digit), '\0'};
    if(selected) {
        canvas_draw_rbox(canvas, x - 2, y - 10, 10, 13, 2);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_str(canvas, x, y, str);
    if(selected) {
        canvas_set_color(canvas, ColorBlack);
    }
}

static void fliprsdr_frequency_editor_draw(Canvas* canvas, void* model_ptr) {
    FlipRSDRFrequencyEditorViewModel* model = model_ptr;
    char buffer[32];

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Set Frequency");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 21, "Long Press OK to Save");

    canvas_set_font(canvas, FontKeyboard);
    fliprsdr_frequency_editor_draw_digit(
        canvas, 24, 39, model->digits[0], model->selected_digit == 0U);
    fliprsdr_frequency_editor_draw_digit(
        canvas, 35, 39, model->digits[1], model->selected_digit == 1U);
    fliprsdr_frequency_editor_draw_digit(
        canvas, 46, 39, model->digits[2], model->selected_digit == 2U);
    canvas_draw_str(canvas, 56, 39, ".");
    fliprsdr_frequency_editor_draw_digit(
        canvas, 66, 39, model->digits[3], model->selected_digit == 3U);
    fliprsdr_frequency_editor_draw_digit(
        canvas, 77, 39, model->digits[4], model->selected_digit == 4U);
    fliprsdr_frequency_editor_draw_digit(
        canvas, 88, 39, model->digits[5], model->selected_digit == 5U);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 98, 39, "MHz");

    snprintf(
        buffer,
        sizeof(buffer),
        "%lu.%03lu MHz",
        (unsigned long)(fliprsdr_frequency_editor_digits_to_hz(model->digits) / 1000000UL),
        (unsigned long)((fliprsdr_frequency_editor_digits_to_hz(model->digits) % 1000000UL) / 1000UL));
    canvas_draw_str(canvas, 20, 53, buffer);

    if(model->valid) {
        canvas_draw_str(canvas, 2, 63, "Back to cancel");
    } else {
        canvas_draw_str(canvas, 2, 63, "Out of range");
    }
}

static bool fliprsdr_frequency_editor_input(InputEvent* event, void* context) {
    FlipRSDRFrequencyEditorView* frequency_editor_view = context;
    if((event->type != InputTypeShort) && (event->type != InputTypeLong) &&
       (event->type != InputTypeRepeat)) {
        return false;
    }

    bool consumed = true;
    with_view_model(
        frequency_editor_view->view,
        FlipRSDRFrequencyEditorViewModel * model,
        {
            switch(event->key) {
            case InputKeyLeft:
                if(model->selected_digit > 0U) {
                    model->selected_digit--;
                }
                break;
            case InputKeyRight:
                if(model->selected_digit < 5U) {
                    model->selected_digit++;
                }
                break;
            case InputKeyUp:
                fliprsdr_frequency_editor_change_digit(model, 1);
                break;
            case InputKeyDown:
                fliprsdr_frequency_editor_change_digit(model, -1);
                break;
            case InputKeyOk:
                if((event->type == InputTypeLong) && model->valid &&
                   frequency_editor_view->callback) {
                    frequency_editor_view->callback(
                        FlipRSDRFrequencyEditorActionSave,
                        fliprsdr_frequency_editor_digits_to_hz(model->digits),
                        frequency_editor_view->context);
                }
                break;
            case InputKeyBack:
                if(frequency_editor_view->callback) {
                    frequency_editor_view->callback(
                        FlipRSDRFrequencyEditorActionCancel,
                        fliprsdr_frequency_editor_digits_to_hz(model->digits),
                        frequency_editor_view->context);
                }
                break;
            default:
                consumed = false;
                break;
            }
        },
        true);

    return consumed;
}

FlipRSDRFrequencyEditorView* fliprsdr_frequency_editor_view_alloc(void) {
    FlipRSDRFrequencyEditorView* frequency_editor_view = malloc(sizeof(FlipRSDRFrequencyEditorView));
    frequency_editor_view->view = view_alloc();
    frequency_editor_view->callback = NULL;
    frequency_editor_view->context = NULL;

    view_allocate_model(
        frequency_editor_view->view,
        ViewModelTypeLocking,
        sizeof(FlipRSDRFrequencyEditorViewModel));
    view_set_context(frequency_editor_view->view, frequency_editor_view);
    view_set_draw_callback(frequency_editor_view->view, fliprsdr_frequency_editor_draw);
    view_set_input_callback(frequency_editor_view->view, fliprsdr_frequency_editor_input);

    with_view_model(
        frequency_editor_view->view,
        FlipRSDRFrequencyEditorViewModel * model,
        {
            fliprsdr_frequency_editor_hz_to_digits(433920000UL, model->digits);
            model->selected_digit = 0U;
            fliprsdr_frequency_editor_update_valid(model);
        },
        true);

    return frequency_editor_view;
}

void fliprsdr_frequency_editor_view_free(FlipRSDRFrequencyEditorView* frequency_editor_view) {
    furi_assert(frequency_editor_view);
    view_free(frequency_editor_view->view);
    free(frequency_editor_view);
}

View* fliprsdr_frequency_editor_view_get_view(FlipRSDRFrequencyEditorView* frequency_editor_view) {
    furi_assert(frequency_editor_view);
    return frequency_editor_view->view;
}

void fliprsdr_frequency_editor_view_set_action_callback(
    FlipRSDRFrequencyEditorView* frequency_editor_view,
    FlipRSDRFrequencyEditorActionCallback callback,
    void* context) {
    furi_assert(frequency_editor_view);
    frequency_editor_view->callback = callback;
    frequency_editor_view->context = context;
}

void fliprsdr_frequency_editor_view_set_frequency(
    FlipRSDRFrequencyEditorView* frequency_editor_view,
    uint32_t frequency_hz) {
    furi_assert(frequency_editor_view);
    with_view_model(
        frequency_editor_view->view,
        FlipRSDRFrequencyEditorViewModel * model,
        {
            fliprsdr_frequency_editor_hz_to_digits(frequency_hz, model->digits);
            model->selected_digit = 0U;
            fliprsdr_frequency_editor_update_valid(model);
        },
        true);
}
