#pragma once

#include "../app/fliprsdr.h"
#include <gui/view.h>

typedef enum {
    FlipRSDRPreviewViewActionFrequencyChanged,
} FlipRSDRPreviewViewAction;

typedef void (*FlipRSDRPreviewViewActionCallback)(
    FlipRSDRPreviewViewAction action,
    uint32_t frequency_hz,
    void* context);

typedef struct FlipRSDRPreviewView FlipRSDRPreviewView;

FlipRSDRPreviewView* fliprsdr_preview_view_alloc(void);
void fliprsdr_preview_view_free(FlipRSDRPreviewView* preview_view);
View* fliprsdr_preview_view_get_view(FlipRSDRPreviewView* preview_view);
void fliprsdr_preview_view_set_action_callback(
    FlipRSDRPreviewView* preview_view,
    FlipRSDRPreviewViewActionCallback callback,
    void* context);
void fliprsdr_preview_view_set_settings(
    FlipRSDRPreviewView* preview_view,
    const FlipRSDRSettings* settings);
