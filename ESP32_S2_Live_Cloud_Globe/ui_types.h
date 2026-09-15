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


// -----------------------------------------------------------------------------
// ISS true-3D orbit type
// -----------------------------------------------------------------------------
//
// Defined in a header included near the top of the Arduino sketch because the
// Arduino build system auto-generates function prototypes before most .ino
// declarations. Any function prototype that mentions ISSSpacePoint therefore
// requires this type to be visible before prototype generation.

struct ISSSpacePoint {
  // Earth-fixed geocentric XYZ, expressed in Earth-radius units.
  //
  // 1.0 = WGS-84 equatorial Earth radius used by AioP13.
  // The ISS is normally around 1.065 Earth radii from Earth's centre,
  // so on a 112 px-radius globe it appears only about 7 px above the limb.
  float xER;
  float yER;
  float zER;
};
