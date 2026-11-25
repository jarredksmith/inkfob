// E-ink driver for the Elecrow CrowPanel 2.13" (250 x 122).
// The board ships with one of two different panels, so both are supported
// and the right one is picked at boot.
#pragma once
#include <Arduino.h>

#define EPD_WIDTH 250
#define EPD_HEIGHT 122

enum PanelType : uint8_t {
  PANEL_AUTO = 0,
  PANEL_SSD1680 = 1,  // hardware V1.0
  PANEL_V12 = 2,      // hardware V1.2 (different controller, external waveform tables)
};

namespace Epd {
// Powers the screen and works out which panel is fitted (unless forced).
PanelType begin(PanelType forced);
PanelType panel();
PanelType detected();  // what the automatic check found, even if overridden
void setFlip(bool flipped);
void setMirror(bool mirrored);

// canvas: 1 bit per pixel, row-major, 32 bytes per row, MSB first, 1 = black.
// full = true does a slow flashing refresh that clears ghosting.
void show(const uint8_t* canvas, bool full);

// Cuts power to the screen before deep sleep. The picture stays. Call begin()
// again before the next show().
void powerOff();
}  // namespace Epd
