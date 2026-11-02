#include "thor_gamepad.h"
#include "../games/totk/runtime/runtime.h"
#include "../games/totk/runtime/controller_map.h"
#include <pthread.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static RuntimeGamepadState state;
static uint64_t key_physical, motion_physical, pressed_edges;
static uint16_t mapping[CONTROLLER_BUTTONS];
static uint8_t modes[CONTROLLER_BUTTONS];
static int started;

static int32_t stick(float value) {
    const float deadzone = 0.12f;
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    float magnitude = value < 0 ? -value : value;
    if (magnitude <= deadzone) return 0;
    int32_t result = (int32_t)((magnitude - deadzone) * (32767.0f / (1.0f - deadzone)));
    return value < 0 ? -result : result;
}

static void initialize(void) {
    if (!started) {
        controller_map_default(mapping, modes);
        started = 1;
    }
}

void runtime_gamepad_start(void) {
    pthread_mutex_lock(&lock);
    initialize();
    pthread_mutex_unlock(&lock);
}

void runtime_gamepad_clear(void) {
    pthread_mutex_lock(&lock);
    state = (RuntimeGamepadState){0};
    key_physical = motion_physical = pressed_edges = 0;
    pthread_mutex_unlock(&lock);
}

void runtime_gamepad_read(RuntimeGamepadState *out) {
    pthread_mutex_lock(&lock);
    *out = state;
    out->buttons |= pressed_edges;
    pressed_edges = 0;
    pthread_mutex_unlock(&lock);
}

static int button_index(int code) {
    switch (code) {
        /* The Thor reports its button labels (Nintendo order: A right, B bottom,
           X top, Y left); the controller map works by position. */
        case AKEYCODE_BUTTON_A: return 1;  /* east  -> Switch A */
        case AKEYCODE_BUTTON_B: return 0;  /* south -> Switch B */
        case AKEYCODE_BUTTON_X: return 3;  /* north -> Switch X */
        case AKEYCODE_BUTTON_Y: return 2;  /* west  -> Switch Y */
        case AKEYCODE_BUTTON_THUMBL: return 4;
        case AKEYCODE_BUTTON_THUMBR: return 5;
        case AKEYCODE_BUTTON_L1: return 6;
        case AKEYCODE_BUTTON_R1: return 7;
        case AKEYCODE_BUTTON_L2: return 8;
        case AKEYCODE_BUTTON_R2: return 9;
        case AKEYCODE_BUTTON_START: return 10;
        case AKEYCODE_BUTTON_SELECT: return 11;
        case AKEYCODE_DPAD_LEFT: return 12;
        case AKEYCODE_DPAD_UP: return 13;
        case AKEYCODE_DPAD_RIGHT: return 14;
        case AKEYCODE_DPAD_DOWN: return 15;
        default: return -1;
    }
}

static void set_button(uint64_t *physical, int index, int down) {
    uint64_t mask = 1ull << index;
    if (down) *physical |= mask;
    else *physical &= ~mask;
}

static void update_buttons(void) {
    uint64_t next = controller_map_apply(mapping, key_physical | motion_physical);
    pressed_edges |= next & ~state.buttons;
    state.buttons = next;
}

int android_gamepad_event(AInputEvent *event) {
    int source = AInputEvent_getSource(event);
    int type = AInputEvent_getType(event);
    if (!(source & (AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK))) return 0;
    pthread_mutex_lock(&lock);
    initialize();
    int handled = 0;
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int index = button_index(AKeyEvent_getKeyCode(event));
        if (index >= 0) {
            int action = AKeyEvent_getAction(event);
            if (action == AKEY_EVENT_ACTION_DOWN || action == AKEY_EVENT_ACTION_UP) {
                set_button(&key_physical, index, action == AKEY_EVENT_ACTION_DOWN);
                update_buttons();
                state.connected = 1;
                handled = 1;
            }
        }
    } else if (type == AINPUT_EVENT_TYPE_MOTION && (source & AINPUT_SOURCE_JOYSTICK)) {
        state.lx = stick(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0));
        state.ly = -stick(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0));
        state.rx = stick(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0));
        state.ry = -stick(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0));
        set_button(&motion_physical, 8, AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0) > 0.55f);
        set_button(&motion_physical, 9, AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0) > 0.55f);
        float hat_x = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
        float hat_y = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
        set_button(&motion_physical, 12, hat_x < -0.5f); set_button(&motion_physical, 14, hat_x > 0.5f);
        set_button(&motion_physical, 13, hat_y < -0.5f); set_button(&motion_physical, 15, hat_y > 0.5f);
        update_buttons();
        state.connected = 1;
        handled = 1;
    }
    pthread_mutex_unlock(&lock);
    return handled;
}
