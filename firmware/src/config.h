#pragma once
// ============================================================================
//  RS232 Web Console - hardware + default configuration
//
//  Two hardware profiles (selected in platformio.ini):
//    default              LilyGO T-RSS3 (ESP32-S3, isolated RS232, WS2812)
//    BOARD_ESP32_DEVKIT   mock-up: ESP32 DevKit (WROOM-32, 30 pin) + MAX3232
//                         module "HW-044" with DB9 female
// ============================================================================

#define FW_NAME     "RS232 Web Console"
#define FW_VERSION  "1.7.1"

#if defined(BOARD_ESP32_DEVKIT)
// ---------------------------------------------------------------- ESP32 DevKit
#define BOARD_NAME      "ESP32 DevKit + MAX3232"
#define PIN_LED         2   // blue on-board LED "D2"
#define LED_IS_WS2812   0
#define PIN_KEY         0   // BOOT button (usable after start-up)
#define PIN_I2C_SDA    21   // optional OLED on D21/D22
#define PIN_I2C_SCL    22
// The BOOT button (PIN_KEY) already does short = OLED page, long = baud rate, so
// no extra panel button is needed on the mock-up. Set this to a free GPIO (and
// take that pin out of PINS_RX/PINS_TX below) if one should be wired anyway.
#define PIN_EXT_KEY    -1

// Serial ports: default RX/TX GPIOs (web UI -> Ports can change them).
// Port 1: D16 (RX2) <- module "RXD", D17 (TX2) -> module "TXD"
#define PORT_DEFAULT_PINS {{16, 17}, {19, 18}, {27, 26}, {33, 25}}
// GPIOs offered in the web UI. Left out on purpose: 0/2/12/15 (strapping, button,
// LED), 1/3 (USB log), 6-11 (flash), 21/22 (OLED). 5 and 14 only as RX (they
// toggle at boot), 34-39 are input only.
#define PINS_RX  {4, 5, 13, 14, 16, 17, 18, 19, 23, 25, 26, 27, 32, 33, 34, 35, 36, 39}
#define PINS_TX  {4, 13, 16, 17, 18, 19, 23, 25, 26, 27, 32, 33}
#define PINS_ADC {32, 33, 34, 35, 36, 39}      // ADC1 (ADC2 does not work with WiFi)
#define PIN_NAMES "36:VP,39:VN,16:RX2,17:TX2"  // silkscreen names that are not "D<n>"
#define PIN_PREFIX "D"

// Battery measurement is optional on the mock-up (divider to an ADC pin, web UI -> Ports)
#define BAT_PIN_DEFAULT  -1
#define BAT_TYPE_DEFAULT  1       // 0 = LiPo 1S, 1 = NiCd/NiMH 4 cells
#define BAT_DIV_DEFAULT  30       // divider x10: 30 = 3:1 (three equal resistors)
// Known hotspot password so the mock-up works without OLED / serial console.
// Change it in the web UI (Setup) after the first login.
#ifndef FIXED_AP_PASS
#define FIXED_AP_PASS  "rs232mockup"
#endif
#define CPU_MHZ          240      // USB powered -> full speed
#define TX_POWER_DEFAULT 34       // WIFI_POWER_8_5dBm: plenty for a few metres, gentle on USB power
#ifndef DEBUG_NET_LOG
#define DEBUG_NET_LOG    1        // mock-up: log every DNS query of the captive portal
#endif

#elif defined(BOARD_VIEWE_5INCH)
// ------------------------------------------------- VIEWE UEDX80480050E-WB (5", 800x480)
// ESP32-S3 with an RGB panel, GT911 touch and an SD slot. The panel eats about
// twenty GPIOs, so exactly two pins are left over (IO17/IO18) plus the UART pair
// IO43/IO44 that carries the USB debug console. Ports 3 and 4 therefore cannot be
// wired natively at all - they are what the I2C daughterboard (2x SC16IS752) is
// for, and its bus is the touch bus below.
#define BOARD_NAME      "VIEWE 5\" ESP32-S3"
#define HAS_PANEL        1        // 800x480 RGB + GT911 touch -> LVGL GUI instead of the OLED
#define HAS_SDCARD       1        // SPI slot: session logs, stored configurations, transfer files
#define PIN_LED          0        // WS2812, shares IO0 with the BOOT button
#define LED_IS_WS2812    1
#define PIN_KEY         -1        // the touchscreen replaces the button
#define PIN_EXT_KEY     -1
#define PIN_I2C_SDA     19        // GT911 touch bus - the daughterboard rides along here
#define PIN_I2C_SCL     20
#define PIN_SD_CS       10        // SD card on SPI (IO10-13)
#define PIN_SD_MOSI     11
#define PIN_SD_SCLK     12
#define PIN_SD_MISO     13

// Port 1 on the two free GPIOs. IO43/44 are offered as well, but they are the USB
// debug console: using them silences the boot log.
#define PORT_DEFAULT_PINS {{18, 17}, {44, 43}, {18, 17}, {44, 43}}
#define PINS_RX  {17, 18, 43, 44}
#define PINS_TX  {17, 18, 43, 44}
#define PINS_ADC {-1}             // free pins are ADC2 only, which does not work with WiFi
#define PIN_NAMES "43:UART TX,44:UART RX,19:TP-SDA,20:TP-SCL"
#define PIN_PREFIX "IO"

#define BAT_PIN_DEFAULT  -1       // USB powered
#define BAT_TYPE_DEFAULT  0
#define BAT_DIV_DEFAULT  20
#define CPU_MHZ          240
#define TX_POWER_DEFAULT 44       // WIFI_POWER_11dBm

#else
// ---------------------------------------------------------------- LilyGO T-RSS3
#define BOARD_NAME      "LilyGO T-RSS3"
// fixed on the board (LilyGO schematic / utilities.h)
#define PIN_LED         1   // WS2812 status LED
#define LED_IS_WS2812   1
#define PIN_KEY         5   // onboard button "IO05" (10k pull-up, active low)
// add-ons on the 1.27 mm 2x15 header J4
//  Left column: 5V IO9 IO8 IO7 IO6 GND GND RX TX IO40 IO39 IO38 IO37 IO36 IO35
//  Right column: 3V3 IO10 IO11 IO12 IO13 IO14 IO15 IO16 IO17 IO18 IO21 IO47 IO33 IO34 IO48
#define PIN_I2C_SDA     8   // OLED SDA
#define PIN_I2C_SCL     9   // OLED SCL
#define PIN_EXT_KEY     6   // optional panel push button to GND.       -1 = none

// Serial ports: port 1 = isolated RSM232 module on the board (IO42 RX, IO41 TX),
// ports 2-4 = extra MAX3232 modules on header J4 (web UI -> Ports can change them).
#define PORT_DEFAULT_PINS {{42, 41}, {10, 11}, {12, 13}, {14, 15}}
#define PINS_RX  {10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 47, 48}
#define PINS_TX  {10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 47, 48}
#define PINS_ADC {7, 10}                      // ADC1 pins on the header
#define PIN_NAMES "42:RS232 RX,41:RS232 TX"
#define PIN_PREFIX "IO"

#define BAT_PIN_DEFAULT   7       // LiPo+ via 100k/100k divider (ADC1_CH6)
#define BAT_TYPE_DEFAULT  0       // LiPo 1S
#define BAT_DIV_DEFAULT  20       // 2:1
#define CPU_MHZ          80       // battery: 80 MHz is plenty and saves ~20 mA
#define TX_POWER_DEFAULT 44       // WIFI_POWER_11dBm
#endif

// ---- Feature flags (set by the board profiles above) -------------------------
#ifndef HAS_PANEL
#define HAS_PANEL        0        // 1 = LVGL GUI on an RGB panel instead of the small OLED
#endif
#ifndef HAS_SDCARD
#define HAS_SDCARD       0
#endif

// ---- Battery measurement ----------------------------------------------------
#define BAT_CAL          1.00f   // fine tune: real_voltage / displayed_voltage
#define BAT_LOW_PCT      10      // warning (LED red blink, OLED + web UI)

// ---- OLED -------------------------------------------------------------------
// Type is selectable in the web UI (0 = SSD1306 0.96", 1 = SH1106 1.3")
#define OLED_ADDR_1     0x3C
#define OLED_ADDR_2     0x3D

// ---- Network ------------------------------------------------------------------
#define AP_SSID_PREFIX   "RS232-"
#define AP_CHANNEL_DEFAULT 0     // 0 = auto (scan at boot, pick the quietest of 1/6/11)
#define AP_MAX_CLIENTS   4
#define DEFAULT_HOSTNAME "rs232"
#define HTTP_PORT        80
#define WS_PORT          81
#define RAW_TCP_PORT     2000     // raw TCP port 1 = 2000, port 2 = 2001 ... (PuTTY "Raw"/"Telnet", nc)
#define TCP_OUT_QUEUE    4096     // per port: bytes waiting for a congested TCP client

// ---- Serial -------------------------------------------------------------------
#define MAX_PORTS        4
#define HW_PORTS         2        // ports 1-2: hardware UART1/UART2, ports 3-4: software UART
#define SW_MAX_BAUD      38400    // software UART limit (bit timing jitter above)
#define DEFAULT_BAUD     9600     // Cisco, Fortinet, Aruba, Juniper default
#define UART_RX_BUF      4096
#define UART_TX_BUF      1024     // hardware UART driver buffer (the bridge paces its writes)
#define SW_RX_BUF        512      // software UART: received bytes
#define SW_ISR_BUF       2048     // software UART: edge timestamps (4 bytes each)
#define TX_QUEUE         4096     // per port: bytes waiting for the serial line (input is never blocking)
#define RING_SIZE        16384    // replay buffer port 1 (power of two)
#define RING_SIZE_EXTRA  8192     // replay buffer ports 2-4

// ---- File transfer (XMODEM / YMODEM) ----------------------------------------------
#define XFER_BUF         16384    // file data buffered ahead of the serial line


// ---- Diagnostics on the USB serial console -------------------------------------
#ifndef DEBUG_NET_LOG
#define DEBUG_NET_LOG    0        // 1 = also log every DNS query (captive portal)
#endif
