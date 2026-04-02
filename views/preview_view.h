#pragma once

#include "../app/fliprsdr.h"
#include <gui/view.h>

typedef struct FlipRSDRPreviewView FlipRSDRPreviewView;

FlipRSDRPreviewView* fliprsdr_preview_view_alloc(void);
void fliprsdr_preview_view_free(FlipRSDRPreviewView* preview_view);
View* fliprsdr_preview_view_get_view(FlipRSDRPreviewView* preview_view);
void fliprsdr_preview_view_set_settings(
    FlipRSDRPreviewView* preview_view,
    const FlipRSDRSettings* settings);
