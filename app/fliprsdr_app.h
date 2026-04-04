#pragma once

#include "fliprsdr.h"
#include "capture.h"
#include "settings.h"
#include "transport.h"
#include "../views/capture_view.h"
#include "../views/frequency_editor_view.h"
#include "../views/preview_view.h"
#include "../scenes/scenes.h"

#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <gui/scene_manager.h>

typedef enum {
    FlipRSDRViewSubmenu,
    FlipRSDRViewCapture,
    FlipRSDRViewPreview,
    FlipRSDRViewSettings,
    FlipRSDRViewFrequencyEditor,
    FlipRSDRViewAbout,
} FlipRSDRView;

typedef enum {
    FlipRSDRCustomEventMenuCapture = 1,
    FlipRSDRCustomEventMenuPreview,
    FlipRSDRCustomEventMenuSettings,
    FlipRSDRCustomEventMenuAbout,
    FlipRSDRCustomEventCaptureToggle,
    FlipRSDRCustomEventCaptureClear,
    FlipRSDRCustomEventCaptureSend,
    FlipRSDRCustomEventSettingsDebugSend,
    FlipRSDRCustomEventFrequencyEditorSave,
    FlipRSDRCustomEventFrequencyEditorCancel,
} FlipRSDRCustomEvent;

typedef struct {
    char line[FLIPRSDR_COMMAND_LINE_MAX];
} FlipRSDRCommandMessage;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Submenu* submenu;
    VariableItemList* variable_item_list;
    Widget* about_widget;
    FlipRSDRCaptureView* capture_view;
    FlipRSDRFrequencyEditorView* frequency_editor_view;
    FlipRSDRPreviewView* preview_view;
    FlipRSDRSettings settings;
    bool settings_dirty;
    bool transport_dirty;
    FuriMessageQueue* command_queue;
    FlipRSDRTransport* transport;
    FlipRSDRCapture* capture;
} FlipRSDRApp;

void fliprsdr_app_apply_settings(FlipRSDRApp* app, bool reinit_transport);
void fliprsdr_app_refresh_capture_view(FlipRSDRApp* app);
