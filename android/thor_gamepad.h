#pragma once
#include <android/input.h>

/* Feed NativeActivity events to the existing RuntimeGamepadState ABI. */
int android_gamepad_event(AInputEvent *event);
