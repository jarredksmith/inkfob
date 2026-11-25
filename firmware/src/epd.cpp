#include "epd.h"

#include "driver/gpio.h"

#include "luts_v12.h"

// Pin numbers from Elecrow's example code and the CircuitPython board definition.
static const int PIN_SCK = 12;
static const int PIN_MOSI = 11;
static const int PIN_RES = 10;
static const int PIN_DC = 13;
static const int PIN_CS = 14;
static const int PIN_BUSY = 9;
static const int PIN_EPD_POWER = 7;

// Both panels are 250 lines of 128 bits (16 bytes), of which 122 are visible.
static const int NATIVE_BYTES_PER_LINE = 16;
static const int NATIVE_SIZE = NATIVE_BYTES_PER_LINE * EPD_WIDTH;  // 4000

static PanelType g_panel = PANEL_SSD1680;
static PanelType g_detected = PANEL_SSD1680;
static bool g_flip = false;
static bool g_mirror = true;
static uint8_t g_new[NATIVE_SIZE];   // 1 = black
static uint8_t g_prev[NATIVE_SIZE];  // what the panel is showing now
static bool g_havePrev = false;

// ---------------------------------------------------------------- low level

static void writeByte(uint8_t dat) {
  digitalWrite(PIN_CS, LOW);
  for (uint8_t i = 0; i < 8; i++) {
    digitalWrite(PIN_SCK, LOW);
    digitalWrite(PIN_MOSI, (dat & 0x80) ? HIGH : LOW);
    digitalWrite(PIN_SCK, HIGH);
    dat <<= 1;
  }
  digitalWrite(PIN_CS, HIGH);
}

static void cmd(uint8_t reg) {
  digitalWrite(PIN_DC, LOW);
  writeByte(reg);
  digitalWrite(PIN_DC, HIGH);
}

static void dat(uint8_t d) {
  digitalWrite(PIN_DC, HIGH);
  writeByte(d);
}

// Waits until the BUSY pin leaves its busy level. Gives up after a timeout so a
// wrong panel setting cannot hang the pager.
static void waitWhile(int busyLevel, uint32_t timeoutMs) {
  uint32_t start = millis();
  while (digitalRead(PIN_BUSY) == busyLevel) {
    if (millis() - start > timeoutMs) {
      Serial.println("[epd] busy timeout");
      return;
    }
    delay(2);
  }
}

// --------------------------------------------------- panel A: SSD1680 (V1.0)

static void ssdBusy() {
  waitWhile(HIGH, 6000);
  delayMicroseconds(100);
}

static void ssdInit() {
  delay(10);
  digitalWrite(PIN_RES, HIGH);
  delay(10);
  digitalWrite(PIN_RES, LOW);
  delay(10);
  digitalWrite(PIN_RES, HIGH);
  delay(10);
  ssdBusy();
  cmd(0x12);  // software reset
  ssdBusy();

  cmd(0x01);  // driver output control
  dat(0xF9);
  dat(0x00);
  dat(0x00);
  cmd(0x11);  // data entry mode
  dat(0x03);
  cmd(0x44);  // RAM X range
  dat(0x00);
  dat(0x0F);
  cmd(0x45);  // RAM Y range
  dat(0x00);
  dat(0x00);
  dat(0xF9);
  dat(0x00);
  cmd(0x3C);  // border
  dat(0x01);
  ssdBusy();
  cmd(0x18);  // internal temperature sensor
  dat(0x80);
  cmd(0x4E);
  dat(0x00);
  cmd(0x4F);
  dat(0x00);
  dat(0x00);
  ssdBusy();
}

static void ssdSetCursor() {
  cmd(0x4E);
  dat(0x00);
  cmd(0x4F);
  dat(0x00);
  dat(0x00);
}

// The SSD1680 wants 0 = black, so the buffers are inverted on the way out.
static void ssdWriteRam(uint8_t reg, const uint8_t* buf) {
  ssdSetCursor();
  cmd(reg);
  for (int i = 0; i < NATIVE_SIZE; i++) dat(~buf[i]);
}

static void ssdSleep() {
  cmd(0x10);
  dat(0x01);
  delay(20);
}

static void ssdShow(bool full) {
  ssdInit();
  if (full) {
    ssdWriteRam(0x24, g_new);
    ssdWriteRam(0x26, g_new);
    cmd(0x22);
    dat(0xF7);
    cmd(0x20);
    ssdBusy();
  } else {
    cmd(0x3C);  // leave the border alone during fast updates
    dat(0x80);
    ssdWriteRam(0x26, g_prev);  // what is on screen
    ssdWriteRam(0x24, g_new);   // what should be on screen
    cmd(0x22);
    dat(0xFC);
    cmd(0x20);
    ssdBusy();
  }
  ssdSleep();
}

// ------------------------------------------------ panel B: hardware V1.2
// Command sequences follow Elecrow's arduino-v1.2 example.

static void v12Busy() { waitWhile(LOW, 8000); }

static void v12Init() {
  delay(20);
  digitalWrite(PIN_RES, HIGH);
  delay(10);
  digitalWrite(PIN_RES, LOW);
  delay(100);
  digitalWrite(PIN_RES, HIGH);
  delay(100);

  cmd(0x00);
  dat(0xF7);
  dat(0x8A);
  cmd(0x01);
  dat(0x03);
  dat(0x00);
  dat(0x3F);
  dat(0x3F);
  dat(0x03);
  cmd(0x03);
  dat(0x00);
  cmd(0x06);
  dat(0x27);
  dat(0x27);
  dat(0x2F);
  cmd(0x30);
  dat(0x0D);
  cmd(0x60);
  dat(0x22);
  cmd(0x82);
  dat(0x07);
  cmd(0xE3);
  dat(0x88);
  cmd(0x41);
  dat(0x00);
  cmd(0x61);
  dat(0x80);
  dat(0x00);
  dat(0xFA);
  cmd(0x65);
  dat(0x00);
  dat(0x00);
  dat(0x00);
  cmd(0x50);
  dat(0xB7);
}

static void v12Table(uint8_t reg, const uint8_t* table) {
  cmd(reg);
  for (int i = 0; i < 56; i++) dat(table[i]);
}

static void v12LoadLut(bool full) {
  v12Table(0x20, full ? lut_R20_GC : lut_R20_DU);
  v12Table(0x21, full ? lut_R21_GC : lut_R21_DU);
  v12Table(0x24, full ? lut_R24_GC : lut_R24_DU);
  // Elecrow's example swaps these two tables on alternate refreshes because it
  // leaves the panel running between them. This driver resets the panel and
  // sends both the old and the new image every time, so the first-refresh
  // order is always the right one.
  v12Table(0x22, full ? lut_R22_GC : lut_R22_DU);
  v12Table(0x23, full ? lut_R23_GC : lut_R23_DU);
}

static void v12Show(bool full) {
  v12Init();
  cmd(0x50);
  dat(0xD7);
  // On real hardware this panel shows a 1 bit as white, so the buffers
  // (1 = black) are inverted on the way out.
  cmd(0x10);  // previous image
  for (int i = 0; i < NATIVE_SIZE; i++) dat(~g_prev[i]);
  cmd(0x13);  // new image
  for (int i = 0; i < NATIVE_SIZE; i++) dat(~g_new[i]);
  v12LoadLut(full);
  cmd(0x17);  // power on, refresh, power off
  dat(0xA5);
  delay(10);
  v12Busy();
  cmd(0x07);  // deep sleep
  dat(0xA5);
  delay(20);
}

// ------------------------------------------------------------------ public

namespace Epd {

PanelType begin(PanelType forced) {
  gpio_hold_dis((gpio_num_t)PIN_EPD_POWER);  // may still be latched off from a sleep
  pinMode(PIN_EPD_POWER, OUTPUT);
  digitalWrite(PIN_EPD_POWER, HIGH);
  pinMode(PIN_SCK, OUTPUT);
  pinMode(PIN_MOSI, OUTPUT);
  pinMode(PIN_RES, OUTPUT);
  pinMode(PIN_DC, OUTPUT);
  pinMode(PIN_CS, OUTPUT);
  pinMode(PIN_BUSY, INPUT);
  digitalWrite(PIN_CS, HIGH);
  digitalWrite(PIN_SCK, LOW);
  delay(50);

  // After a reset the SSD1680 idles with BUSY low; the V1.2 panel idles high.
  digitalWrite(PIN_RES, HIGH);
  delay(10);
  digitalWrite(PIN_RES, LOW);
  delay(20);
  digitalWrite(PIN_RES, HIGH);
  delay(250);
  PanelType detected = (digitalRead(PIN_BUSY) == HIGH) ? PANEL_V12 : PANEL_SSD1680;
  g_detected = detected;
  g_panel = (forced == PANEL_AUTO) ? detected : forced;
  Serial.printf("[epd] detected panel %d, using %d\n", detected, g_panel);

  memset(g_prev, 0x00, sizeof(g_prev));
  g_havePrev = false;
  return g_panel;
}

void powerOff() {
  // Nothing may stay high on the panel's pins once its supply is cut.
  for (int pin : {PIN_SCK, PIN_MOSI, PIN_RES, PIN_DC, PIN_CS}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }
  gpio_hold_dis((gpio_num_t)PIN_EPD_POWER);
  pinMode(PIN_EPD_POWER, OUTPUT);
  digitalWrite(PIN_EPD_POWER, LOW);
  gpio_hold_en((gpio_num_t)PIN_EPD_POWER);  // stays off through deep sleep
  g_havePrev = false;
}

PanelType panel() { return g_panel; }

PanelType detected() { return g_detected; }

void setFlip(bool flipped) { g_flip = flipped; }

void setMirror(bool mirrored) { g_mirror = mirrored; }

void show(const uint8_t* canvas, bool full) {
  // Rotate the landscape canvas into the panel's portrait memory layout.
  memset(g_new, 0x00, sizeof(g_new));
  const int canvasStride = (EPD_WIDTH + 7) / 8;
  for (int y = 0; y < EPD_HEIGHT; y++) {
    const uint8_t* row = canvas + y * canvasStride;
    for (int x = 0; x < EPD_WIDTH; x++) {
      if (!(row[x >> 3] & (0x80 >> (x & 7)))) continue;
      int px = g_flip ? (EPD_WIDTH - 1 - x) : x;
      int py = g_flip ? (EPD_HEIGHT - 1 - y) : y;
      int bit = py;                     // position along the 128-bit line
      // Elecrow's example maps this the other way round, which came out
      // mirrored on real hardware, so mirroring is on by default.
      int line = g_mirror ? px : (EPD_WIDTH - 1 - px);  // which of the 250 lines
      g_new[line * NATIVE_BYTES_PER_LINE + (bit >> 3)] |= (0x80 >> (bit & 7));
    }
  }

  if (!g_havePrev) full = true;
  if (g_panel == PANEL_V12) {
    v12Show(full);
  } else {
    ssdShow(full);
  }
  memcpy(g_prev, g_new, sizeof(g_prev));
  g_havePrev = true;
}

}  // namespace Epd
