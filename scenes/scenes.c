#include "scenes.h"

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
void (*const fliprsdr_scene_on_enter_handlers[])(void*) = {
#include "../app/scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
bool (*const fliprsdr_scene_on_event_handlers[])(void* context, SceneManagerEvent event) = {
#include "../app/scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
void (*const fliprsdr_scene_on_exit_handlers[])(void* context) = {
#include "../app/scene_config.h"
};
#undef ADD_SCENE

const SceneManagerHandlers fliprsdr_scene_handlers = {
    .on_enter_handlers = fliprsdr_scene_on_enter_handlers,
    .on_event_handlers = fliprsdr_scene_on_event_handlers,
    .on_exit_handlers = fliprsdr_scene_on_exit_handlers,
    .scene_num = FlipRSDRSceneCount,
};
