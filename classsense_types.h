#pragma once
// Tipos de ClassSense Alerts 2.4 Plus. Van en un archivo aparte para que Arduino IDE
// los conozca antes de generar los prototipos de las funciones del sketch.
#include <Arduino.h>

constexpr uint32_t LONG_PRESS_MS = 700;

struct Note { uint16_t freq; uint16_t ms; };

struct Button {
  uint8_t pin;
  bool stable = false, lastRaw = false, longFired = false;
  uint32_t changedAt = 0, pressedAt = 0;
  // 0 = nada, 1 = pulsacion corta (al soltar), 2 = pulsacion larga (al mantener)
  uint8_t poll(uint32_t now) {
    bool raw = digitalRead(pin) == LOW;
    if (raw != lastRaw) { lastRaw = raw; changedAt = now; }
    if (raw != stable && now - changedAt >= 25) {
      stable = raw;
      if (stable) { pressedAt = now; longFired = false; }
      else if (!longFired) return 1;
    }
    if (stable && !longFired && now - pressedAt >= LONG_PRESS_MS) { longFired = true; return 2; }
    return 0;
  }
};

enum Overlay : uint8_t { OV_NONE, OV_VOLUME, OV_BRIGHTNESS, OV_MESSAGE, OV_IP, OV_REACTION };
