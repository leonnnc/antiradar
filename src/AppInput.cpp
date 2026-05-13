#include "AppInput.h"

#include "CyberdeckPins.h"

namespace {

constexpr uint16_t DEBOUNCE_MS = 32;
constexpr uint16_t LONG_PRESS_MS = 650;
constexpr uint16_t REPEAT_START_MS = 430;
constexpr uint16_t REPEAT_MS = 145;
constexpr uint8_t ENCODER_STEPS_PER_DETENT = 4;

struct ButtonState {
    uint8_t pin = 0;
    bool raw = true;
    bool stable = true;
    bool previousStable = true;
    uint32_t changedAt = 0;
    uint32_t pressedAt = 0;
    uint32_t repeatAt = 0;
    bool longSent = false;
};

ButtonState btnUp;
ButtonState btnDown;
ButtonState btnOk;
ButtonState btnBack;
ButtonState btnEncSw;

uint8_t encoderPrev = 0;
int8_t encoderAccum = 0;
uint32_t encoderEdgeUs = 0;

void initButton(ButtonState& b, uint8_t pin) {
    b.pin = pin;
    pinMode(pin, INPUT_PULLUP);
    b.raw = digitalRead(pin);
    b.stable = b.raw;
    b.previousStable = b.stable;
    b.changedAt = millis();
    b.pressedAt = 0;
    b.repeatAt = 0;
    b.longSent = false;
}

void updateButton(ButtonState& b) {
    const bool reading = digitalRead(b.pin);
    const uint32_t now = millis();

    b.previousStable = b.stable;

    if (reading != b.raw) {
        b.raw = reading;
        b.changedAt = now;
    }

    if ((now - b.changedAt) >= DEBOUNCE_MS && b.stable != b.raw) {
        b.stable = b.raw;

        if (!b.stable) {
            b.pressedAt = now;
            b.repeatAt = now + REPEAT_START_MS;
            b.longSent = false;
        }
    }
}

bool pressedEdge(const ButtonState& b) {
    return b.previousStable && !b.stable;
}

bool releasedEdge(const ButtonState& b) {
    return !b.previousStable && b.stable;
}

bool active(const ButtonState& b) {
    return !b.stable;
}

bool repeatReady(ButtonState& b) {
    if (!active(b)) return false;
    const uint32_t now = millis();
    if (now < b.repeatAt) return false;
    b.repeatAt = now + REPEAT_MS;
    return true;
}

AppAction pollSelect(ButtonState& b) {
    const uint32_t now = millis();

    if (active(b) && !b.longSent && b.pressedAt > 0 && (now - b.pressedAt) >= LONG_PRESS_MS) {
        b.longSent = true;
        return AppAction::LongSelect;
    }

    if (releasedEdge(b) && !b.longSent) {
        return AppAction::Select;
    }

    return AppAction::None;
}

AppAction pollEncoder() {
    const uint32_t nowUs = micros();
    const uint8_t encoded = (digitalRead(CD_ENC_CLK) << 1) | digitalRead(CD_ENC_DT);
    if (encoded == encoderPrev) return AppAction::None;

    const uint8_t transition = (encoderPrev << 2) | encoded;
    encoderPrev = encoded;

    if ((nowUs - encoderEdgeUs) < 700) return AppAction::None;
    encoderEdgeUs = nowUs;

    static const int8_t table[16] = {
        0, -1,  1,  0,
        1,  0,  0, -1,
       -1,  0,  0,  1,
        0,  1, -1,  0
    };

    encoderAccum += table[transition];

    if (encoderAccum >= ENCODER_STEPS_PER_DETENT) {
        encoderAccum = 0;
        return AppAction::Down;
    }

    if (encoderAccum <= -ENCODER_STEPS_PER_DETENT) {
        encoderAccum = 0;
        return AppAction::Up;
    }

    return AppAction::None;
}

}  // namespace

void inputBegin() {
    initButton(btnUp, CD_BTN_UP);
    initButton(btnDown, CD_BTN_DOWN);
    initButton(btnOk, CD_BTN_OK);
    initButton(btnBack, CD_BTN_BACK);
    initButton(btnEncSw, CD_ENC_SW);

    pinMode(CD_ENC_CLK, INPUT_PULLUP);
    pinMode(CD_ENC_DT, INPUT_PULLUP);
    encoderPrev = (digitalRead(CD_ENC_CLK) << 1) | digitalRead(CD_ENC_DT);
    encoderAccum = 0;
    encoderEdgeUs = micros();
}

AppAction inputRead() {
    updateButton(btnUp);
    updateButton(btnDown);
    updateButton(btnOk);
    updateButton(btnBack);
    updateButton(btnEncSw);

    if (pressedEdge(btnBack)) return AppAction::Back;

    AppAction select = pollSelect(btnOk);
    if (select != AppAction::None) return select;

    select = pollSelect(btnEncSw);
    if (select != AppAction::None) return select;

    if (pressedEdge(btnUp) || repeatReady(btnUp)) return AppAction::Up;
    if (pressedEdge(btnDown) || repeatReady(btnDown)) return AppAction::Down;

    return pollEncoder();
}

bool inputAnyHeld() {
    return active(btnUp) || active(btnDown) || active(btnOk) || active(btnBack) || active(btnEncSw);
}

const char* actionName(AppAction action) {
    switch (action) {
        case AppAction::Up: return "UP";
        case AppAction::Down: return "DOWN";
        case AppAction::Select: return "OK";
        case AppAction::Back: return "BACK";
        case AppAction::LongSelect: return "OK HOLD";
        default: return "NONE";
    }
}
