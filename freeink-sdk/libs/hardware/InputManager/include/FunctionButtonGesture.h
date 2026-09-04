#pragma once

#include <cstdint>

namespace freeink::input {

class FunctionButtonGesture {
 public:
  static constexpr uint8_t BACK = 1u << 0;
  static constexpr uint8_t CONFIRM = 1u << 1;
  static constexpr uint8_t LEFT = 1u << 2;
  static constexpr uint8_t RIGHT = 1u << 3;
  static constexpr uint8_t UP = 1u << 4;
  static constexpr uint8_t DOWN = 1u << 5;

  struct State {
    uint8_t down = 0;
    uint8_t pressed = 0;
    uint8_t released = 0;
    uint32_t startedMs = 0;
  };

  State update(uint8_t raw, uint32_t nowMs) {
    State state;
    uint8_t pressed = 0;
    uint8_t released = 0;

    if (raw != candidate_) {
      candidate_ = raw;
      candidateChangedMs_ = nowMs;
    }

    if (candidate_ != stable_ && elapsed(candidateChangedMs_, nowMs) >= DEBOUNCE_MS) {
      const uint8_t old = stable_;
      stable_ = candidate_;
      pressed = stable_ & static_cast<uint8_t>(~old);
      released = old & static_cast<uint8_t>(~stable_);
      state.pressed = pressed & BACK;
      state.released = released & BACK;

      if (pressed & CONFIRM) handleFunctionPress(state);
      if (released & CONFIRM) handleFunctionRelease(nowMs, state);
    }

    updateDirection(LEFT, UP, leftPressedMs_, pressed, released, nowMs, state);
    updateDirection(RIGHT, DOWN, rightPressedMs_, pressed, released, nowMs, state);

    if ((stable_ & CONFIRM) && (clickPhase_ == ClickPhase::FirstPress || clickPhase_ == ClickPhase::SecondPress) &&
        elapsed(functionPressedMs_, nowMs) >= DOUBLE_CLICK_MS) {
      clickPhase_ = ClickPhase::ConfirmHold;
      state.pressed |= CONFIRM;
      state.startedMs = functionPressedMs_;
    }

    if (clickPhase_ == ClickPhase::PendingConfirm && !(candidate_ & CONFIRM) &&
        elapsed(pendingConfirmMs_, nowMs) > DOUBLE_CLICK_MS) {
      clickPhase_ = ClickPhase::Idle;
      state.pressed |= CONFIRM;
      state.released |= CONFIRM;
      state.startedMs = pendingConfirmPressedMs_;
    }

    state.down = stable_ & BACK;
    state.down |= directionDown_;
    if (clickPhase_ == ClickPhase::ConfirmHold) state.down |= CONFIRM;
    return state;
  }

  bool isDebouncePending() const { return candidate_ != stable_; }

  static constexpr uint32_t DEBOUNCE_MS = 5;
  static constexpr uint32_t DOUBLE_CLICK_MS = 300;
  static constexpr uint32_t DIRECTION_HOLD_MS = 650;

 private:
  enum class ClickPhase : uint8_t { Idle, FirstPress, PendingConfirm, SecondPress, ConfirmHold };

  static constexpr uint32_t elapsed(uint32_t start, uint32_t now) { return now - start; }

  void updateDirection(uint8_t physical, uint8_t logical, uint32_t& pressedMs, uint8_t pressed, uint8_t released,
                       uint32_t nowMs, State& state) {
    if (pressed & physical) pressedMs = nowMs;

    if ((stable_ & physical) && !(directionDown_ & logical) && elapsed(pressedMs, nowMs) >= DIRECTION_HOLD_MS) {
      directionDown_ |= logical;
      state.pressed |= logical;
      state.startedMs = pressedMs;
    }

    if (!(released & physical)) return;

    if (directionDown_ & logical) {
      directionDown_ &= static_cast<uint8_t>(~logical);
      state.released |= logical;
    } else {
      state.pressed |= physical;
      state.released |= physical;
    }
    state.startedMs = pressedMs;
  }

  void handleFunctionPress(State& state) {
    switch (clickPhase_) {
      case ClickPhase::PendingConfirm:
        if (elapsed(pendingConfirmMs_, candidateChangedMs_) <= DOUBLE_CLICK_MS) {
          clickPhase_ = ClickPhase::SecondPress;
        } else {
          state.pressed |= CONFIRM;
          state.released |= CONFIRM;
          state.startedMs = pendingConfirmPressedMs_;
          clickPhase_ = ClickPhase::FirstPress;
        }
        break;
      case ClickPhase::Idle:
      case ClickPhase::FirstPress:
      case ClickPhase::SecondPress:
      case ClickPhase::ConfirmHold:
        clickPhase_ = ClickPhase::FirstPress;
        break;
    }
    functionPressedMs_ = candidateChangedMs_;
  }

  void handleFunctionRelease(uint32_t nowMs, State& state) {
    if (clickPhase_ == ClickPhase::ConfirmHold) {
      state.released |= CONFIRM;
      state.startedMs = functionPressedMs_;
      clickPhase_ = ClickPhase::Idle;
      return;
    }

    switch (clickPhase_) {
      case ClickPhase::SecondPress:
        state.pressed |= BACK;
        state.released |= BACK;
        state.startedMs = pendingConfirmPressedMs_;
        clickPhase_ = ClickPhase::Idle;
        break;
      case ClickPhase::FirstPress:
        pendingConfirmMs_ = nowMs;
        pendingConfirmPressedMs_ = functionPressedMs_;
        clickPhase_ = ClickPhase::PendingConfirm;
        break;
      case ClickPhase::Idle:
      case ClickPhase::PendingConfirm:
      case ClickPhase::ConfirmHold:
        break;
    }
  }

  ClickPhase clickPhase_ = ClickPhase::Idle;
  uint8_t candidate_ = 0;
  uint8_t stable_ = 0;
  uint8_t directionDown_ = 0;
  uint32_t candidateChangedMs_ = 0;
  uint32_t functionPressedMs_ = 0;
  uint32_t pendingConfirmMs_ = 0;
  uint32_t pendingConfirmPressedMs_ = 0;
  uint32_t leftPressedMs_ = 0;
  uint32_t rightPressedMs_ = 0;
};

}  // namespace freeink::input
