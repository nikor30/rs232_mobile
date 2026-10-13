#include "rgb_bounce.h"
#ifdef LCD_RGB_PANEL
#include <Arduino.h>
#include <hal/lcd_ll.h>
#include <soc/lcd_cam_struct.h>
#include <soc/lcd_periph.h>
#include <driver/periph_ctrl.h>
#include <driver/gpio.h>
#include <hal/gpio_hal.h>
#include <esp_rom_gpio.h>
#include <esp_heap_caps.h>
#include <esp_intr_alloc.h>
#include <esp_timer.h>
#include <rom/ets_sys.h>
#include <esp32s3/rom/cache.h>

static const size_t DESC_MAX = 4000;       // bytes per DMA descriptor (limit 4095, a multiple of 4)

static void pinSignal(int pin, int sig) {
  if (pin < 0) return;
  gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[pin], PIN_FUNC_GPIO);
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
  esp_rom_gpio_connect_out_signal(pin, sig, false, false);
}

// (Re)start a frame from its first byte: both buffers filled, DMA at the first
// descriptor, then the LCD unit. Called once from init() and from the vsync
// interrupt when the two have lost step.
void IRAM_ATTR BusRgbBounce::start() {
  lcd_cam_dev_t *dev = &LCD_CAM;
  lcd_ll_stop(dev);
  gdma_stop(dma);
  gdma_reset(dma);
  lcd_ll_fifo_reset(dev);
  if (fbNext) { fb = fbNext; fbNext = nullptr; }
  const uint8_t *from = fb;
  for (int b = 0; b < LCD_RGB_BOUNCE_BUFS; b++) memcpy(bb[b], from + b * bbLen, bbLen);
  fbPos = (LCD_RGB_BOUNCE_BUFS * bbLen) % fbLen;
  eofInFrame = 0;
  firstVsync = true;
  lastEof = LCD_RGB_BOUNCE_BUFS - 1;
  gdma_start(dma, (intptr_t)desc);
  ets_delay_us(1);                         // let the DMA fill the LCD FIFO
  lcd_ll_start(dev);
}

static volatile int64_t copyStart[LCD_RGB_BOUNCE_BUFS];
static BusRgbBounce *theBus;
static bool IRAM_ATTR copiedCb(async_memcpy_t, async_memcpy_event_t *, void *arg) {
  theBus->copied(copyStart[(uintptr_t)arg]);     // arg: the buffer's number
  return false;
}

void IRAM_ATTR BusRgbBounce::copied(int64_t t0) {
  uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
  if (us > statCopyUsMax) statCopyUsMax = us;
  statCopyUsSum = statCopyUsSum + us;
  statCopies = statCopies + 1;
}

// The DMA unit has sent buffer i and goes on with the next ones: refill this
// one with the piece that follows what the others hold.
void IRAM_ATTR BusRgbBounce::onEof(intptr_t descAddr) {
  uint8_t i = (uint8_t)(((dma_descriptor_t *)descAddr - desc) / descPerBb);
  if (i >= LCD_RGB_BOUNCE_BUFS) return;
  size_t pos = fbPos;
  uint8_t *fb = this->fb;
  uint8_t expected = (lastEof + 1) % LCD_RGB_BOUNCE_BUFS;
  if (i != expected) {
    // Not the buffer whose turn it was: this interrupt came so late that
    // others went by unanswered. They are on their way to the panel with old
    // lines (stripes, for one picture); skipping their pieces keeps everything
    // after them in place.
    uint8_t missed = (i + LCD_RGB_BOUNCE_BUFS - expected) % LCD_RGB_BOUNCE_BUFS;
    pos = (pos + missed * bbLen) % fbLen;
    eofInFrame = eofInFrame + missed;
    statSkipped = statSkipped + missed;
  }
  lastEof = i;
  copyStart[i] = esp_timer_get_time();
  if (esp_async_memcpy(mcp, bb[i], fb + pos, bbLen, copiedCb, (void *)(uintptr_t)i) != ESP_OK)
  {
    memcpy(bb[i], fb + pos, bbLen);        // no room in its queue: by hand, late but right
    statByHand = statByHand + 1;
  }
  pos += bbLen;
  if (pos >= fbLen) {                      // the next piece opens a new picture: the moment to change buffers
    pos = 0;
    if (fbNext) { this->fb = fbNext; fbNext = nullptr; }
  }
  fbPos = pos;
  eofInFrame = eofInFrame + 1;
}

static const size_t FILL_BLOCK = 1984;     // zeros in internal RAM: little of it, and a multiple of 64 for the PSRAM side

static bool IRAM_ATTR clearedCb(async_memcpy_t, async_memcpy_event_t *, void *) {
  theBus->cleared();
  return false;
}

bool BusRgbBounce::clear(uint8_t *buffer) {
  if (!mcpFill || !zeros || !buffer) return false;
  // whatever the cache still holds of this buffer is about to be out of date
  Cache_Invalidate_Addr((uint32_t)buffer, (uint32_t)fbLen);
  size_t off = 0;
  while (off < fbLen) {
    size_t n = fbLen - off > FILL_BLOCK ? FILL_BLOCK : fbLen - off;
    while (clearsLeft >= 2) delayMicroseconds(20);     // little at a time: the picture stream needs the same bus
    noInterrupts();
    clearsLeft = clearsLeft + 1;
    interrupts();
    if (esp_async_memcpy(mcpFill, buffer + off, zeros, n, clearedCb, nullptr) != ESP_OK) {
      noInterrupts();
      clearsLeft = clearsLeft - 1;
      interrupts();
      delayMicroseconds(100);
      continue;
    }
    off += n;
  }
  for (int i = 0; i < 400 && clearsLeft > 0; i++) delayMicroseconds(50);
  return true;
}

void BusRgbBounce::carry(uint8_t *buffer, size_t len) {
  if (buffer && len) Cache_WriteBack_Addr((uint32_t)buffer, (uint32_t)len);
}

void BusRgbBounce::written(int y, int h) {
  if (!fb || h <= 0) return;
  Cache_WriteBack_Addr((uint32_t)(fb + (size_t)y * lineLen), (uint32_t)h * lineLen);
}

// Once per picture: has the DMA unit delivered exactly one frame since the last
// one? If an interrupt came too late it has not, and the picture would stay
// shifted for good - start over.
void IRAM_ATTR BusRgbBounce::onVsync() {
  statFrames = statFrames + 1;
  if (firstVsync) { firstVsync = false; eofInFrame = 0; return; }
  if (eofInFrame != chunksPerFrame) {
    statResync = statResync + 1;
    start();
    return;
  }
  eofInFrame = 0;
}

static bool IRAM_ATTR eofCb(gdma_channel_handle_t, gdma_event_data_t *ev, void *arg) {
  ((BusRgbBounce *)arg)->onEof(ev->tx_eof_desc_addr);
  return false;
}

static void IRAM_ATTR lcdIsr(void *arg) {
  lcd_cam_dev_t *dev = &LCD_CAM;
  uint32_t st = lcd_ll_get_interrupt_status(dev);
  lcd_ll_clear_interrupt_status(dev, st);
  if (st & LCD_LL_EVENT_VSYNC_END) ((BusRgbBounce *)arg)->onVsync();
}

bool BusRgbBounce::init() {
  const auto &cfg = config();
  const int W = cfg.panel->width(), H = cfg.panel->height();
  const int bytes = LCD_RGB_BITS / 8;
  lineLen = (size_t)W * bytes;
  fbLen = lineLen * H;
  bbLen = lineLen * LCD_RGB_BOUNCE_LINES;
  chunksPerFrame = fbLen / bbLen;
  if (fbLen % bbLen) return false;         // the frame must be a whole number of buffers

  // FRAME_SLACK: a canvas that draws straight into a frame buffer (lcd_ui.cpp)
  // fills in whole words and may step a few bytes past the last pixel.
  fb = (uint8_t *)heap_caps_aligned_alloc(64, fbLen + FRAME_SLACK, MALLOC_CAP_SPIRAM);
  // The bounce buffers are not taken from the heap: allocated there after the
  // radio has started, they end up in the middle of it, and the largest free
  // block is then too small for the 24 kB stack of the control centre's key
  // exchange (c2.cpp, big()).
  static uint8_t store[LCD_RGB_BOUNCE_BUFS][LCD_W * (LCD_RGB_BITS / 8) * LCD_RGB_BOUNCE_LINES] __attribute__((aligned(4)));
  bool got = fb != nullptr && bbLen <= sizeof(store[0]);
  for (int b = 0; b < LCD_RGB_BOUNCE_BUFS; b++) bb[b] = store[b];
  descPerBb = (bbLen + DESC_MAX - 1) / DESC_MAX;
  const size_t descs = LCD_RGB_BOUNCE_BUFS * descPerBb;
  desc = (dma_descriptor_t *)heap_caps_calloc(descs, sizeof(dma_descriptor_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!got || !desc) return false;
  memset(fb, 0, fbLen);
  theBus = this;

  // buffer 0 -> 1 -> ... -> 0; the last descriptor of each buffer raises "EOF"
  for (size_t b = 0; b < LCD_RGB_BOUNCE_BUFS; b++) {
    size_t left = bbLen;
    for (size_t d = 0; d < descPerBb; d++) {
      dma_descriptor_t &n = desc[b * descPerBb + d];
      size_t len = left > DESC_MAX ? DESC_MAX : left;
      n.buffer = bb[b] + (bbLen - left);
      n.dw0.size = len;
      n.dw0.length = len;
      n.dw0.owner = 1;                     // DMA
      n.dw0.suc_eof = d == descPerBb - 1;
      n.next = &desc[(b * descPerBb + d + 1) % descs];
      left -= len;
    }
  }

  // ---- LCD unit
  periph_module_enable(lcd_periph_signals.panels[0].module);
  periph_module_reset(lcd_periph_signals.panels[0].module);
  lcd_cam_dev_t *dev = &LCD_CAM;
  lcd_ll_enable_clock(dev, true);
  // pixel clock from the 240 MHz PLL, whole-numbered dividers only
  uint32_t div = (240000000UL + cfg.freq_write / 2) / cfg.freq_write;
  if (div < 2) div = 2;
  bool half = !(div & 1);                  // even: divide by div/2, then by 2 in the prescaler
  pclkHz = 240000000UL / div;
  {
    typeof(dev->lcd_clock) c;
    c.val = 0;
    c.lcd_clk_sel = 2;                     // 240 MHz
    c.lcd_clkm_div_num = half ? div / 2 : div;
    c.lcd_clkm_div_a = 0;
    c.lcd_clkm_div_b = 0;
    c.lcd_clkcnt_n = 1;
    c.lcd_clk_equ_sysclk = !half;
    c.lcd_ck_idle_edge = 0;
    c.lcd_ck_out_edge = cfg.pclk_idle_high;   // 1: data changes on the rising edge, the panel takes it on the falling one
    c.clk_en = 1;
    dev->lcd_clock.val = c.val;
  }
  lcd_ll_reset(dev);
  lcd_ll_fifo_reset(dev);
  lcd_ll_enable_rgb_mode(dev, true);
  lcd_ll_enable_rgb_yuv_convert(dev, false);
  lcd_ll_set_phase_cycles(dev, 0, 0, 1);   // data phase only
  lcd_ll_set_data_width(dev, LCD_RGB_BITS);
  lcd_ll_reverse_bit_order(dev, false);
  lcd_ll_swap_byte_order(dev, 16, false);
  lcd_ll_set_blank_cycles(dev, 1, 1);
  lcd_ll_set_horizontal_timing(dev, cfg.hsync_pulse_width, cfg.hsync_back_porch, W, cfg.hsync_front_porch);
  lcd_ll_set_vertical_timing(dev, cfg.vsync_pulse_width, cfg.vsync_back_porch, H, cfg.vsync_front_porch);
  lcd_ll_enable_output_hsync_in_porch_region(dev, true);
  lcd_ll_set_hsync_position(dev, 0);
  lcd_ll_set_idle_level(dev, cfg.hsync_polarity, cfg.vsync_polarity, cfg.de_idle_high);
  lcd_ll_enable_output_always_on(dev, true);
  lcd_ll_enable_auto_next_frame(dev, true);   // one frame after the other without a restart

  // LovyanGFX keeps RGB565 with the bytes swapped in the frame buffer, so the
  // two halves of the data bus change places (as in its own Bus_RGB).
#if LCD_RGB_BITS == 16
  static const uint8_t SIG[16] = {8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7};
#else
  // RGB332 on eight signals: blue 0-1, green 2-4, red 5-7. Each colour's bits
  // are repeated down the panel's lines so that full stays full (B0..B4,
  // G0..G5, R0..R4 - the table LovyanGFX uses for this).
  static const uint8_t SIG[16] = {1, 0, 1, 0, 1, 2, 3, 4, 2, 3, 4, 5, 6, 5, 6, 7};
#endif
  for (int i = 0; i < 16; i++) pinSignal(cfg.pin_data[i], lcd_periph_signals.panels[0].data_sigs[SIG[i]]);
  pinSignal(cfg.pin_henable, lcd_periph_signals.panels[0].de_sig);
  pinSignal(cfg.pin_hsync, lcd_periph_signals.panels[0].hsync_sig);
  pinSignal(cfg.pin_vsync, lcd_periph_signals.panels[0].vsync_sig);
  pinSignal(cfg.pin_pclk, lcd_periph_signals.panels[0].pclk_sig);

  // ---- DMA
  gdma_channel_alloc_config_t ac = {};
  ac.direction = GDMA_CHANNEL_DIRECTION_TX;
  if (gdma_new_channel(&ac, &dma) != ESP_OK) return false;
  gdma_connect(dma, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0));
  gdma_transfer_ability_t ability = {};
  ability.sram_trans_align = 4;
  ability.psram_trans_align = 64;
  gdma_set_transfer_ability(dma, &ability);
  async_memcpy_config_t mc = ASYNC_MEMCPY_DEFAULT_CONFIG();
  mc.backlog = 16;                         // DMA descriptors: a buffer takes up to four, copies may overlap
  mc.sram_trans_align = 4;
  mc.psram_trans_align = 64;               // frame buffer and buffer length are multiples of 64
  if (esp_async_memcpy_install(&mc, &mcp) != ESP_OK) return false;
  // Try one copy here, where a refusal can still be printed (in the interrupt it cannot).
  esp_err_t tried = esp_async_memcpy(mcp, bb[0], fb, bbLen, nullptr, nullptr);
  Serial.printf("[LCD]  DMA-Kopie %u Bytes PSRAM %p -> RAM %p: %s\n", (unsigned)bbLen, fb, bb[0], tried == ESP_OK ? "ok" : esp_err_to_name(tried));
  if (tried != ESP_OK) return false;
  delay(2);
  // From here on it is called in an interrupt, where its error message would
  // end in abort() (printf takes a lock): no messages, onEof() has a fallback.
  esp_log_level_set("async_memcpy", ESP_LOG_NONE);
  // a second channel for clear(); the picture works without it
  static uint8_t zeroStore[FILL_BLOCK] __attribute__((aligned(4)));   // .bss: zeros already
  zeros = zeroStore;
  {
    async_memcpy_config_t fc = ASYNC_MEMCPY_DEFAULT_CONFIG();
    fc.backlog = 8;
    fc.sram_trans_align = 4;
    fc.psram_trans_align = 64;
    if (esp_async_memcpy_install(&fc, &mcpFill) != ESP_OK) mcpFill = nullptr;
  }
  gdma_tx_event_callbacks_t cbs = {};
  cbs.on_trans_eof = eofCb;
  gdma_register_tx_event_callbacks(dma, &cbs, this);

  lcd_ll_enable_interrupt(dev, LCD_LL_EVENT_VSYNC_END, true);
  if (esp_intr_alloc_intrstatus(lcd_periph_signals.panels[0].irq_id, ESP_INTR_FLAG_SHARED | ESP_INTR_FLAG_INTRDISABLED,
                                (uint32_t)lcd_ll_get_interrupt_status_reg(dev), LCD_LL_EVENT_VSYNC_END, lcdIsr, this, &intr) != ESP_OK)
    return false;
  esp_intr_enable(intr);

  start();
  return true;
}
#endif
