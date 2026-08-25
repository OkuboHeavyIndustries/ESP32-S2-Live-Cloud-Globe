#pragma once
#include <Arduino.h>

enum class ReplayMode : uint8_t {
  Live = 0,
  Hours24,
  Days7,
  Days30
};

enum class ButtonEvent : uint8_t {
  None = 0,
  Single,
  Double,
  Triple,
  LongPress
};
