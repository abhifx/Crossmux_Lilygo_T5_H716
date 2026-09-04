#include "InputManager.h"

#include <algorithm>

#include "MultiTouchGestureMath.h"

#if FREEINK_CAP_TOUCH
#include <Wire.h>
#include <driver/gpio.h>

#include <cstring>
#if FREEINK_DEVICE_EEGO_A4
#include "gsl/EegoA4GslFirmware.h"
#endif
#if FREEINK_DEVICE_MURPHY_M4
#include <MurphyM4I2c.h>
#endif
#endif
#if FREEINK_DEVICE_PAPERMONO
#include <PaperMonoBoard.h>
#endif
#if defined(TOUCH_PROBE_DEBUG)
#include <esp_rom_sys.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#endif

// Recorded ADC values from real devices
// BACK CONF LEFT RGHT   UP DOWN
// 3597 2760 1530    6 2300    6
// 3470 2666 1480    6 2222    5
// 3470 2655 1470    3 2205    3
//
// Averages
// BACK CONF LEFT RGHT   UP DOWN
// 3512 2694 1493    5 2242    5
//
// Setup ranges, if ADC value is between value `i` and `i + 1`, button `i` is
// being pressed. These ranges are based on real world values above, and are
// much more tolerant of different devices than a fixed threshold check. They
// are calculated by taking the midpoint of the pairs of averaged values above.
const int InputManager::ADC_RANGES_1[] = {ADC_NO_BUTTON, 3100, 2090, 750, INT32_MIN};
const int InputManager::ADC_RANGES_2[] = {ADC_NO_BUTTON, 1120, INT32_MIN};
const char* InputManager::BUTTON_NAMES[] = {"Back", "Confirm", "Left", "Right", "Up", "Down", "Power"};

namespace {
int absInt(const int value) { return value < 0 ? -value : value; }

#if FREEINK_DEVICE_EEGO_A4 || FREEINK_DEVICE_MURPHY_M4
bool movedBeyondSlop(const int dx, const int dy, const int slop) {
  return absInt(dx) > slop || absInt(dy) > slop;
}
#endif

#if defined(TOUCH_PROBE_DEBUG)
void touchDebugPrintf(const char* format, ...) {
  char buf[192];
  va_list args;
  va_start(args, format);
  const int len = vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  if (len < 0) return;
  const size_t n = strnlen(buf, sizeof(buf));
#if FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_USB_CDC_WRITE
  Serial.write(reinterpret_cast<const uint8_t*>(buf), n);
#elif FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_ROM_PRINTF
  esp_rom_printf("%s", buf);
#else
  if (Serial) {
    Serial.print(buf);
  }
#endif
}
#endif
}  // namespace

InputManager::InputManager()
    : currentState(0),
      lastState(0),
      pressedEvents(0),
      releasedEvents(0),
      lastDebounceTime(0),
      buttonPressStart(0),
      buttonPressFinish(0),
      powerButtonPressStart(0),
      powerButtonPressFinish(0),
      confirmBackPressStart(0),
      confirmBackPhysicalPressed(false),
      confirmBackLongPressActive(false),
      confirmPowerPressStart(0),
      confirmPowerPhysicalPressed(false),
      confirmPowerLongPressActive(false),
      twoButtonPhysicalState(0),
      twoButtonPressStart(0),
      twoButtonLongPressActive(false) {}

void InputManager::begin() {
  if (BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::XteinkAdcLadder) {
    pinMode(BUTTON_ADC_PIN_1, INPUT);
    pinMode(BUTTON_ADC_PIN_2, INPUT);
    pinMode(BoardConfig::ACTIVE.input.power, BoardConfig::ACTIVE.input.powerActiveHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
    analogSetAttenuation(ADC_11db);
    beginTouch();
    return;
  }

  const int8_t pins[] = {BoardConfig::ACTIVE.input.back, BoardConfig::ACTIVE.input.confirm,
                         BoardConfig::ACTIVE.input.left, BoardConfig::ACTIVE.input.right,
                         BoardConfig::ACTIVE.input.up,   BoardConfig::ACTIVE.input.down,
                         BoardConfig::ACTIVE.input.power};
  for (const int8_t pin : pins) {
    if (pin >= 0) {
      pinMode(pin, INPUT_PULLUP);
    }
  }
#if FREEINK_DEVICE_EEGO_A4
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::EegoA4 && BoardConfig::ACTIVE.input.power >= 0) {
    pinMode(BoardConfig::ACTIVE.input.power, BoardConfig::ACTIVE.input.powerActiveHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  }
#endif
  beginTouch();
}

int InputManager::getButtonFromADC(const int adcValue, const int ranges[], const int numButtons) {
  for (int i = 0; i < numButtons; i++) {
    if (ranges[i + 1] < adcValue && adcValue <= ranges[i]) {
      return i;
    }
  }

  return -1;
}

void InputManager::readButtonAdc(ButtonAdcSample& group1, ButtonAdcSample& group2) {
  group1 = {BUTTON_ADC_PIN_1, -1, -1};
  group2 = {BUTTON_ADC_PIN_2, -1, -1};
  if (BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::XteinkAdcLadder) {
    return;
  }

  group1.raw = analogRead(BUTTON_ADC_PIN_1);
  group1.button = getButtonFromADC(group1.raw, ADC_RANGES_1, NUM_BUTTONS_1);

  group2.raw = analogRead(BUTTON_ADC_PIN_2);
  const int b2 = getButtonFromADC(group2.raw, ADC_RANGES_2, NUM_BUTTONS_2);
  group2.button = b2 >= 0 ? b2 + 4 : -1;  // map group-2 local 0/1 to BTN_UP / BTN_DOWN
}

uint8_t InputManager::getState() {
  uint8_t state = 0;

  if (BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::XteinkAdcLadder) {
    state = getDigitalState();
    state |= serviceTouch();                    // run the touch machine; OR any synthesized button
    if (s_buttonHook) state |= s_buttonHook();  // board buttons (e.g. I2C expander)
    return state;
  }

  // Read GPIO1 buttons
  const int adcValue1 = analogRead(BUTTON_ADC_PIN_1);
  const int button1 = getButtonFromADC(adcValue1, ADC_RANGES_1, NUM_BUTTONS_1);
  if (button1 >= 0) {
    state |= (1 << button1);
  }

  // Read GPIO2 buttons
  const int adcValue2 = analogRead(BUTTON_ADC_PIN_2);
  const int button2 = getButtonFromADC(adcValue2, ADC_RANGES_2, NUM_BUTTONS_2);
  if (button2 >= 0) {
    state |= (1 << (button2 + 4));
  }

  // Read power button (polarity per board; X4 active-LOW, de-link active-HIGH)
  const int powerActiveLevel = BoardConfig::ACTIVE.input.powerActiveHigh ? HIGH : LOW;
  if (digitalRead(BoardConfig::ACTIVE.input.power) == powerActiveLevel) {
    state |= (1 << BTN_POWER);
  }

  state |= serviceTouch();
  if (s_buttonHook) state |= s_buttonHook();  // board buttons (e.g. I2C expander)
  return state;
}

InputManager::ButtonHook InputManager::s_buttonHook = nullptr;

void InputManager::beginAsync(const uint8_t taskPriority, const uint32_t pollMs, const uint8_t queueLen) {
  if (_asyncTask) return;  // already running
  _asyncPollMs = pollMs;
  _asyncQueue = xQueueCreate(queueLen, sizeof(uint8_t));
  if (!_asyncQueue) return;
  _asyncTapQueue = xQueueCreate(queueLen, sizeof(float) * 2);
  _asyncSwipeQueue = xQueueCreate(queueLen, sizeof(float) * 4);
  _asyncMultiTouchSwipeQueue = xQueueCreate(queueLen, sizeof(QueuedMultiTouchSwipe));
  _asyncMultiTouchRotationQueue = xQueueCreate(queueLen, sizeof(QueuedMultiTouchRotation));
  xTaskCreate(asyncTaskTrampoline, "fi_input", 4096, this, taskPriority, &_asyncTask);
}

void InputManager::asyncTaskTrampoline(void* self) { static_cast<InputManager*>(self)->asyncPoll(); }

void InputManager::asyncPoll() {
  static const uint8_t kButtons[] = {BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN, BTN_POWER};
  for (;;) {
    update();
    for (const uint8_t b : kButtons) {
      if (wasPressed(b)) xQueueSend(_asyncQueue, &b, 0);
    }
    float tap[2];
    if (_asyncTapQueue && wasTouchTap(tap[0], tap[1])) {
      xQueueSend(_asyncTapQueue, tap, 0);
    }
    float swipe[4];
    if (_asyncSwipeQueue && wasSwipe(swipe[0], swipe[1], swipe[2], swipe[3])) {
      xQueueSend(_asyncSwipeQueue, swipe, 0);
    }
    if (_asyncMultiTouchSwipeQueue && multiTouchSwipeEvent && !touchSuppressed) {
      const QueuedMultiTouchSwipe multiTouchSwipe = {multiTouchSwipeStartX,       multiTouchSwipeStartY,
                                                     multiTouchSwipeEndX,         multiTouchSwipeEndY,
                                                     multiTouchSwipeContactCount, multiTouchSwipeDurationMs};
      xQueueSend(_asyncMultiTouchSwipeQueue, &multiTouchSwipe, 0);
    }
    if (_asyncMultiTouchRotationQueue && multiTouchRotationEvent && !touchSuppressed) {
      const QueuedMultiTouchRotation rotation = {multiTouchRotationDegrees, multiTouchRotationCenterX,
                                                 multiTouchRotationCenterY, multiTouchRotationDurationMs};
      xQueueSend(_asyncMultiTouchRotationQueue, &rotation, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(_asyncPollMs));
  }
}

bool InputManager::popPress(uint8_t& button) {
  if (!_asyncQueue) return false;
  return xQueueReceive(_asyncQueue, &button, 0) == pdTRUE;
}

bool InputManager::popTouchTap(float& nx, float& ny) {
  if (!_asyncTapQueue) return false;
  float tap[2];
  if (xQueueReceive(_asyncTapQueue, tap, 0) != pdTRUE) return false;
  nx = tap[0];
  ny = tap[1];
  return true;
}

bool InputManager::popSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) {
  if (!_asyncSwipeQueue) return false;
  float swipe[4];
  if (xQueueReceive(_asyncSwipeQueue, swipe, 0) != pdTRUE) return false;
  nxStart = swipe[0];
  nyStart = swipe[1];
  nxEnd = swipe[2];
  nyEnd = swipe[3];
  return true;
}

bool InputManager::popMultiTouchSwipe(uint8_t& contactCount, float& nxStart, float& nyStart, float& nxEnd, float& nyEnd,
                                      unsigned long& durationMs) {
  if (!_asyncMultiTouchSwipeQueue) return false;
  QueuedMultiTouchSwipe swipe{};
  if (xQueueReceive(_asyncMultiTouchSwipeQueue, &swipe, 0) != pdTRUE) return false;
  contactCount = swipe.contactCount;
  normalizeTouchPoint(swipe.startX, swipe.startY, nxStart, nyStart);
  normalizeTouchPoint(swipe.endX, swipe.endY, nxEnd, nyEnd);
  durationMs = swipe.durationMs;
  return true;
}

bool InputManager::popMultiTouchRotation(float& degrees, float& nxCenter, float& nyCenter, unsigned long& durationMs) {
  if (!_asyncMultiTouchRotationQueue) return false;
  QueuedMultiTouchRotation rotation{};
  if (xQueueReceive(_asyncMultiTouchRotationQueue, &rotation, 0) != pdTRUE) return false;
  degrees = rotation.degrees;
  normalizeTouchPoint(rotation.centerX, rotation.centerY, nxCenter, nyCenter);
  durationMs = rotation.durationMs;
  return true;
}

bool InputManager::isDigitalPressed(const int8_t pin) const { return pin >= 0 && digitalRead(pin) == LOW; }

uint8_t InputManager::getDigitalState() const {
  uint8_t state = 0;

  if (BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::DigitalConfirmBackHold &&
      BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::DigitalConfirmPowerHold) {
    if (isDigitalPressed(BoardConfig::ACTIVE.input.back)) state |= (1 << BTN_BACK);
    if (isDigitalPressed(BoardConfig::ACTIVE.input.confirm)) state |= (1 << BTN_CONFIRM);
  }

  if (isDigitalPressed(BoardConfig::ACTIVE.input.left)) state |= (1 << BTN_LEFT);
  if (isDigitalPressed(BoardConfig::ACTIVE.input.right)) state |= (1 << BTN_RIGHT);
  if (isDigitalPressed(BoardConfig::ACTIVE.input.up)) state |= (1 << BTN_UP);
  if (isDigitalPressed(BoardConfig::ACTIVE.input.down)) state |= (1 << BTN_DOWN);
#if FREEINK_DEVICE_EEGO_A4
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::EegoA4) {
    // A4 power key: self-contained polarity-aware read. The generic
    // LOW-active fallback below must never see A4's idle pull-down level —
    // it would report the un-pressed key as held forever (root cause of the
    // 4508ms boot-time sleep on 118aa1e0). Non-A4 boards keep upstream logic.
    if (BoardConfig::ACTIVE.input.power >= 0 &&
        digitalRead(BoardConfig::ACTIVE.input.power) ==
            (BoardConfig::ACTIVE.input.powerActiveHigh ? HIGH : LOW) &&
        BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::DigitalConfirmBackHold &&
        BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::DigitalConfirmPowerHold) {
      state |= (1 << BTN_POWER);
    }
  } else
#endif
  if (isDigitalPressed(BoardConfig::ACTIVE.input.power) &&
      BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::DigitalConfirmBackHold &&
      BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::DigitalConfirmPowerHold) {
    state |= (1 << BTN_POWER);
  }

  return state;
}

void InputManager::applyStateChange(const uint8_t state, const unsigned long currentTime) {
  pressedEvents = state & ~currentState;
  releasedEvents = currentState & ~state;

  if (pressedEvents > 0 && currentState == 0) {
    buttonPressStart = currentTime;
  }

  if (releasedEvents > 0 && state == 0) {
    buttonPressFinish = currentTime;
  }

  if (pressedEvents & (1 << BTN_POWER)) {
    powerButtonPressStart = currentTime;
  }

  if (releasedEvents & (1 << BTN_POWER)) {
    powerButtonPressFinish = currentTime;
  }

  currentState = state;
  // Keep lastState in sync with the committed state so isDebouncePending() is
  // meaningful on every input style. A no-op for the debounced ADC path (state
  // already equals lastState at commit time), but the hold-style updates call
  // applyStateChange() directly without ever sampling through the debounce.
  lastState = state;
}

void InputManager::updateConfirmBackHold(const unsigned long currentTime) {
  const bool pressed = isDigitalPressed(BoardConfig::ACTIVE.input.confirm);
  const uint8_t nonSharedState = getDigitalState();
  bool emitConfirmClick = false;

  if (pressed && !confirmBackPhysicalPressed) {
    confirmBackPhysicalPressed = true;
    confirmBackLongPressActive = false;
    confirmBackPressStart = currentTime;
  }

  uint8_t nextState = nonSharedState;
  if (pressed && currentTime - confirmBackPressStart >= CONFIRM_BACK_HOLD_MS) {
    confirmBackLongPressActive = true;
    nextState |= (1 << BTN_BACK);
  }

  if (!pressed && confirmBackPhysicalPressed) {
    confirmBackPhysicalPressed = false;
    if (!confirmBackLongPressActive) {
      emitConfirmClick = true;
      buttonPressStart = confirmBackPressStart;
      buttonPressFinish = currentTime;
    }
    confirmBackLongPressActive = false;
  }

  applyStateChange(nextState, currentTime);

  if (emitConfirmClick) {
    pressedEvents |= (1 << BTN_CONFIRM);
    releasedEvents |= (1 << BTN_CONFIRM);
  }
}

void InputManager::updateConfirmPowerHold(const unsigned long currentTime) {
  const int8_t sharedPin =
      BoardConfig::ACTIVE.input.confirm >= 0 ? BoardConfig::ACTIVE.input.confirm : BoardConfig::ACTIVE.input.power;
  const bool pressed = isDigitalPressed(sharedPin);
  uint8_t nonSharedState = getDigitalState();
  nonSharedState |= serviceTouch();
  if (s_buttonHook) nonSharedState |= s_buttonHook();
  bool emitConfirmClick = false;

  if (pressed && !confirmPowerPhysicalPressed) {
    confirmPowerPhysicalPressed = true;
    confirmPowerLongPressActive = false;
    confirmPowerPressStart = currentTime;
  }

  uint8_t nextState = nonSharedState;
  if (pressed && s_sharedConfirmPowerShortPressEmitsPower) {
    nextState |= (1 << BTN_POWER);
  } else if (pressed && currentTime - confirmPowerPressStart >= CONFIRM_POWER_HOLD_MS) {
    confirmPowerLongPressActive = true;
    nextState |= (1 << BTN_POWER);
  }

  if (!pressed && confirmPowerPhysicalPressed) {
    confirmPowerPhysicalPressed = false;
    if (!confirmPowerLongPressActive) {
      if (!s_sharedConfirmPowerShortPressEmitsPower) {
        emitConfirmClick = true;
      }
      buttonPressStart = confirmPowerPressStart;
      buttonPressFinish = currentTime;
    }
    confirmPowerLongPressActive = false;
  }

  applyStateChange(nextState, currentTime);

  if (pressedEvents & (1 << BTN_POWER)) {
    powerButtonPressStart = confirmPowerPressStart;
  }

  if (emitConfirmClick) {
    pressedEvents |= (1 << BTN_CONFIRM);
    releasedEvents |= (1 << BTN_CONFIRM);
  }
}

void InputManager::updateDigitalTwoButton(const unsigned long currentTime) {
  const bool up = isDigitalPressed(BoardConfig::ACTIVE.input.up);
  const bool down = isDigitalPressed(BoardConfig::ACTIVE.input.down);
  const uint8_t physical = static_cast<uint8_t>((up ? 1u : 0u) | (down ? 2u : 0u));
  uint8_t auxiliaryState = serviceTouch();
  if (s_buttonHook) auxiliaryState |= s_buttonHook();
#if FREEINK_DEVICE_PAPERMONO
  // The power button reaches only the PM1 PMIC; clicks surface here as a
  // one-tick BTN_POWER pulse in the STATE, so applyStateChange() emits the
  // press this update and the release on the next. Never write the event
  // masks directly — applyStateChange() assigns them from the state diff,
  // clobbering direct writes the same tick.
  if (freeink::papermono::pollPowerButtonClicked(currentTime)) {
    auxiliaryState |= static_cast<uint8_t>(1u << BTN_POWER);
  }
#endif

  if (physical != twoButtonPhysicalState) {
    const uint8_t releasedPhysical = twoButtonPhysicalState;
    const bool emitShort = physical == 0 && !twoButtonLongPressActive;

    applyStateChange(auxiliaryState, currentTime);
    if (emitShort && (releasedPhysical == 1 || releasedPhysical == 2)) {
      const uint8_t logical = releasedPhysical == 1 ? BTN_UP : BTN_DOWN;
      pressedEvents |= static_cast<uint8_t>(1u << logical);
      releasedEvents |= static_cast<uint8_t>(1u << logical);
      buttonPressStart = twoButtonPressStart;
      buttonPressFinish = currentTime;
    }

    twoButtonPhysicalState = physical;
    twoButtonPressStart = currentTime;
    twoButtonLongPressActive = false;
    return;
  }

  if (physical == 0) {
    applyStateChange(auxiliaryState, currentTime);
    return;
  }

  uint8_t nextState = auxiliaryState;
  if (currentTime - twoButtonPressStart >= TWO_BUTTON_HOLD_MS) {
    twoButtonLongPressActive = true;
    const uint8_t logical = physical == 1 ? BTN_BACK : physical == 2 ? BTN_CONFIRM : BTN_POWER;
    nextState |= static_cast<uint8_t>(1u << logical);
  }
  applyStateChange(nextState, currentTime);
  if (pressedEvents & (1u << BTN_POWER)) powerButtonPressStart = twoButtonPressStart;
}

#if FREEINK_DEVICE_WAVESHARE_EPAPER_397
void InputManager::updateFunctionMultiGesture(const unsigned long currentTime) {
  using Gesture = freeink::input::FunctionButtonGesture;
  static_assert(Gesture::BACK == (1u << BTN_BACK) && Gesture::CONFIRM == (1u << BTN_CONFIRM) &&
                    Gesture::LEFT == (1u << BTN_LEFT) && Gesture::RIGHT == (1u << BTN_RIGHT) &&
                    Gesture::UP == (1u << BTN_UP) && Gesture::DOWN == (1u << BTN_DOWN),
                "Function gesture button bits must match InputManager");

  uint8_t raw = 0;
  if (isDigitalPressed(BoardConfig::ACTIVE.input.back)) raw |= Gesture::BACK;
  if (isDigitalPressed(BoardConfig::ACTIVE.input.left)) raw |= Gesture::LEFT;
  if (isDigitalPressed(BoardConfig::ACTIVE.input.right)) raw |= Gesture::RIGHT;
  if (isDigitalPressed(BoardConfig::ACTIVE.input.confirm)) raw |= Gesture::CONFIRM;

  const auto state = functionButtonGesture.update(raw, currentTime);
  const uint8_t auxiliaryState = s_buttonHook ? s_buttonHook() : 0;
  applyStateChange(state.down | auxiliaryState, currentTime);
  pressedEvents |= state.pressed;
  releasedEvents |= state.released;

  const uint8_t direction = Gesture::LEFT | Gesture::RIGHT | Gesture::UP | Gesture::DOWN;
  if (state.pressed & direction) buttonPressStart = state.startedMs;
  if (state.released & direction) buttonPressFinish = currentTime;
  if (state.pressed & Gesture::CONFIRM) buttonPressStart = state.startedMs;
  const uint8_t click = Gesture::BACK | Gesture::CONFIRM;
  if ((state.pressed & click) && (state.released & click)) {
    buttonPressStart = state.startedMs;
    buttonPressFinish = currentTime;
  }
}
#endif

void InputManager::update() {
  const unsigned long currentTime = millis();

  pressedEvents = 0;
  releasedEvents = 0;
  touchPressedEvent = false;  // one-shot touch coord events, cleared each update()
  touchReleasedEvent = false;
  touchLongPressEvent = false;
  multiTouchSwipeEvent = false;
  multiTouchRotationEvent = false;
  touchHomeKeyEvent = false;
  touchHomeKeyTapEvent = false;
  touchHomeKeyLongEvent = false;

  if (BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::DigitalConfirmBackHold) {
    updateConfirmBackHold(currentTime);
    return;
  }
  if (BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::DigitalConfirmPowerHold) {
    updateConfirmPowerHold(currentTime);
    return;
  }
  if (BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::DigitalTwoButton) {
    updateDigitalTwoButton(currentTime);
    return;
  }
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::WaveshareEpaper397 &&
      BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::DigitalFunctionMultiGesture) {
    updateFunctionMultiGesture(currentTime);
    return;
  }
#endif

  const uint8_t state = getState();

  // Debounce
  if (state != lastState) {
    lastDebounceTime = currentTime;
    lastState = state;
  }

  if ((currentTime - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (state != currentState) {
      applyStateChange(state, currentTime);
    }
  }
#if FREEINK_DEVICE_EEGO_A4
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::EegoA4 && touchHomeKeyTapEvent) {
    pressedEvents |= (1 << BTN_BACK);
    releasedEvents |= (1 << BTN_BACK);
  }
#endif
}

bool InputManager::isPressed(const uint8_t buttonIndex) const { return currentState & (1 << buttonIndex); }

bool InputManager::isPowerButtonPhysicallyPressed() const {
  const int8_t pin = BoardConfig::ACTIVE.input.power;
  if (pin < 0) return false;
  const int activeLevel = BoardConfig::ACTIVE.input.powerActiveHigh ? HIGH : LOW;
  return digitalRead(pin) == activeLevel;
}

bool InputManager::wasPressed(const uint8_t buttonIndex) const { return pressedEvents & (1 << buttonIndex); }

bool InputManager::wasAnyPressed() const { return pressedEvents > 0; }

bool InputManager::wasReleased(const uint8_t buttonIndex) const { return releasedEvents & (1 << buttonIndex); }

bool InputManager::wasAnyReleased() const { return releasedEvents > 0; }

unsigned long InputManager::getHeldTime() const {
  // Still hold a button
  if (currentState > 0) {
    return millis() - buttonPressStart;
  }

  return buttonPressFinish - buttonPressStart;
}

unsigned long InputManager::getPowerButtonHeldTime() const {
  if (isPressed(BTN_POWER)) {
    return millis() - powerButtonPressStart;
  }

  return powerButtonPressFinish - powerButtonPressStart;
}

const char* InputManager::getButtonName(const uint8_t buttonIndex) {
  if (buttonIndex <= BTN_POWER) {
    return BUTTON_NAMES[buttonIndex];
  }
  return "Unknown";
}

bool InputManager::s_sharedConfirmPowerShortPressEmitsPower = false;

bool InputManager::isPowerButtonPressed() const { return isPressed(BTN_POWER); }

// ============================================================================
// Capacitive touch
//
// The public touch API is always available. Compiled only when
// FREEINK_CAP_TOUCH is set; the backend dispatches on
// BoardConfig::ACTIVE.touch.controller:
//   * CHSC6x (Murphy M3) — IRQ-driven, hand-rolled 16-byte frame decode.
//   * GT911  (LilyGo)    — polled status/point registers over I2C.
//   * FT5x06 (Paper Mono FT6336) — active-low IRQ + 0x02 point frame.
// Coordinates are delivered raw-panel-oriented; the app owns rotation.
// ============================================================================

bool InputManager::hasTouch() const {
#if FREEINK_CAP_TOUCH
  return touchDataEnabled;
#else
  return false;  // touch code not compiled in (FREEINK_CAP_TOUCH=0)
#endif
}

InputManager::TouchPoint InputManager::getTouchPoint() const { return touchPoint; }

bool InputManager::supportsMultiTouch() const {
#if FREEINK_CAP_TOUCH
  return touchDataEnabled && BoardConfig::ACTIVE.touch.controller == BoardConfig::TouchController::Gt911;
#else
  return false;
#endif
}

InputManager::TouchSnapshot InputManager::getTouchSnapshot() const {
#if FREEINK_CAP_TOUCH
  return touchSnapshot;
#else
  return {};
#endif
}

bool InputManager::isTouchPressed() const { return touchPressed; }
bool InputManager::wasTouchPressed() const { return touchPressedEvent && !touchMultiContactSequence; }
bool InputManager::wasTouchReleased() const {
  // A multi-touch gesture must still provide the raw release edge so UI code
  // can drop pressed-state feedback, even though its tap/drag classifiers are
  // suppressed below.
  return touchReleasedEvent && (!touchSuppressed || touchMultiContactSequence);
}

void InputManager::normalizeTouchPoint(const uint16_t x, const uint16_t y, float& nx, float& ny) const {
  const auto& t = BoardConfig::ACTIVE.touch;
  const uint16_t w = (t.rawMaxX > t.rawMinX) ? static_cast<uint16_t>(t.rawMaxX - t.rawMinX) : 1;
  const uint16_t h = (t.rawMaxY > t.rawMinY) ? static_cast<uint16_t>(t.rawMaxY - t.rawMinY) : 1;
  const auto clamp01 = [](const float value) { return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value); };
  nx = clamp01(static_cast<float>(x) / w);
  ny = clamp01(static_cast<float>(y) / h);
}

bool InputManager::wasTouchTap(float& nx, float& ny) const {
#if FREEINK_CAP_TOUCH
  if (!touchReleasedEvent || touchSuppressed || touchMultiContactSequence) return false;
  // Hold/long-press detection uses the tighter 28 px stationary slop, but a
  // released tap remains valid until motion reaches the 60 px swipe threshold.
  // Using the stationary threshold here created a 29..59 px dead band where a
  // normal finger roll was neither a tap nor a swipe.
  if (touchMovedBeyondTapReleaseSlop) return false;
  // Tap position = the FIRST contact sample (touch-down), not the last: the
  // reported centroid drifts 10-20px as a finger rolls off during lift, which
  // made small targets (steppers) feel unreliable with release-point routing.
  // A tap routes to where the user touched, not where the finger let go.
  normalizeTouchPoint(touchDownPoint.x, touchDownPoint.y, nx, ny);
  return true;
#else
  (void)nx;
  (void)ny;
  return false;
#endif
}

bool InputManager::wasTouchPressedAt(float& nx, float& ny) const {
#if FREEINK_CAP_TOUCH
  // Press-edge analogue of wasTouchTap: true on the frame a touch begins,
  // writing the touch-down position normalized 0..1 in the panel's native
  // frame. Lets the app highlight what's under the finger on touch-down (before
  // release).
  if (!touchPressedEvent || touchMultiContactSequence) return false;
  normalizeTouchPoint(touchDownPoint.x, touchDownPoint.y, nx, ny);
  return true;
#else
  (void)nx;
  (void)ny;
  return false;
#endif
}

bool InputManager::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
#if FREEINK_CAP_TOUCH
  if (!touchPressed || touchMovedBeyondTapSlop || touchSuppressed || touchMultiContactSequence) return false;
  normalizeTouchPoint(touchDownPoint.x, touchDownPoint.y, nx, ny);
  heldMs = millis() - touchDownPoint.timestamp;
  return true;
#else
  (void)nx;
  (void)ny;
  (void)heldMs;
  return false;
#endif
}

bool InputManager::isTouchHeldAt(float& nx, float& ny) const {
#if FREEINK_CAP_TOUCH
  // Live drag tracking: the latest contact sample (touchUpPoint is refreshed on
  // every sample while pressed), with no tap-slop gate.
  if (!touchPressed || touchSuppressed || touchMultiContactSequence) return false;
  normalizeTouchPoint(touchUpPoint.x, touchUpPoint.y, nx, ny);
  return true;
#else
  (void)nx;
  (void)ny;
  return false;
#endif
}

unsigned long InputManager::lastTouchHeldMs() const {
#if FREEINK_CAP_TOUCH
  return lastTouchHeldDurationMs;
#else
  return 0;
#endif
}

bool InputManager::wasTouchActivity() const {
#if FREEINK_CAP_TOUCH
  return touchPressedEvent || touchReleasedEvent;
#else
  return false;
#endif
}

bool InputManager::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
#if FREEINK_CAP_TOUCH
  if (!touchReleasedEvent || touchSuppressed || touchMultiContactSequence) return false;
  // A flick: travelled past a distance threshold within a time window. Distance
  // is measured in native px; the dominant axis is left to the app (after
  // mapping to its logical frame).
  if (lastTouchHeldDurationMs > TOUCH_SWIPE_MAX_MS) return false;
  const int dx = static_cast<int>(touchUpPoint.x) - static_cast<int>(touchDownPoint.x);
  const int dy = static_cast<int>(touchUpPoint.y) - static_cast<int>(touchDownPoint.y);
  const int adx = absInt(dx);
  const int ady = absInt(dy);
  if (adx < TOUCH_SWIPE_MIN_PX && ady < TOUCH_SWIPE_MIN_PX) return false;
  normalizeTouchPoint(touchDownPoint.x, touchDownPoint.y, nxStart, nyStart);
  normalizeTouchPoint(touchUpPoint.x, touchUpPoint.y, nxEnd, nyEnd);
  return true;
#else
  (void)nxStart;
  (void)nyStart;
  (void)nxEnd;
  (void)nyEnd;
  return false;
#endif
}

bool InputManager::wasMultiTouchSwipe(uint8_t& contactCount, float& nxStart, float& nyStart, float& nxEnd, float& nyEnd,
                                      unsigned long& durationMs) const {
#if FREEINK_CAP_TOUCH
  if (!multiTouchSwipeEvent || touchSuppressed) return false;
  contactCount = multiTouchSwipeContactCount;
  normalizeTouchPoint(multiTouchSwipeStartX, multiTouchSwipeStartY, nxStart, nyStart);
  normalizeTouchPoint(multiTouchSwipeEndX, multiTouchSwipeEndY, nxEnd, nyEnd);
  durationMs = multiTouchSwipeDurationMs;
  return true;
#else
  (void)contactCount;
  (void)nxStart;
  (void)nyStart;
  (void)nxEnd;
  (void)nyEnd;
  (void)durationMs;
  return false;
#endif
}

bool InputManager::wasMultiTouchRotation(float& degrees, float& nxCenter, float& nyCenter,
                                         unsigned long& durationMs) const {
#if FREEINK_CAP_TOUCH
  if (!multiTouchRotationEvent || touchSuppressed) return false;
  degrees = multiTouchRotationDegrees;
  normalizeTouchPoint(multiTouchRotationCenterX, multiTouchRotationCenterY, nxCenter, nyCenter);
  durationMs = multiTouchRotationDurationMs;
  return true;
#else
  (void)degrees;
  (void)nxCenter;
  (void)nyCenter;
  (void)durationMs;
  return false;
#endif
}

bool InputManager::wasTouchLongPress(float& nx, float& ny) const {
#if FREEINK_CAP_TOUCH
  if (!touchLongPressEvent || touchMultiContactSequence) return false;
  // Long-press routes to the touch-down point, same rationale as wasTouchTap.
  normalizeTouchPoint(touchDownPoint.x, touchDownPoint.y, nx, ny);
  return true;
#else
  (void)nx;
  (void)ny;
  return false;
#endif
}

void InputManager::suppressTouchContact() {
#if FREEINK_CAP_TOUCH
  // Only meaningful mid-contact (or on its release-edge frame); the latch
  // self-clears in serviceTouch() once the contact is fully over.
  if (touchPressed || touchReleasedEvent) touchSuppressed = true;
  cancelMultiTouchGesture();
  if (_asyncMultiTouchSwipeQueue) xQueueReset(_asyncMultiTouchSwipeQueue);
  if (_asyncMultiTouchRotationQueue) xQueueReset(_asyncMultiTouchRotationQueue);
#endif
}

void InputManager::startMultiTouchGesture(const TouchSnapshot& snapshot, const unsigned long now) {
  if (snapshot.count < 2 || snapshot.count > MAX_TOUCH_CONTACTS || snapshot.reportedCount != snapshot.count) {
    blockMultiTouchGesture();
    return;
  }

  if (snapshot.idsStable) {
    for (uint8_t i = 0; i < snapshot.count; ++i) {
      for (uint8_t j = i + 1; j < snapshot.count; ++j) {
        if (snapshot.points[i].id == snapshot.points[j].id) {
          blockMultiTouchGesture();
          return;
        }
      }
    }
  }

  trackedTouchContactCount = snapshot.count;
  multiTouchRotationEligible = trackedTouchContactCount == 2;
  for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
    multiTouchContacts[i] = {snapshot.points[i].id, snapshot.points[i].point, snapshot.points[i].point};
    multiTouchContacts[i].start.timestamp = now;
  }
  for (uint8_t i = trackedTouchContactCount; i < MAX_TOUCH_CONTACTS; ++i) multiTouchContacts[i] = {};

  multiTouchGestureState = MultiTouchGestureState::Tracking;
  touchMultiContactSequence = true;
  // Any multi-contact sequence invalidates every single-contact classifier.
  touchMovedBeyondTapSlop = true;
  touchMovedBeyondTapReleaseSlop = true;
  touchLongPressEvent = false;
  touchLongPressFired = true;
}

void InputManager::blockMultiTouchGesture() {
  multiTouchGestureState = MultiTouchGestureState::Blocked;
  touchMultiContactSequence = true;
  touchMovedBeyondTapSlop = true;
  touchMovedBeyondTapReleaseSlop = true;
  touchLongPressEvent = false;
  touchLongPressFired = true;
}

void InputManager::resetMultiTouchGesture() {
  multiTouchGestureState = MultiTouchGestureState::Idle;
  trackedTouchContactCount = 0;
  multiTouchRotationEligible = false;
  touchMultiContactSequence = false;
  for (auto& contact : multiTouchContacts) contact = {};
}

void InputManager::cancelMultiTouchGesture() {
  multiTouchSwipeEvent = false;
  multiTouchRotationEvent = false;
  multiTouchGestureState =
      (touchPressed || touchReleasedEvent) ? MultiTouchGestureState::Blocked : MultiTouchGestureState::Idle;
}

bool InputManager::findContactAssignment(const TouchSnapshot& snapshot, const uint8_t trackedCount,
                                         uint8_t assignment[MAX_TOUCH_CONTACTS]) const {
  if (trackedCount == 0 || trackedCount > snapshot.count || snapshot.count > MAX_TOUCH_CONTACTS) return false;

  if (snapshot.idsStable) {
    uint8_t used = 0;
    for (uint8_t tracked = 0; tracked < trackedCount; ++tracked) {
      bool found = false;
      for (uint8_t current = 0; current < snapshot.count; ++current) {
        if ((used & (1u << current)) || snapshot.points[current].id != multiTouchContacts[tracked].id) continue;
        assignment[tracked] = current;
        used |= 1u << current;
        found = true;
        break;
      }
      if (!found) return false;
    }
    return true;
  }

  // Coordinate-only GT911 variants have no persistent contact identity. Find
  // the unique lowest-movement injective assignment. With at most four points
  // the exhaustive search is bounded at 4^4 candidates and allocates nothing.
  const auto squaredDistance = [](const TouchPoint& a, const TouchPoint& b) {
    const int64_t dx = static_cast<int64_t>(a.x) - static_cast<int64_t>(b.x);
    const int64_t dy = static_cast<int64_t>(a.y) - static_cast<int64_t>(b.y);
    return dx * dx + dy * dy;
  };

  uint16_t candidateCount = 1;
  for (uint8_t i = 0; i < trackedCount; ++i) candidateCount *= snapshot.count;
  int64_t bestDistance = INT64_MAX;
  int64_t secondDistance = INT64_MAX;
  uint8_t bestAssignment[MAX_TOUCH_CONTACTS] = {};
  for (uint16_t code = 0; code < candidateCount; ++code) {
    uint16_t remaining = code;
    uint8_t used = 0;
    uint8_t candidate[MAX_TOUCH_CONTACTS] = {};
    int64_t distance = 0;
    bool valid = true;
    for (uint8_t tracked = 0; tracked < trackedCount; ++tracked) {
      const uint8_t current = remaining % snapshot.count;
      remaining /= snapshot.count;
      if (used & (1u << current)) {
        valid = false;
        break;
      }
      used |= 1u << current;
      candidate[tracked] = current;
      distance += squaredDistance(multiTouchContacts[tracked].last, snapshot.points[current].point);
    }
    if (!valid) continue;
    if (distance < bestDistance) {
      secondDistance = bestDistance;
      bestDistance = distance;
      std::copy(candidate, candidate + trackedCount, bestAssignment);
    } else if (distance < secondDistance) {
      secondDistance = distance;
    }
  }

  if (bestDistance == INT64_MAX) return false;
  if (secondDistance != INT64_MAX && secondDistance - bestDistance <= TOUCH_CONTACT_ASSIGNMENT_AMBIGUITY_PX_SQ) {
    return false;
  }
  std::copy(bestAssignment, bestAssignment + trackedCount, assignment);
  return true;
}

bool InputManager::matchMultiTouchSnapshot(const TouchSnapshot& snapshot) {
  if (snapshot.count != trackedTouchContactCount) return false;
  uint8_t assignment[MAX_TOUCH_CONTACTS] = {};
  if (!findContactAssignment(snapshot, trackedTouchContactCount, assignment)) return false;
  for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
    multiTouchContacts[i].last = snapshot.points[assignment[i]].point;
  }
  return true;
}

bool InputManager::expandMultiTouchGesture(const TouchSnapshot& snapshot, const unsigned long now) {
  if (snapshot.count <= trackedTouchContactCount || snapshot.count > MAX_TOUCH_CONTACTS) return false;

  uint8_t assignment[MAX_TOUCH_CONTACTS] = {};
  if (!findContactAssignment(snapshot, trackedTouchContactCount, assignment)) return false;
  for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
    const TouchPoint& current = snapshot.points[assignment[i]].point;
    if (absInt(static_cast<int>(current.x) - multiTouchContacts[i].start.x) > TOUCH_TAP_SLOP_PX ||
        absInt(static_cast<int>(current.y) - multiTouchContacts[i].start.y) > TOUCH_TAP_SLOP_PX) {
      return false;
    }
  }

  // Fingers commonly land a few frames apart. While the existing contacts are
  // still stationary, adopt the larger cardinality and start the translation
  // from this coherent frame. A contact joining after motion begins is rejected.
  startMultiTouchGesture(snapshot, now);
  return multiTouchGestureState == MultiTouchGestureState::Tracking;
}

bool InputManager::isTrackedContact(const MultiTouchPoint& point) const {
  for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
    if (point.id == multiTouchContacts[i].id) return true;
  }
  return false;
}

bool InputManager::hasStableTranslationGeometry() const {
  for (uint8_t first = 0; first < trackedTouchContactCount; ++first) {
    for (uint8_t second = first + 1; second < trackedTouchContactCount; ++second) {
      const int startSeparationX =
          static_cast<int>(multiTouchContacts[second].start.x) - multiTouchContacts[first].start.x;
      const int startSeparationY =
          static_cast<int>(multiTouchContacts[second].start.y) - multiTouchContacts[first].start.y;
      const int endSeparationX = static_cast<int>(multiTouchContacts[second].last.x) - multiTouchContacts[first].last.x;
      const int endSeparationY = static_cast<int>(multiTouchContacts[second].last.y) - multiTouchContacts[first].last.y;
      if (absInt(endSeparationX - startSeparationX) > TOUCH_MULTI_CONTACT_SEPARATION_SLOP_PX ||
          absInt(endSeparationY - startSeparationY) > TOUCH_MULTI_CONTACT_SEPARATION_SLOP_PX) {
        return false;
      }
    }
  }
  return true;
}

bool InputManager::hasEligibleRotationScale() const {
  if (trackedTouchContactCount != 2) return false;
  const auto toGesturePoint = [](const TouchPoint& point) {
    return freeink::input_detail::GesturePoint{point.x, point.y};
  };
  return freeink::input_detail::hasRotationScale(
      toGesturePoint(multiTouchContacts[0].start), toGesturePoint(multiTouchContacts[1].start),
      toGesturePoint(multiTouchContacts[0].last), toGesturePoint(multiTouchContacts[1].last));
}

bool InputManager::isMultiTouchTranslation(const unsigned long now) const {
  if (trackedTouchContactCount < 2 || now - multiTouchContacts[0].start.timestamp > TOUCH_MULTI_SWIPE_MAX_MS ||
      !hasStableTranslationGeometry()) {
    return false;
  }

  int startCenterX = 0;
  int startCenterY = 0;
  int endCenterX = 0;
  int endCenterY = 0;
  for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
    startCenterX += multiTouchContacts[i].start.x;
    startCenterY += multiTouchContacts[i].start.y;
    endCenterX += multiTouchContacts[i].last.x;
    endCenterY += multiTouchContacts[i].last.y;
  }
  startCenterX /= trackedTouchContactCount;
  startCenterY /= trackedTouchContactCount;
  endCenterX /= trackedTouchContactCount;
  endCenterY /= trackedTouchContactCount;
  const int centerDx = endCenterX - startCenterX;
  const int centerDy = endCenterY - startCenterY;
  const int centerAbsX = absInt(centerDx);
  const int centerAbsY = absInt(centerDy);

  if (centerAbsX >= TOUCH_SWIPE_MIN_PX && centerAbsX * 2 >= centerAbsY * 3) {
    for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
      const int dx = static_cast<int>(multiTouchContacts[i].last.x) - multiTouchContacts[i].start.x;
      if (absInt(dx) < TOUCH_SWIPE_MIN_PX || (dx > 0) != (centerDx > 0)) return false;
    }
    return true;
  }
  if (centerAbsY >= TOUCH_SWIPE_MIN_PX && centerAbsY * 2 >= centerAbsX * 3) {
    for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
      const int dy = static_cast<int>(multiTouchContacts[i].last.y) - multiTouchContacts[i].start.y;
      if (absInt(dy) < TOUCH_SWIPE_MIN_PX || (dy > 0) != (centerDy > 0)) return false;
    }
    return true;
  }
  return false;
}

bool InputManager::classifyMultiTouchRotation(const unsigned long now) {
  if (!multiTouchRotationEligible || trackedTouchContactCount != 2 ||
      now - multiTouchContacts[0].start.timestamp > TOUCH_MULTI_SWIPE_MAX_MS) {
    return false;
  }

  const auto toGesturePoint = [](const TouchPoint& point) {
    return freeink::input_detail::GesturePoint{point.x, point.y};
  };
  freeink::input_detail::RotationResult result;
  if (!freeink::input_detail::classifyRotation(
          toGesturePoint(multiTouchContacts[0].start), toGesturePoint(multiTouchContacts[1].start),
          toGesturePoint(multiTouchContacts[0].last), toGesturePoint(multiTouchContacts[1].last), result)) {
    return false;
  }

  multiTouchRotationDegrees = result.degrees;
  multiTouchRotationCenterX = result.centerX;
  multiTouchRotationCenterY = result.centerY;
  multiTouchRotationDurationMs = static_cast<uint16_t>(now - multiTouchContacts[0].start.timestamp);
  multiTouchRotationEvent = true;
  return true;
}

void InputManager::finishMultiTouchGesture(const unsigned long now) {
  if (classifyMultiTouchRotation(now)) {
    multiTouchGestureState = MultiTouchGestureState::Blocked;
    return;
  }
  if (isMultiTouchTranslation(now)) {
    uint32_t startX = 0;
    uint32_t startY = 0;
    uint32_t endX = 0;
    uint32_t endY = 0;
    for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
      startX += multiTouchContacts[i].start.x;
      startY += multiTouchContacts[i].start.y;
      endX += multiTouchContacts[i].last.x;
      endY += multiTouchContacts[i].last.y;
    }
    multiTouchSwipeContactCount = trackedTouchContactCount;
    multiTouchSwipeStartX = static_cast<uint16_t>(startX / trackedTouchContactCount);
    multiTouchSwipeStartY = static_cast<uint16_t>(startY / trackedTouchContactCount);
    multiTouchSwipeEndX = static_cast<uint16_t>(endX / trackedTouchContactCount);
    multiTouchSwipeEndY = static_cast<uint16_t>(endY / trackedTouchContactCount);
    multiTouchSwipeDurationMs = static_cast<uint16_t>(now - multiTouchContacts[0].start.timestamp);
    multiTouchSwipeEvent = true;
  }
  multiTouchGestureState = MultiTouchGestureState::Blocked;
}

void InputManager::updateMultiTouchGesture(const TouchSnapshot& snapshot, const unsigned long now) {
  if (snapshot.reportedCount > MAX_TOUCH_CONTACTS || snapshot.count != snapshot.reportedCount) {
    blockMultiTouchGesture();
    return;
  }
  if (snapshot.count == 0) {
    if (multiTouchGestureState == MultiTouchGestureState::Tracking) finishMultiTouchGesture(now);
    return;
  }
  if (multiTouchGestureState == MultiTouchGestureState::Blocked) return;
  if (multiTouchGestureState == MultiTouchGestureState::Idle) {
    if (snapshot.count < 2) return;
    // A finger that has already dragged or long-pressed owns this contact;
    // joining more fingers must not retroactively convert it into a swipe.
    if (touchPressed && (touchMovedBeyondTapSlop || touchLongPressFired)) {
      blockMultiTouchGesture();
      return;
    }
    startMultiTouchGesture(snapshot, now);
    return;
  }
  if (snapshot.count < trackedTouchContactCount) {
    // Stable IDs let us reject a replacement contact arriving in the same
    // frame that one of the original contacts leaves.
    if (snapshot.idsStable) {
      for (uint8_t i = 0; i < snapshot.count; ++i) {
        if (!isTrackedContact(snapshot.points[i])) {
          blockMultiTouchGesture();
          return;
        }
        for (uint8_t j = i + 1; j < snapshot.count; ++j) {
          if (snapshot.points[i].id == snapshot.points[j].id) {
            blockMultiTouchGesture();
            return;
          }
        }
      }
    }
    finishMultiTouchGesture(now);  // first tracked contact left
    return;
  }
  if (snapshot.count > trackedTouchContactCount) {
    if (!expandMultiTouchGesture(snapshot, now)) blockMultiTouchGesture();
    return;
  }
  if (!matchMultiTouchSnapshot(snapshot)) {
    blockMultiTouchGesture();
    return;
  }
  if (multiTouchRotationEligible && !hasEligibleRotationScale()) multiTouchRotationEligible = false;
}

bool InputManager::wasHomeKeyPressed() const { return touchHomeKeyEvent; }

bool InputManager::wasHomeKeyTapped() const { return touchHomeKeyTapEvent; }

bool InputManager::wasHomeKeyLongPressed() const { return touchHomeKeyLongEvent; }

void InputManager::clearTouchTapEvent() {
  // Drop the one-shot tap/release edges without delivering them. Used on
  // activity transitions so the new activity does not re-read a tap the
  // previous activity already consumed within the same frame (these are
  // cleared in update(), but a pushActivity runs mid-frame).
  touchPressedEvent = false;
  touchReleasedEvent = false;
}

void InputManager::prepareForDeepSleep() {
#if FREEINK_CAP_TOUCH
  const auto& t = BoardConfig::ACTIVE.touch;
  switch (t.controller) {
    case BoardConfig::TouchController::Ft6336u:
#if FREEINK_DEVICE_MURPHY_M4
      pauseFt6336uPolling();
      if (t.powerEnable >= 0) {
        pinMode(t.powerEnable, OUTPUT);
        digitalWrite(t.powerEnable, t.powerEnableActiveHigh ? LOW : HIGH);
      }
      touchDataEnabled = false;
#endif
      return;
    case BoardConfig::TouchController::Gslx680:
#if FREEINK_DEVICE_EEGO_A4
      gslx680Write32(0xe0, 0x00000088);
      delay(5);
      Wire.end();
      if (t.sda >= 0) pinMode(t.sda, INPUT);
      if (t.scl >= 0) pinMode(t.scl, INPUT);
      if (t.reset >= 0) {
        const auto reset = static_cast<gpio_num_t>(t.reset);
        gpio_hold_dis(reset);
        pinMode(t.reset, OUTPUT);
        digitalWrite(t.reset, LOW);
        gpio_hold_en(reset);
      }
#endif
      return;
    case BoardConfig::TouchController::Gt911: {
      if (gt911Addr != 0) {
        // Send GT911 deep sleep command (0x8040 = 0x05)
        uint8_t sleepCmd[3] = {0x80, 0x40, 0x05};
        Wire.beginTransmission(gt911Addr);
        Wire.write(sleepCmd, 3);
        Wire.endTransmission();
        touchDataEnabled = false;
      }
      return;
    }
    case BoardConfig::TouchController::None:
    case BoardConfig::TouchController::Chsc6x:
    case BoardConfig::TouchController::Ft5x06:
      return;
  }
#endif
}

bool InputManager::reinitializeTouchAfterSharedReset() {
#if FREEINK_DEVICE_MURPHY_M4
  if (BoardConfig::ACTIVE.touch.controller == BoardConfig::TouchController::Ft6336u) {
    if (!beginFt6336u(false)) return false;
    if (!startFt6336uPolling()) {
      esp_rom_printf("[touch] M4 static polling task unavailable; using synchronous polling\r\n");
    }
    return true;
  }
#endif
  return true;
}

void InputManager::beginTouch() {
#if FREEINK_CAP_TOUCH
  const auto& t = BoardConfig::ACTIVE.touch;
#if FREEINK_DEVICE_EEGO_A4
  if (t.controller == BoardConfig::TouchController::Gslx680) {
      beginGslx680();
    return;
  }
#endif
#if FREEINK_DEVICE_MURPHY_M4
  if (t.controller == BoardConfig::TouchController::Ft6336u) {
    beginFt6336u(true);
    return;
  }
#endif
  if (t.controller == BoardConfig::TouchController::None) {
    return;
  }
  if (t.controller == BoardConfig::TouchController::Gt911) {
    beginGt911();
    return;
  }
  if (t.controller == BoardConfig::TouchController::Ft5x06) {
    beginFt5x06();
    return;
  }
  // CHSC6x: I2C bus only. The IRQ is left unconfigured — it's a brief pulse on
  // this controller, so detection polls I2C and gates on the frame's touch bit
  // instead (see decodeChsc6xFrame / updateTouchFromIrq).
  if (t.sda >= 0 && t.scl >= 0 && t.i2cAddress != 0) {
    Wire.begin(t.sda, t.scl, 100000);
    Wire.setTimeOut(4);
    touchDataEnabled = true;
  }
#endif
}

uint8_t InputManager::serviceTouch() {
#if FREEINK_CAP_TOUCH
  if (!touchDataEnabled) {
    return 0;
  }
  const unsigned long now = millis();
  const auto& t = BoardConfig::ACTIVE.touch;

  // Contact bookkeeping shared by all backends. Runs BEFORE the poll so the
  // suppression latch releases on the first fully-idle frame (contact over,
  // release edge consumed) and a new contact beginning in this same call is
  // delivered normally.
  if (!touchPressed && !touchReleasedEvent) {
    touchSuppressed = false;
    touchLongPressFired = false;
    resetMultiTouchGesture();
  }

#if FREEINK_DEVICE_EEGO_A4
  if (t.controller == BoardConfig::TouchController::Gslx680) {
    pollGslx680(now);
  } else
#endif
#if FREEINK_DEVICE_MURPHY_M4
  if (t.controller == BoardConfig::TouchController::Ft6336u) {
    pollFt6336u(now);
  } else
#endif
  {
    if (t.controller == BoardConfig::TouchController::Gt911) {
      pollGt911(now);
    } else if (t.controller == BoardConfig::TouchController::Ft5x06) {
      pollFt5x06(now);
    } else {
      updateTouchFromIrq(now, 0);  // detection polls I2C; the IRQ is unused now
      // Synthesized confirm tracks an actually-detected press, not the IRQ line.
      if (touchPressedEvent) touchIrqPulseUntil = now + TOUCH_IRQ_PULSE_MS;
    }
  }

  // Long-press classification, beside the tap/swipe machinery it shares state
  // with. Fires once per contact, while the finger is still down.
  if (touchPressed && !touchMultiContactSequence && !touchMovedBeyondTapSlop && !touchLongPressFired &&
      !touchSuppressed && now - touchDownPoint.timestamp >= TOUCH_LONG_PRESS_MS) {
    touchLongPressFired = true;
    touchLongPressEvent = true;
  }

  return (t.synthesizeConfirm && now < touchIrqPulseUntil) ? (1 << BTN_CONFIRM) : 0;
#else
  return 0;
#endif
}

#if FREEINK_CAP_TOUCH

void InputManager::updateTouchFromIrq(const unsigned long now, const int irqRaw) {
  // Poll the controller over I2C on a fixed cadence, independent of the IRQ.
  // The CHSC6x IRQ is a brief (~24ms) pulse at touch-down, not a level held for
  // the contact, so edge/level-gated reads missed quick taps. readChsc6xPoint
  // only returns true for a real touch (data[3] touch bit), so polling can't
  // latch the idle phantom frame. A valid read sets the press and refreshes the
  // release deadline; once reads stop coming, the touch releases after a short
  // hold-over.
  (void)irqRaw;
  if (now >= touchReadAt) {
    touchReadAt = now + TOUCH_SAMPLE_DELAY_MS;
    TouchPoint point = {false, 0, 0, 0};
    if (readChsc6xPoint(point)) {
      touchPoint = point;
      if (!touchPressed) {
        touchPressed = true;
        touchPressedEvent = true;
        touchDownPoint = point;  // first contact sample, used for tap routing
        touchUpPoint = point;
        touchMovedBeyondTapSlop = false;
        touchMovedBeyondTapReleaseSlop = false;
      } else {
        touchUpPoint = point;
        const int dx = static_cast<int>(touchUpPoint.x) - static_cast<int>(touchDownPoint.x);
        const int dy = static_cast<int>(touchUpPoint.y) - static_cast<int>(touchDownPoint.y);
        if (absInt(dx) > TOUCH_TAP_SLOP_PX || absInt(dy) > TOUCH_TAP_SLOP_PX) {
          touchMovedBeyondTapSlop = true;
        }
        if (absInt(dx) > TOUCH_TAP_RELEASE_SLOP_PX || absInt(dy) > TOUCH_TAP_RELEASE_SLOP_PX) {
          touchMovedBeyondTapReleaseSlop = true;
        }
      }
      touchReleaseAt = now + TOUCH_IRQ_PULSE_MS;
    }
  }

  if (touchPressed && now >= touchReleaseAt) {
    touchPressed = false;
    touchReleasedEvent = true;
    lastTouchHeldDurationMs = now - touchDownPoint.timestamp;
  }
}

bool InputManager::readChsc6xPoint(TouchPoint& point) {
  const uint8_t addr = BoardConfig::ACTIVE.touch.i2cAddress;
  Wire.beginTransmission(addr);
  Wire.write(TOUCH_READ_COMMAND);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  uint8_t data[TOUCH_FRAME_SIZE] = {};
  const uint8_t received = Wire.requestFrom(addr, TOUCH_FRAME_SIZE, static_cast<uint8_t>(true));
  if (received != TOUCH_FRAME_SIZE) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (uint8_t i = 0; i < TOUCH_FRAME_SIZE; ++i) {
    data[i] = Wire.read();
  }
  return decodeChsc6xFrame(data, TOUCH_FRAME_SIZE, point);
}

bool InputManager::decodeChsc6xFrame(const uint8_t* data, const size_t len, TouchPoint& point) const {
  if (len < 7) {
    return false;
  }
  // data[3] bit 7 is the touch-present flag: 0x80 while a finger is down, 0x00
  // when idle. The controller keeps returning a stale coordinate frame between
  // touches, so without this gate every read looks like a phantom touch (which
  // is why polling reported a fixed point and IRQ-gated reads were needed to
  // dodge it). Release transitions briefly show 0x40/0xff — both fail this test
  // or the coordinate sanity check below.
  if ((data[3] & 0x80) == 0) {
    return false;
  }
  const uint16_t rawX = data[4];                                          // X: one byte
  const uint16_t rawY = (static_cast<uint16_t>(data[5]) << 8) | data[6];  // Y: 16-bit big-endian
  if ((rawX == 0 && rawY == 0) || (rawX == 0xff && rawY == 0xffff)) {
    return false;
  }
  const auto& t = BoardConfig::ACTIVE.touch;
  point.valid = true;
  // Panel-native coordinates (the calibrated raw range, in the touch panel's
  // own orientation); the app maps to its display/logical frame. See the touch
  // note in the README.
  point.x = mapTouchAxis(rawX, t.rawMinX, t.rawMaxX, t.rawMaxX - t.rawMinX);
  point.y = mapTouchAxis(rawY, t.rawMinY, t.rawMaxY, t.rawMaxY - t.rawMinY);
  point.timestamp = millis();
  return true;
}

uint16_t InputManager::mapTouchAxis(uint16_t raw, const uint16_t rawMin, const uint16_t rawMax,
                                    const uint16_t outMax) const {
  if (raw <= rawMin) return 0;
  if (raw >= rawMax) return outMax;
  return static_cast<uint32_t>(raw - rawMin) * outMax / (rawMax - rawMin);
}

#if FREEINK_DEVICE_EEGO_A4 || FREEINK_DEVICE_MURPHY_M4
InputManager::TouchPoint InputManager::mapTouchPoint(const uint16_t rawX, const uint16_t rawY,
                                                     const unsigned long now) const {
  const auto& t = BoardConfig::ACTIVE.touch;
  const uint16_t sx = t.swapXY ? rawY : rawX;
  const uint16_t sy = t.swapXY ? rawX : rawY;
  const uint16_t width = t.rawMaxX > t.rawMinX ? t.rawMaxX - t.rawMinX : 1;
  const uint16_t height = t.rawMaxY > t.rawMinY ? t.rawMaxY - t.rawMinY : 1;
  uint16_t x = mapTouchAxis(sx, t.rawMinX, t.rawMaxX, width);
#if FREEINK_DEVICE_MURPHY_M4
  uint16_t y = freeink::mapMurphyM4TouchShortAxis(sy, murphyM4Batch, height);
#else
  uint16_t y = mapTouchAxis(sy, t.rawMinY, t.rawMaxY, height);
#endif
  if (t.flipX) x = static_cast<uint16_t>(width - x);
  if (t.flipY) y = static_cast<uint16_t>(height - y);
  return {true, x, y, now};
}

void InputManager::updateTouchContact(const TouchPoint& point) {
  touchPoint = point;
  if (!touchPressed) {
    touchPressedEvent = true;
    touchDownPoint = point;
    touchMovedBeyondTapSlop = false;
    touchMovedBeyondTapReleaseSlop = false;
  }
  touchUpPoint = point;
  const int dx = static_cast<int>(touchUpPoint.x) - static_cast<int>(touchDownPoint.x);
  const int dy = static_cast<int>(touchUpPoint.y) - static_cast<int>(touchDownPoint.y);
  if (movedBeyondSlop(dx, dy, TOUCH_TAP_SLOP_PX)) touchMovedBeyondTapSlop = true;
  if (movedBeyondSlop(dx, dy, TOUCH_TAP_RELEASE_SLOP_PX)) touchMovedBeyondTapReleaseSlop = true;
  touchPressed = true;
}

void InputManager::releaseTouch(const unsigned long now) {
  if (touchPressed) {
    touchReleasedEvent = true;
    lastTouchHeldDurationMs = now - touchDownPoint.timestamp;
    touchUpPoint = touchPoint;
  }
  touchPressed = false;
  touchPoint.valid = false;
}
#endif

// --- FT5x06 / FT6336 (M5Stack Paper Mono) ----------------------------------

bool InputManager::ft5x06WriteReg(const uint8_t reg, const uint8_t value) {
  const uint8_t addr = BoardConfig::ACTIVE.touch.i2cAddress;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool InputManager::ft5x06ReadReg(const uint8_t reg, uint8_t* buf, const uint8_t len) {
  const uint8_t addr = BoardConfig::ACTIVE.touch.i2cAddress;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  const uint8_t got = Wire.requestFrom(addr, len, static_cast<uint8_t>(true));
  if (got != len) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (uint8_t i = 0; i < len; ++i) buf[i] = Wire.read();
  return true;
}

void InputManager::beginFt5x06() {
  const auto& t = BoardConfig::ACTIVE.touch;
  if (t.sda < 0 || t.scl < 0 || t.i2cAddress == 0) return;

#if FREEINK_DEVICE_PAPERMONO
  // The FT6336's power rail and reset line live on the M5IOE1 expander, not
  // ESP GPIOs — raise/release them before the probe below.
  freeink::papermono::enableTouch();
#endif

  // The bus is shared with M5PM1/M5IOE1/RX8130, whose standing profile is
  // 100 kHz. FT6336 accepts that rate even though M5GFX uses 400 kHz for its
  // controller-specific transactions.
  Wire.begin(t.sda, t.scl, 100000);
  Wire.setTimeOut(10);
  if (t.irq >= 0) pinMode(t.irq, INPUT_PULLUP);

  // Match M5GFX Touch_FT5x06::_check_init(): enter working mode, read the
  // chip/firmware/vendor window, then select polling/level interrupt mode.
  // Retried over ~600 ms: the FT6336 needs up to ~300 ms after a hardware
  // reset before its I2C interface answers, and on boards where the rail/reset
  // bring-up happens right here (Paper Mono: enableTouch() above) a one-shot
  // probe races the controller's boot and leaves touch dead for the session.
  // Gate on the transactions succeeding, NOT on the ID contents: Paper Mono
  // units ACK and serve the whole 0xA3..0xA8 window as zeros, so a vendor-byte
  // check reads as "absent" on a perfectly working controller.
  uint8_t id[6] = {};
  bool wrMode = false, rdId = false, wrIrq = false;
  for (int attempt = 0; attempt < 12 && !rdId; ++attempt) {
    if (attempt) delay(50);
    wrMode = ft5x06WriteReg(0x00, 0x00);
    rdId = wrMode && ft5x06ReadReg(0xA3, id, sizeof(id));
    wrIrq = rdId && ft5x06WriteReg(0xA4, 0x00);
  }
  touchDataEnabled = wrMode && rdId && wrIrq;
#ifdef TOUCH_PROBE_DEBUG
#if FREEINK_DEVICE_PAPERMONO
  // Expander state alongside the probe result: OUT should show TP_EN (bit 12)
  // and TP_RST (bit 5) high, MODE should show the configured output mask
  // (0x39B4). All-zero probe ids + correct expander state = the FT6336 itself
  // isn't answering; wrong expander state = the rail/reset never asserted.
  uint16_t ioeMode = 0xFFFF, ioeOut = 0xFFFF;
  freeink::m5ioe1::readReg16(freeink::m5ioe1::REG_GPIO_MODE_L, &ioeMode);
  freeink::m5ioe1::readReg16(freeink::m5ioe1::REG_GPIO_OUT_L, &ioeOut);
  touchDebugPrintf("[touch] IOE1 addr=0x%02X mode=0x%04X out=0x%04X\n", freeink::m5ioe1::g_addr, ioeMode, ioeOut);
  // Full bus scan with the touch rail up: expected residents are 0x32 (RX8130
  // RTC), 0x4F/0x6F (IOE1), 0x68 (BMI270), 0x6E (PM1), 0x50 (NFC on Pro) —
  // whatever ELSE ACKs is the touch controller (FT6336 = 0x38; some unit
  // revisions may carry a CST820 = 0x15 instead).
  touchDebugPrintf("[touch] i2c scan:");
  for (uint8_t a = 0x08; a <= 0x77; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) touchDebugPrintf(" 0x%02X", a);
    delayMicroseconds(200);
  }
  touchDebugPrintf("\n");
#endif
  touchDebugPrintf(
      "[touch] FT5x06 probe: enabled=%d wrMode=%d rdId=%d wrIrq=%d "
      "cipher=0x%02X fw=0x%02X vendor=0x%02X irq=%d\n",
      touchDataEnabled, wrMode, rdId, wrIrq, id[0], id[3], id[5], t.irq);
#endif
}

void InputManager::pollFt5x06(const unsigned long now) {
  const auto& t = BoardConfig::ACTIVE.touch;
  if (now < touchReadAt) return;
  touchReadAt = now + TOUCH_SAMPLE_DELAY_MS;

  // The controller runs in interrupt-polling mode (G_MODE=0, set in begin),
  // where INT emits low PULSES at the report rate while a contact is held —
  // the line reads HIGH between pulses even with the finger down, so its
  // level must not be treated as a release (that splits one swipe into a
  // phantom tap plus a swipe). Idle fast-path gate only; while a contact is
  // live, the TD_STATUS zero-contact frame below is the release authority.
  const bool irqDown = t.irq < 0 || digitalRead(t.irq) == LOW;
  if (!irqDown && !touchPressed) {
    return;
  }

  // Register 0x02 is TD_STATUS followed by the first point's XH/XL/YH/YL.
  // One contact is enough for the app's tap/swipe/drag gesture model.
  uint8_t data[5] = {};
  if (!ft5x06ReadReg(0x02, data, sizeof(data))) {
    // Transient read failures happen on the shared PY32 bus; survive them.
    // But a controller that has stopped answering (rail glitch) must not
    // leave the contact latched — release once samples go stale.
    constexpr unsigned long STALE_RELEASE_MS = 100;
    if (touchPressed && now - touchPoint.timestamp > STALE_RELEASE_MS) {
      touchPressed = false;
      touchPoint.valid = false;
      touchReleasedEvent = true;
      lastTouchHeldDurationMs = now - touchDownPoint.timestamp;
    }
    return;
  }
  if ((data[0] & 0x0F) == 0) {
    // FT6336 may keep INT low until TD_STATUS has been drained. Treat the
    // controller's zero-contact frame as authoritative; waiting only for the
    // GPIO to rise leaves touchPressed latched and drops every later tap.
    if (touchPressed) {
      touchPressed = false;
      touchPoint.valid = false;
      touchReleasedEvent = true;
      lastTouchHeldDurationMs = now - touchDownPoint.timestamp;
#ifdef TOUCH_PROBE_DEBUG
      touchDebugPrintf("[touch] FT release via TD_STATUS=0 held=%lums\n", lastTouchHeldDurationMs);
#endif
    }
    return;
  }
  const uint16_t rawX = static_cast<uint16_t>((data[1] & 0x0F) << 8) | data[2];
  const uint16_t rawY = static_cast<uint16_t>((data[3] & 0x0F) << 8) | data[4];
  const uint16_t sx = t.swapXY ? rawY : rawX;
  const uint16_t sy = t.swapXY ? rawX : rawY;

  touchPoint.valid = true;
  touchPoint.x = mapTouchAxis(sx, t.rawMinX, t.rawMaxX, t.rawMaxX - t.rawMinX);
  touchPoint.y = mapTouchAxis(sy, t.rawMinY, t.rawMaxY, t.rawMaxY - t.rawMinY);
  if (t.flipX) {
    touchPoint.x = static_cast<uint16_t>((t.rawMaxX - t.rawMinX) - touchPoint.x);
  }
  if (t.flipY) {
    touchPoint.y = static_cast<uint16_t>((t.rawMaxY - t.rawMinY) - touchPoint.y);
  }
  touchPoint.timestamp = now;

  if (!touchPressed) {
    touchPressed = true;
    touchPressedEvent = true;
    touchDownPoint = touchPoint;
    touchUpPoint = touchPoint;
    touchMovedBeyondTapSlop = false;
    touchMovedBeyondTapReleaseSlop = false;
#ifdef TOUCH_PROBE_DEBUG
    touchDebugPrintf("[touch] FT press raw=(%u,%u) panel=(%u,%u)\n", rawX, rawY, touchPoint.x, touchPoint.y);
#endif
  } else {
    touchUpPoint = touchPoint;
    const int dx = static_cast<int>(touchUpPoint.x) - static_cast<int>(touchDownPoint.x);
    const int dy = static_cast<int>(touchUpPoint.y) - static_cast<int>(touchDownPoint.y);
    if (absInt(dx) > TOUCH_TAP_SLOP_PX || absInt(dy) > TOUCH_TAP_SLOP_PX) {
      touchMovedBeyondTapSlop = true;
    }
    if (absInt(dx) > TOUCH_TAP_RELEASE_SLOP_PX || absInt(dy) > TOUCH_TAP_RELEASE_SLOP_PX) {
      touchMovedBeyondTapReleaseSlop = true;
    }
  }
}

// --- FT6336U (Murphy M4) -----------------------------------------------------

#if FREEINK_DEVICE_MURPHY_M4
bool InputManager::beginFt6336u(const bool powerCycle) {
  const auto& t = BoardConfig::ACTIVE.touch;
  pauseFt6336uPolling();
  touchDataEnabled = false;
  if (t.sda < 0 || t.scl < 0 || t.i2cAddress == 0 || t.powerEnable < 0) return false;

  portENTER_CRITICAL(&ft6336uPollMux);
  ft6336uPollState = {};
  portEXIT_CRITICAL(&ft6336uPollMux);

  if (powerCycle) {
    gpio_hold_dis(static_cast<gpio_num_t>(t.powerEnable));
    pinMode(t.powerEnable, OUTPUT);
    digitalWrite(t.powerEnable, t.powerEnableActiveHigh ? HIGH : LOW);
    delay(500);
    if (t.reset >= 0) {
      pinMode(t.reset, OUTPUT);
      digitalWrite(t.reset, LOW);
      delay(50);
      digitalWrite(t.reset, HIGH);
      delay(100);
    }
  }
  if (t.irq >= 0) pinMode(t.irq, INPUT_PULLUP);

  const auto device = freeink::murphy_m4_i2c::touchDevice(t.sda, t.scl, t.i2cAddress);
  const uint8_t mode[] = {0x00, 0x00};
  const uint8_t threshold[] = {0x80, 0x16};
  const uint8_t rate[] = {0x88, 0x04};
  uint8_t readMode = 0xFF;
  uint8_t readThreshold = 0xFF;
  uint8_t readRate = 0xFF;
  uint8_t probe[11] = {};
  const bool writesOk = freeink::murphy_m4_i2c::write(device, mode, sizeof(mode)) &&
                        freeink::murphy_m4_i2c::write(device, threshold, sizeof(threshold)) &&
                        freeink::murphy_m4_i2c::write(device, rate, sizeof(rate));
  const bool readsOk = writesOk && freeink::murphy_m4_i2c::read(device, 0x00, &readMode, 1) &&
                       freeink::murphy_m4_i2c::read(device, 0x80, &readThreshold, 1) &&
                       freeink::murphy_m4_i2c::read(device, 0x88, &readRate, 1) &&
                       freeink::murphy_m4_i2c::read(device, 0x02, probe, sizeof(probe));
  auto probeState = freeink::murphy_m4_touch::classifyFrame(probe[0], probe[1], probe[2], probe[3], probe[4]);
  if (probeState == freeink::murphy_m4_touch::FrameState::Contact &&
      !freeink::murphy_m4_touch::pointInBounds(probe[1], probe[2], probe[3], probe[4], t.rawMaxX, t.rawMaxY,
                                               t.swapXY)) {
    probeState = freeink::murphy_m4_touch::FrameState::Invalid;
  }
  touchDataEnabled = readsOk && readMode == 0x00 && readThreshold == 0x16 && readRate == 0x04 &&
                     probeState != freeink::murphy_m4_touch::FrameState::Invalid;
  if (!touchDataEnabled) {
    esp_rom_printf(
        "[touch] M4 FT6336U initialization failed: writes=%d reads=%d "
        "mode=%02X threshold=%02X rate=%02X frame=%d\r\n",
        static_cast<int>(writesOk), static_cast<int>(readsOk), readMode, readThreshold, readRate,
        static_cast<int>(probeState));
    digitalWrite(t.powerEnable, t.powerEnableActiveHigh ? LOW : HIGH);
  }
  return touchDataEnabled;
}

bool InputManager::readFt6336uFrame(freeink::murphy_m4_touch::FrameState& state, uint16_t& rawX, uint16_t& rawY) {
  const auto& t = BoardConfig::ACTIVE.touch;
  uint8_t frame[11] = {};
  const auto device = freeink::murphy_m4_i2c::touchDevice(t.sda, t.scl, t.i2cAddress);
  if (!freeink::murphy_m4_i2c::read(device, 0x02, frame, sizeof(frame))) return false;

  state = freeink::murphy_m4_touch::classifyFrame(frame[0], frame[1], frame[2], frame[3], frame[4]);
  if (state == freeink::murphy_m4_touch::FrameState::Contact &&
      !freeink::murphy_m4_touch::pointInBounds(frame[1], frame[2], frame[3], frame[4], t.rawMaxX, t.rawMaxY,
                                               t.swapXY)) {
    state = freeink::murphy_m4_touch::FrameState::Invalid;
  }
  if (state == freeink::murphy_m4_touch::FrameState::Contact) {
    rawX = freeink::murphy_m4_touch::axis(frame[1], frame[2]);
    rawY = freeink::murphy_m4_touch::axis(frame[3], frame[4]);
  }
  return true;
}

void InputManager::applyFt6336uSnapshot(const freeink::murphy_m4_touch::Snapshot& snapshot) {
  if (snapshot.completedPending) {
    if (!touchPressed) {
      updateTouchContact(
          mapTouchPoint(snapshot.completedDown.x, snapshot.completedDown.y, snapshot.completedDown.timestamp));
    }
    updateTouchContact(mapTouchPoint(snapshot.completedUp.x, snapshot.completedUp.y, snapshot.completedUp.timestamp));
    releaseTouch(snapshot.completedUp.timestamp);
    return;
  }

  if (!snapshot.contact) return;
  if (!touchPressed) updateTouchContact(mapTouchPoint(snapshot.down.x, snapshot.down.y, snapshot.down.timestamp));
  updateTouchContact(mapTouchPoint(snapshot.latest.x, snapshot.latest.y, snapshot.latest.timestamp));
}

bool InputManager::startFt6336uPolling() {
  if (!touchDataEnabled) return false;

  if (ft6336uTask == nullptr) {
    ft6336uTask = xTaskCreateStaticPinnedToCore(ft6336uTaskTrampoline, "fi_ft6336", FT6336U_TASK_STACK_BYTES, this, 5,
                                                ft6336uTaskStack, &ft6336uTaskTcb, 0);
    if (ft6336uTask == nullptr) return false;
    esp_rom_printf("[touch] M4 FT6336U polling on core 0 every %lu ms (static stack=%lu)\r\n",
                   static_cast<unsigned long>(FT6336U_POLL_MS), static_cast<unsigned long>(FT6336U_TASK_STACK_BYTES));
  }

  portENTER_CRITICAL(&ft6336uPollMux);
  ft6336uPollingEnabled = true;
  portEXIT_CRITICAL(&ft6336uPollMux);
  return true;
}

void InputManager::pauseFt6336uPolling() {
  if (ft6336uTask == nullptr) return;

  portENTER_CRITICAL(&ft6336uPollMux);
  ft6336uPollingEnabled = false;
  bool inFlight = ft6336uPollInFlight;
  portEXIT_CRITICAL(&ft6336uPollMux);

  const unsigned long startedAt = millis();
  while (inFlight && millis() - startedAt < 30) {
    delay(1);
    portENTER_CRITICAL(&ft6336uPollMux);
    inFlight = ft6336uPollInFlight;
    portEXIT_CRITICAL(&ft6336uPollMux);
  }
  if (inFlight) esp_rom_printf("[touch] M4 polling pause timed out with I2C in flight\r\n");
}

void InputManager::ft6336uTaskTrampoline(void* self) { static_cast<InputManager*>(self)->ft6336uTaskLoop(); }

void InputManager::ft6336uTaskLoop() {
  TickType_t wakeAt = xTaskGetTickCount();
  for (;;) {
    portENTER_CRITICAL(&ft6336uPollMux);
    const bool shouldPoll = ft6336uPollingEnabled;
    if (shouldPoll) ft6336uPollInFlight = true;
    portEXIT_CRITICAL(&ft6336uPollMux);

    if (shouldPoll) {
      freeink::murphy_m4_touch::FrameState state = freeink::murphy_m4_touch::FrameState::Invalid;
      uint16_t rawX = 0;
      uint16_t rawY = 0;
      const bool readOk = readFt6336uFrame(state, rawX, rawY);
      const uint32_t now = millis();

      portENTER_CRITICAL(&ft6336uPollMux);
      if (ft6336uPollingEnabled) {
        if (!readOk || state == freeink::murphy_m4_touch::FrameState::Invalid) {
          freeink::murphy_m4_touch::releaseIfStale(ft6336uPollState, now, FT6336U_STALE_RELEASE_MS);
        } else {
          switch (state) {
            case freeink::murphy_m4_touch::FrameState::Contact:
              freeink::murphy_m4_touch::recordContact(ft6336uPollState, rawX, rawY, now);
              break;
            case freeink::murphy_m4_touch::FrameState::Released:
              freeink::murphy_m4_touch::recordRelease(ft6336uPollState, now);
              break;
            case freeink::murphy_m4_touch::FrameState::Invalid:
              break;
          }
        }
      }
      ft6336uPollInFlight = false;
      portEXIT_CRITICAL(&ft6336uPollMux);
    }

    vTaskDelayUntil(&wakeAt, pdMS_TO_TICKS(FT6336U_POLL_MS));
  }
}

void InputManager::pollFt6336u(const unsigned long now) {
  if (ft6336uTask != nullptr) {
    portENTER_CRITICAL(&ft6336uPollMux);
    const auto snapshot = freeink::murphy_m4_touch::takeSnapshot(ft6336uPollState);
    portEXIT_CRITICAL(&ft6336uPollMux);
    applyFt6336uSnapshot(snapshot);
    return;
  }

  if (now < touchReadAt) return;
  touchReadAt = now + TOUCH_SAMPLE_DELAY_MS;

  freeink::murphy_m4_touch::FrameState state = freeink::murphy_m4_touch::FrameState::Invalid;
  uint16_t rawX = 0;
  uint16_t rawY = 0;
  if (!readFt6336uFrame(state, rawX, rawY) || state == freeink::murphy_m4_touch::FrameState::Invalid) {
    if (touchPressed && now - touchPoint.timestamp >= FT6336U_STALE_RELEASE_MS) releaseTouch(now);
    return;
  }

  switch (state) {
    case freeink::murphy_m4_touch::FrameState::Released:
      releaseTouch(now);
      return;
    case freeink::murphy_m4_touch::FrameState::Contact:
      updateTouchContact(mapTouchPoint(rawX, rawY, now));
      return;
    case freeink::murphy_m4_touch::FrameState::Invalid:
      return;
  }
}
#endif

// --- GSLX680 (EEGO A4) ------------------------------------------------------

#if FREEINK_DEVICE_EEGO_A4
bool InputManager::gslx680Write(const uint8_t reg, const uint8_t* data, const uint8_t len) {
  Wire.beginTransmission(BoardConfig::ACTIVE.touch.i2cAddress);
  Wire.write(reg);
  if (len && data) Wire.write(data, len);
  return Wire.endTransmission() == 0;
}

bool InputManager::gslx680Write32(const uint8_t reg, const uint32_t value) {
  const uint8_t data[] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                          static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  return gslx680Write(reg, data, sizeof(data));
}

bool InputManager::gslx680Read(const uint8_t reg, uint8_t* data, const uint8_t len) {
  Wire.beginTransmission(BoardConfig::ACTIVE.touch.i2cAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(BoardConfig::ACTIVE.touch.i2cAddress, len, static_cast<uint8_t>(true)) != len) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (uint8_t i = 0; i < len; ++i) data[i] = Wire.read();
  return true;
}

void InputManager::gslx680ClearRegisters() {
  gslx680Write32(0xe0, 0x00000088);
  delay(20);
  gslx680Write32(0x80, 0x00000003);
  delay(5);
  gslx680Write32(0xe4, 0x00000004);
  delay(5);
  gslx680Write32(0xe0, 0);
  delay(20);
}

void InputManager::gslx680ResetChip() {
  gslx680Write32(0xe0, 0x00000088);
  delay(20);
  gslx680Write32(0xe4, 0x00000004);
  delay(10);
  gslx680Write32(0xbc, 0);
  delay(10);
}

void InputManager::gslx680StartChip() {
  gslx680Write32(0xe0, 0);
  delay(10);
}

bool InputManager::gslx680LoadFirmware() {
  size_t i = 0;
  while (i < freeink::EEGO_A4_GSL_FIRMWARE_COUNT) {
    freeink::Gslx680FirmwareEntry entry;
    memcpy_P(&entry, &freeink::EEGO_A4_GSL_FIRMWARE[i], sizeof(entry));
    if (entry.offset == 0xf0) {
      if (!gslx680Write32(entry.offset, entry.value)) return false;
      ++i;
      continue;
    }
    uint8_t payload[64];
    uint8_t words = 0;
    const uint8_t firstReg = entry.offset;
    while (i < freeink::EEGO_A4_GSL_FIRMWARE_COUNT && words < 16) {
      memcpy_P(&entry, &freeink::EEGO_A4_GSL_FIRMWARE[i], sizeof(entry));
      if (entry.offset == 0xf0 || entry.offset != static_cast<uint8_t>(firstReg + words * 4)) {
        break;
      }
      memcpy(payload + words * 4, &entry.value, sizeof(entry.value));
      ++words;
      ++i;
    }
    if (!words || !gslx680Write(firstReg, payload, words * 4)) return false;
  }
  return true;
}

bool InputManager::gslx680Check() {
  uint8_t status[4] = {};
  return gslx680Read(0xb0, status, sizeof(status)) && status[0] == 0x5a && status[1] == 0x5a && status[2] == 0x5a &&
         status[3] == 0x5a;
}

void InputManager::beginGslx680() {
  const auto& t = BoardConfig::ACTIVE.touch;
  if (t.reset >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(t.reset));
    pinMode(t.reset, OUTPUT);
    digitalWrite(t.reset, HIGH);
    delay(20);
  }
  Wire.begin(t.sda, t.scl, 400000);
  Wire.setTimeOut(256);
  uint8_t probe = 0;
  if (!gslx680Read(0xf0, &probe, 1) || !gslx680Write32(0xf0, 0x00000012) || !gslx680Read(0xf0, &probe, 1)) {
    touchDataEnabled = false;
    return;
  }
  gslx680ClearRegisters();
  gslx680ResetChip();
  gslx680ResetChip();
  gslx680ClearRegisters();
  gslx680ResetChip();
  if (!gslx680LoadFirmware()) {
    touchDataEnabled = false;
    return;
  }
  gslx680StartChip();
  gslx680ResetChip();
  gslx680StartChip();
  if (!gslx680Check()) {
    gslx680ResetChip();
    gslx680StartChip();
  }
  touchDataEnabled = gslx680Check();
}

void InputManager::pollGslx680(const unsigned long now) {
  if (now < touchReadAt) return;
  touchReadAt = now + TOUCH_SAMPLE_DELAY_MS;
  uint8_t frame[24] = {};
  if (!gslx680Read(0x80, frame, sizeof(frame))) return;

  const auto finishHomeKey = [this, now]() {
    if (!touchHomeKeyDown) return;
    lastTouchHeldDurationMs = now - touchHomeKeyDownAt;
    if (!touchHomeKeyLongFired) touchHomeKeyTapEvent = true;
    touchHomeKeyDown = false;
    touchHomeKeyLongFired = false;
  };
  const uint8_t count = frame[0] > 5 ? 5 : frame[0];
  if (!count) {
    finishHomeKey();
    releaseTouch(now);
    return;
  }

  const uint16_t rawYWord = static_cast<uint16_t>(frame[5]) << 8 | frame[4];
  const uint16_t rawXWord = static_cast<uint16_t>(frame[7]) << 8 | frame[6];
  const bool homeKeyDown = count == 1 && rawXWord == 0x03a0 && rawYWord == 0x1020;
  if (homeKeyDown) {
    if (!touchHomeKeyDown) {
      touchHomeKeyEvent = true;
      touchHomeKeyDown = true;
      touchHomeKeyLongFired = false;
      touchHomeKeyDownAt = now;
    } else if (!touchHomeKeyLongFired && now - touchHomeKeyDownAt >= HOME_KEY_LONG_PRESS_MS) {
      touchHomeKeyLongEvent = true;
      touchHomeKeyLongFired = true;
    }
    touchPressed = false;
    touchPoint.valid = false;
    return;
  }
  finishHomeKey();

  const uint16_t rawY = rawYWord & 0x0fff;
  const uint16_t rawX = rawXWord & 0x0fff;

  // EEGO A4 (GSLX680) coordinate calibration. The linear raw-range mapping is
  // wrong for this panel: the GSL reports a portrait digitizer whose raw axes
  // run 0..~920 x 0..~680, while the UC8279C framebuffer is landscape 768x552.
  // Recovered from the official 1.2.7 GSL firmware's FUN_4204c510 (CrossLink
  // 1.0.10's narrower transform only covered ~536 of the 552 short-axis rows,
  // making edge/front-key hits inaccurate):
  //   portraitX = min(rawY, 680) * 551 / 680
  //   portraitY = (920 - min(rawX, 920)) * 767 / 920
  // The digitizer is portrait while the framebuffer is landscape, so swap the
  // calibrated axes when returning panel-native coordinates.
  const uint16_t limitedY = rawY > 680 ? 680 : rawY;
  const uint16_t limitedX = rawX > 920 ? 920 : rawX;
  const uint16_t portraitX = static_cast<uint32_t>(limitedY) * 551 / 680;
  const uint16_t portraitY = static_cast<uint32_t>(920 - limitedX) * 767 / 920;

  // GfxRenderer's Portrait transform rotates panel-native coordinates clockwise
  // and therefore computes logical X as (551 - nativeY). The digitizer's raw-Y
  // axis grows in logical left-to-right order, so nativeY must be mirrored here.
  // Without this final reflection every horizontal touch target is reversed.
  const TouchPoint point = {true, portraitY, static_cast<uint16_t>(551 - portraitX), now};
  updateTouchContact(point);
}
#endif

// --- GT911 (LilyGo) ---------------------------------------------------------

void InputManager::beginGt911() {
  const auto& t = BoardConfig::ACTIVE.touch;

  // Power the touch rail first (boards that gate it, e.g. Sticky's TOUCH_EN on
  // GPIO42). Active-high + settle, before the reset dance and I2C probe;
  // without this the GT911 never ACKs and touch is reported absent. No-op when
  // unassigned. gpio_hold_dis first: the sleep path holds this pin LOW and the
  // hold survives the deep-sleep wake reset; the HIGH write is a no-op until it
  // is released.
  if (t.powerEnable >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(t.powerEnable));
    pinMode(t.powerEnable, OUTPUT);
    // ON level: HIGH for active-high enables (Sticky), LOW for active-low (X4
    // Pro GPIO2).
    digitalWrite(t.powerEnable, t.powerEnableActiveHigh ? HIGH : LOW);
    delay(50);
  }

  if (t.sda >= 0 && t.scl >= 0) {
    Wire.begin(t.sda, t.scl, 400000);
    Wire.setTimeOut(10);
  }

  auto resetWithIntLevel = [&](const uint8_t level) {
    if (t.reset < 0 || t.irq < 0) return;
    pinMode(t.irq, OUTPUT);
    pinMode(t.reset, OUTPUT);
    digitalWrite(t.reset, LOW);
    digitalWrite(t.irq, level);
    delay(10);
    digitalWrite(t.reset, HIGH);
    delay(10);
    digitalWrite(t.irq, level);
    delay(50);
    pinMode(t.irq, INPUT);
    delay(50);
  };

  auto probeCandidates = [&]() {
    const uint8_t candidates[2] = {t.i2cAddress, t.i2cAddressAlt};
    for (uint8_t a : candidates) {
      if (a == 0) continue;
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        gt911Addr = a;
        return true;
      }
    }
    return false;
  };

  // Reset + address-select dance: INT level as RST rises selects the address.
  // Boards differ in which strapped address survives their module wiring, so
  // try the primary-select level first, then the alternate level before
  // declaring the touch controller absent.
  gt911Addr = 0;
  resetWithIntLevel(LOW);
  if (!probeCandidates()) {
    resetWithIntLevel(HIGH);
    probeCandidates();
  }

  touchDataEnabled = (gt911Addr != 0);
#ifdef TOUCH_PROBE_DEBUG
  touchDebugPrintf(
      "[touch] GT911 probe: addr=0x%02X enabled=%d (sda=%d scl=%d "
      "cand=0x%02X/0x%02X)\n",
      gt911Addr, touchDataEnabled, t.sda, t.scl, t.i2cAddress, t.i2cAddressAlt);
#endif
}

bool InputManager::gt911ReadReg(const uint16_t reg, uint8_t* buf, const uint8_t len) {
  Wire.beginTransmission(gt911Addr);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  const uint8_t got = Wire.requestFrom(gt911Addr, len, static_cast<uint8_t>(true));
  if (got != len) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (uint8_t i = 0; i < len; ++i) {
    buf[i] = Wire.read();
  }
  return true;
}

void InputManager::gt911ClearStatus() {
  Wire.beginTransmission(gt911Addr);
  Wire.write(0x81);
  Wire.write(0x4E);
  Wire.write(static_cast<uint8_t>(0x00));
  Wire.endTransmission();
}

void InputManager::pollGt911(const unsigned long now) {
  if (gt911Addr == 0) {
    return;
  }
  uint8_t status = 0;
  if (!gt911ReadReg(0x814E, &status, 1)) {
    // Keep the last complete frame while the single-touch state remains
    // latched. Clearing only this snapshot makes a transient I2C failure look
    // like a multi-contact release to multi-touch consumers, which can split one
    // physical gesture into two. A confirmed zero-contact frame below clears
    // the snapshot together with the rest of the touch state.
    return;
  }

  // Capacitive home key long-press (status bit 0x10). Fire from the LATCHED
  // down-state + wall clock, BEFORE the buffer-ready gate below: a motionless
  // hold stops producing new-data frames (0x80 stays clear), so gating the hold
  // timer on fresh frames would never let it cross the threshold. The
  // press/release EDGES still come from fresh frames (handled after the gate).
  if (touchHomeKeyDown && !touchHomeKeyLongFired && now - touchHomeKeyDownAt >= HOME_KEY_LONG_PRESS_MS) {
    touchHomeKeyLongEvent = true;  // crossed the threshold (a hold shortcut)
    touchHomeKeyLongFired = true;  // once per hold; also suppresses the release tap
  }

  if (!(status & 0x80)) {  // buffer not ready
    return;
  }

  // Home-key press/release edges (need a fresh frame). Short tap = primary
  // "home" action, fires on release; the long hold above suppresses it.
  const bool homeKeyDown = (status & 0x10) != 0;
  if (homeKeyDown && !touchHomeKeyDown) {  // press edge
    touchHomeKeyEvent = true;
    touchHomeKeyDownAt = now;
    touchHomeKeyLongFired = false;
  } else if (!homeKeyDown && touchHomeKeyDown && !touchHomeKeyLongFired) {
    touchHomeKeyTapEvent = true;  // release edge of a short press
  }
  touchHomeKeyDown = homeKeyDown;

  const uint8_t count = status & 0x0F;
  if (count > 0) {
    // GT911 stores each contact in a contiguous 8-byte record at 0x8150. Read
    // the bounded records in one transaction so every stored contact comes
    // from one coherent controller frame.
    const uint8_t storedCount = std::min<uint8_t>(count, MAX_TOUCH_CONTACTS);
    uint8_t points[MAX_TOUCH_CONTACTS * 8] = {};
    if (gt911ReadReg(0x8150, points, storedCount * 8)) {
      const auto& t = BoardConfig::ACTIVE.touch;
      touchSnapshot.reportedCount = count;
      touchSnapshot.idsStable = !t.gt911CoordsAtByte0;
      touchSnapshot.count = storedCount;
      for (uint8_t i = 0; i < touchSnapshot.count; ++i) {
        const uint8_t* record = points + i * 8;
        touchSnapshot.points[i].id = t.gt911CoordsAtByte0 ? i : (record[0] & 0x0F);
        const uint8_t offset = t.gt911CoordsAtByte0 ? 0 : 1;
        const uint16_t rawX = static_cast<uint16_t>(record[offset]) | (static_cast<uint16_t>(record[offset + 1]) << 8);
        const uint16_t rawY =
            static_cast<uint16_t>(record[offset + 2]) | (static_cast<uint16_t>(record[offset + 3]) << 8);
        TouchPoint& point = touchSnapshot.points[i].point;
        point.valid = true;
        // Panel-native coordinates (calibrated raw range, touch panel's
        // orientation); the app maps to its display/logical frame. Correct
        // digitizer mounting so the touch frame matches the display NATIVE
        // (panel) frame before any orientation mapping: swap axes first, then
        // map with the panel-axis ranges, then per-axis flip.
        const uint16_t sx = t.swapXY ? rawY : rawX;
        const uint16_t sy = t.swapXY ? rawX : rawY;
        point.x = mapTouchAxis(sx, t.rawMinX, t.rawMaxX, t.rawMaxX - t.rawMinX);
        point.y = mapTouchAxis(sy, t.rawMinY, t.rawMaxY, t.rawMaxY - t.rawMinY);
        if (t.flipX) point.x = static_cast<uint16_t>((t.rawMaxX - t.rawMinX) - point.x);
        if (t.flipY) point.y = static_cast<uint16_t>((t.rawMaxY - t.rawMinY) - point.y);
        point.timestamp = now;
      }

      // Gesture state consumes only completed controller frames. A failed
      // status/point read above deliberately does not arrive here, preserving
      // the active sequence through transient I2C failures.
      updateMultiTouchGesture(touchSnapshot, now);

      // Preserve the existing single-touch API from the first contact.
      touchPoint = touchSnapshot.points[0].point;
      if (!touchPressed) {
        touchPressedEvent = true;
        touchDownPoint = touchPoint;  // first contact sample, used for tap
                                      // routing (wasTouchTap)
        touchMovedBeyondTapSlop = false;
        touchMovedBeyondTapReleaseSlop = false;
      }
      touchUpPoint = touchPoint;
      const int dx = static_cast<int>(touchUpPoint.x) - static_cast<int>(touchDownPoint.x);
      const int dy = static_cast<int>(touchUpPoint.y) - static_cast<int>(touchDownPoint.y);
      if (absInt(dx) > TOUCH_TAP_SLOP_PX || absInt(dy) > TOUCH_TAP_SLOP_PX) {
        touchMovedBeyondTapSlop = true;
      }
      if (absInt(dx) > TOUCH_TAP_RELEASE_SLOP_PX || absInt(dy) > TOUCH_TAP_RELEASE_SLOP_PX) {
        touchMovedBeyondTapReleaseSlop = true;
      }
      // A multi-contact gesture must not become a primary-contact tap in
      // existing single-touch consumers.
      if (touchSnapshot.reportedCount > 1) {
        touchMovedBeyondTapSlop = true;
        touchMovedBeyondTapReleaseSlop = true;
      }
#ifdef TOUCH_PROBE_DEBUG
      if (!touchPressed)
        touchDebugPrintf("[touch] press contacts=%u primary=(%u,%u)\n", touchSnapshot.count, touchPoint.x,
                         touchPoint.y);
#endif
      touchPressed = true;
    } else {
      // Retain the last complete snapshot until the controller provides an
      // authoritative contact count. The existing single-touch state is also
      // retained on this transient point-read failure.
    }
  } else {
    touchSnapshot.count = 0;
    touchSnapshot.reportedCount = 0;
    touchSnapshot.idsStable = !BoardConfig::ACTIVE.touch.gt911CoordsAtByte0;
    updateMultiTouchGesture(touchSnapshot, now);
    if (touchPressed) {
      touchReleasedEvent = true;
      lastTouchHeldDurationMs = now - touchDownPoint.timestamp;
      touchUpPoint = touchPoint;  // last contact sample, used for swipe routing
    }
    touchPressed = false;
    touchPoint.valid = false;
  }

  gt911ClearStatus();  // GT911 requires clearing 0x814E after each read
}

#endif  // FREEINK_CAP_TOUCH
