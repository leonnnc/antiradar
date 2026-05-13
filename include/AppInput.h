#pragma once

#include <Arduino.h>

enum class AppAction : uint8_t {
    None,
    Up,
    Down,
    Select,
    Back,
    LongSelect
};

void inputBegin();
AppAction inputRead();
bool inputAnyHeld();
const char* actionName(AppAction action);
