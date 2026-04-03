#pragma once

#include <gui/view.h>
#include <stdint.h>

typedef enum {
    FlipRSDRFrequencyEditorActionCancel,
    FlipRSDRFrequencyEditorActionSave,
} FlipRSDRFrequencyEditorAction;

typedef void (*FlipRSDRFrequencyEditorActionCallback)(
    FlipRSDRFrequencyEditorAction action,
    uint32_t frequency_hz,
    void* context);

typedef struct FlipRSDRFrequencyEditorView FlipRSDRFrequencyEditorView;

FlipRSDRFrequencyEditorView* fliprsdr_frequency_editor_view_alloc(void);
void fliprsdr_frequency_editor_view_free(FlipRSDRFrequencyEditorView* frequency_editor_view);
View* fliprsdr_frequency_editor_view_get_view(FlipRSDRFrequencyEditorView* frequency_editor_view);
void fliprsdr_frequency_editor_view_set_action_callback(
    FlipRSDRFrequencyEditorView* frequency_editor_view,
    FlipRSDRFrequencyEditorActionCallback callback,
    void* context);
void fliprsdr_frequency_editor_view_set_frequency(
    FlipRSDRFrequencyEditorView* frequency_editor_view,
    uint32_t frequency_hz);
