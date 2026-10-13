#pragma once
// RGB panel bus with bounce buffers, for LovyanGFX's Panel_RGB.
//
// LovyanGFX's own Bus_RGB lets the DMA unit read the frame buffer straight from
// PSRAM. Flash and PSRAM share one SPI bus, and the DMA FIFO in front of the
// LCD unit holds some 40 pixels: every time the bus is busy for a moment the
// rest of the line arrives late and the picture tears and shifts sideways. At
// the 12..16 MHz other 800x480 boards get away with, this panel flickers (its
// datasheet wants 23..27 MHz, the vendor runs 19).
//
// So, as ESP-IDF 5 and the vendor's examples do it: the DMA unit only ever
// reads a few small buffers in internal RAM, one after the other, in a circle.
// Each time it has finished one, the next piece of the frame buffer is copied
// into it. Arduino core 2 (ESP-IDF 4.4) has no driver for this.
//
// The copy is not done by the processor: at 20 MHz it would spend two thirds
// of one core in that interrupt (measured: 237 us for a buffer that has to be
// ready in 345 us) and still be late for every sixth picture. A second DMA
// channel copies memory to memory instead (esp_async_memcpy); it reads PSRAM
// behind the processor's cache, so whoever has drawn into the frame buffer
// calls written() for those lines.
#include "config.h"
#ifdef LCD_RGB_PANEL
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <esp_private/gdma.h>
#include <esp_async_memcpy.h>

// Panel_RGB keeps RGB565 unless told otherwise, and offers no setter for it.
struct PanelRgb : lgfx::Panel_RGB {
  PanelRgb() {
#if LCD_RGB_BITS == 8
    _write_depth = lgfx::color_depth_t::rgb332_1Byte;
    _read_depth = lgfx::color_depth_t::rgb332_1Byte;
#endif
  }
};

class BusRgbBounce : public lgfx::Bus_RGB {
public:
  bool init() override;
  void release() override {}
  uint8_t *getDMABuffer(uint32_t) override { return fb; }

  // for the debug console
  volatile uint32_t statFrames = 0, statResync = 0, statCopyUsMax = 0, statCopyUsSum = 0, statCopies = 0, statByHand = 0, statSkipped = 0;
  uint32_t pclkHz = 0;

  // Lines y .. y+h-1 of the frame buffer have been drawn: carry them from the
  // processor's cache to the PSRAM, where the copying DMA channel reads.
  void written(int y, int h);

  // Two frame buffers, no copying: show() names the buffer the panel is fed
  // from once the picture now on its way is complete; shown() is false until
  // then. The caller has carried the buffer to the PSRAM (carry()) and leaves
  // the one that was on show alone until shown() says so.
  uint8_t *frame() const { return fb; }
  size_t frameLen() const { return fbLen; }
  static const size_t FRAME_SLACK = 64;   // spare bytes behind a frame buffer, see init()
  void show(uint8_t *buffer) { if (buffer != fb) fbNext = buffer; }
  bool shown() const { return fbNext == nullptr; }
  void carry(uint8_t *buffer, size_t len);
  // Fill a frame buffer that is not on show with zeros (black), by DMA: the
  // processor's way through its cache reads every line from the PSRAM before
  // it overwrites it (20 ms for one buffer, measured). Returns when done.
  // false: not available, the caller clears by hand.
  bool clear(uint8_t *buffer);
  void cleared() { clearsLeft = clearsLeft - 1; }

  // interrupt side (rgb_bounce.cpp)
  void copied(int64_t t0);
  void start();
  void onEof(intptr_t descAddr);
  void onVsync();

private:
  uint8_t *volatile fb = nullptr, *volatile fbNext = nullptr;
  uint8_t *bb[LCD_RGB_BOUNCE_BUFS] = {};
  size_t fbLen = 0, bbLen = 0;
  volatile size_t fbPos = 0;
  volatile uint32_t eofInFrame = 0;
  volatile bool firstVsync = true;
  volatile uint8_t lastEof = LCD_RGB_BOUNCE_BUFS - 1;
  uint32_t chunksPerFrame = 0;
  dma_descriptor_t *desc = nullptr;
  size_t descPerBb = 0;
  gdma_channel_handle_t dma = nullptr;
  async_memcpy_t mcp = nullptr, mcpFill = nullptr;   // refilling the bounce buffers has a channel to itself
  uint8_t *zeros = nullptr;
  volatile int clearsLeft = 0;
  size_t lineLen = 0;
  intr_handle_t intr = nullptr;
};
#endif
