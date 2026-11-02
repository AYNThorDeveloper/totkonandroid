/* Android HID host: the game-facing controller functions of the macOS
 * runtime (games/totk/runtime/input.c), backed by the Thor's built-in
 * controller through thor_gamepad.c. The state ABI follows libnx HID. The
 * macOS keyboard, scripted-input and screenshot diagnostics are not ported. */
#include "runtime.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, pad; } NpadState;
_Static_assert(sizeof(NpadState) == 40, "Npad full-key ABI");
typedef struct { int64_t sampling; int32_t x, y, dx, dy, wx, wy; uint32_t buttons, attributes; } MouseState;
typedef struct { int64_t sampling; uint32_t modifiers, attributes; uint64_t keys[4]; } KeyboardState;
_Static_assert(sizeof(MouseState) == 40 && sizeof(KeyboardState) == 48, "mouse/keyboard ABI");
typedef struct { int8_t players; uint8_t pad[3]; uint32_t selected; } ControllerSupportResult;
_Static_assert(sizeof(ControllerSupportResult) == 8, "controller support result ABI");

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t styles = 1;
static bool enabled, allowed = true, mouse_ready, keyboard_ready;
static int64_t sample;

void runtime_input_clear(void) { runtime_gamepad_clear(); }

static void initialize(void) {
    runtime_gamepad_start();
    pthread_mutex_lock(&lock); enabled = true; pthread_mutex_unlock(&lock);
    fprintf(stderr, "[android-input] Thor controller initialized as %s\n",
        runtime_handheld_mode() ? "handheld controller" : "player 1 full-key controller");
}
/* Docked: one full-key controller as player 1 (id 0, style bit 0). Handheld:
   the handheld controller (id 0x20, style bit 1), as in the macOS runtime. */
static uint32_t pad_id(void) { return runtime_handheld_mode() ? 0x20u : 0u; }
static uint32_t pad_style(void) { return runtime_handheld_mode() ? 2u : 1u; }
static void supported_ids(const uint32_t *ids, uint64_t n) {
    if (n > 10 || (!ids && n)) runtime_fail("invalid controller ids");
    bool found = false;
    for (uint64_t i = 0; i < n; i++) found |= ids[i] == pad_id();
    pthread_mutex_lock(&lock); allowed = found; pthread_mutex_unlock(&lock);
}
static void supported_styles(uint32_t value) { pthread_mutex_lock(&lock); styles = value; pthread_mutex_unlock(&lock); }
static uint32_t style(const uint32_t *id) {
    if (!id) runtime_fail("controller id pointer");
    pthread_mutex_lock(&lock);
    uint32_t v = enabled && allowed && *id == pad_id() ? (styles & pad_style()) : 0;
    pthread_mutex_unlock(&lock);
    return v;
}
static int pad_states(NpadState *out, int max, const uint32_t *id, uint32_t want_style) {
    if (max <= 0) return 0;
    if (!out || !id) runtime_fail("controller output pointer");
    pthread_mutex_lock(&lock);
    NpadState state = {.sampling = ++sample};
    if (enabled && allowed && *id == pad_id() && (styles & want_style) && want_style == pad_style()) {
        RuntimeGamepadState pad;
        runtime_gamepad_read(&pad);
        state.attributes = 1;   /* connected */
        state.buttons = pad.buttons;
        static uint64_t last_buttons;
        if (pad.buttons && pad.buttons != last_buttons) fprintf(stderr, "[android-input] buttons %#llx stick %d,%d\n", (unsigned long long)pad.buttons, pad.lx, pad.ly);
        last_buttons = pad.buttons;
        state.lx = pad.lx; state.ly = pad.ly; state.rx = pad.rx; state.ry = pad.ry;
    }
    *out = state;
    pthread_mutex_unlock(&lock);
    return 1;
}
static int full_states(NpadState *out, int max, const uint32_t *id) { return pad_states(out, max, id, 1); }
static int handheld_states(NpadState *out, int max, const uint32_t *id) { return pad_states(out, max, id, 2); }
static int disconnected_states(NpadState *out, int max, const uint32_t *id) {
    (void)id;
    if (max <= 0) return 0;
    if (!out) runtime_fail("controller output pointer");
    pthread_mutex_lock(&lock); *out = (NpadState){.sampling = ++sample}; pthread_mutex_unlock(&lock);
    return 1;
}
static void hold_type(int type) { if (type < 0 || type > 1) runtime_fail("invalid joy hold type"); }
static void disconnect(const uint32_t *id) {
    if (!id) runtime_fail("disconnect id");
    if (*id == pad_id()) { pthread_mutex_lock(&lock); allowed = false; pthread_mutex_unlock(&lock); }
}
/* The game sees no mouse and a keyboard without keys, as on macOS. */
static void initialize_mouse(void) { pthread_mutex_lock(&lock); mouse_ready = true; pthread_mutex_unlock(&lock); }
static void initialize_keyboard(void) { pthread_mutex_lock(&lock); keyboard_ready = true; pthread_mutex_unlock(&lock); }
static int mouse_states(MouseState *out, int max) {
    if (max <= 0) return 0;
    if (!out) runtime_fail("mouse output pointer");
    pthread_mutex_lock(&lock);
    if (!mouse_ready) runtime_fail("mouse used before InitializeMouse");
    *out = (MouseState){.sampling = ++sample};
    pthread_mutex_unlock(&lock);
    return 1;
}
static void keyboard_state(KeyboardState *out) {
    if (!out) runtime_fail("keyboard output pointer");
    pthread_mutex_lock(&lock);
    if (!keyboard_ready) runtime_fail("keyboard used before InitializeKeyboard");
    *out = (KeyboardState){.sampling = ++sample};
    pthread_mutex_unlock(&lock);
}
/* One controller: the controller-support applet returns at once with it. */
static uint32_t show_controller_support(ControllerSupportResult *out, const void *arg) {
    (void)arg;
    if (out) *out = (ControllerSupportResult){.players = 1, .selected = pad_id()};
    return 0;
}

/* Rumble and motion stay off unless TOTK_RUMBLE / TOTK_GYRO are set, and the
   Android side has no actuator or sensor bridge yet. */
void runtime_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high) {
    (void)side; (void)amp_low; (void)freq_low; (void)amp_high; (void)freq_high;
}
int runtime_motion_read(RuntimeMotionState *out, int max) { (void)out; (void)max; return 0; }
void runtime_motion_start(void) {}

void *runtime_input_override(const char *name) {
    void *feedback = runtime_hid_feedback_override(name);
    if (feedback) return feedback;
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn3hid14InitializeNpadEv", initialize},
        {"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", supported_ids},
        {"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", supported_styles},
        {"_ZN2nn3hid15GetNpadStyleSetERKj", style},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", full_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", disconnected_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", disconnected_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", disconnected_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", handheld_states},
        {"_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", hold_type},
        {"_ZN2nn3hid14DisconnectNpadERKj", disconnect},
        {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", show_controller_support},
        {"_ZN2nn3hid15InitializeMouseEv", initialize_mouse},
        {"_ZN2nn3hid18InitializeKeyboardEv", initialize_keyboard},
        {"_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", mouse_states},
        {"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", keyboard_state}};
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++)
        if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}
