#pragma once
#include <cmath>
#include <cstdint>

enum class GOCrossfadeMode : uint8_t {
  Linear = 0,
  SinEqualPower,  // a=cos(pi/2*t), b=sin(pi/2*t)
  Sin2,           // a=(1-t)^2, b=t^2
  SqrtEqualPower, // a=sqrt(1-t), b=sqrt(t)
  X2,             // a=1-(t*t),  b=t*t
  Custom          // Platzhalter
};

struct GOCrossfadeGains {
  float a, b;
};

constexpr float kGOCrossfadeHalfPi = 1.57079632679489661923f;

inline GOCrossfadeGains go_crossfade_eval(GOCrossfadeMode m, float t) {
  if (t < 0.f)
    t = 0.f;
  else if (t > 1.f)
    t = 1.f;
  switch (m) {
  case GOCrossfadeMode::Linear:
    return {1.f - t, t};
  case GOCrossfadeMode::SinEqualPower:
    return {std::cos(kGOCrossfadeHalfPi * t), std::sin(kGOCrossfadeHalfPi * t)};
  case GOCrossfadeMode::Sin2: {
    float a = std::cos(kGOCrossfadeHalfPi * t);
    a *= a;
    float b = std::sin(kGOCrossfadeHalfPi * t);
    b *= b;
    return {a, b};
  }
  case GOCrossfadeMode::SqrtEqualPower:
    return {std::sqrt(1.f - t), std::sqrt(t)};
  case GOCrossfadeMode::X2:
    return {1.f - t * t, t * t};
  case GOCrossfadeMode::Custom:
    return {0.f, 1.f};
  }
  return {1.f - t, t};
}
