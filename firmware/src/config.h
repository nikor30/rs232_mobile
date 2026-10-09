#pragma once
// ============================================================================
//  RS232 Web Console - hardware + default configuration
//
//  Board: Waveshare ESP32-S3-Touch-LCD-2
// ============================================================================

#define FW_NAME     "RS232 Web Console"
#define FW_VERSION  "1.8.0"

// --------------------------------------- Waveshare ESP32-S3-Touch-LCD-2 (2", 240x320)
// ESP32-S3R8, 16 MB flash, ST7789T3 on SPI, CST816D touch, USB-C on the native
// USB port, QMI8658 accelerometer, SD slot, LiPo connector. Colour touch
// interface (lcd_ui.cpp) and a Bluetooth LE console.
#define BOARD_NAME      "Waveshare ESP32-S3-Touch-LCD-2"
#define PIN_LCD_SCLK    39  // display pins as in the LovyanGFX board configuration
#define PIN_LCD_MOSI    38
#define PIN_LCD_MISO    40
#define PIN_LCD_DC      42
#define PIN_LCD_CS      45
#define PIN_LCD_BL       1  // backlight, active high
// The panel takes 40 MHz, but the SD card hears the same clock and is specified
// up to 25 MHz. A card that stopped answering until its next power cycle is the
// open problem here (WAVESHARE.md); staying inside its limit costs nothing now
// that only changed parts of the picture are sent.
#define LCD_SPI_HZ      20000000
#define PIN_SD_CS       41  // SD slot on the LCD's SPI lines
#define PIN_SD_MOSI     PIN_LCD_MOSI
#define PIN_SD_SCLK     PIN_LCD_SCLK
#define PIN_SD_MISO     PIN_LCD_MISO
// Display rotation (0..3) for "this accelerometer axis points up"; the opposite
// direction is the rotation plus two. DEFAULT is used while the board lies flat.
#define IMU_ROT_DEFAULT  3
#define IMU_ROT_X_POS    0
#define IMU_ROT_Y_POS    1
#define PIN_KEY          0  // BOOT button (usable after start-up)
#define PIN_I2C_SDA     48  // touch (CST816D, 0x15) + accelerometer (QMI8658)
#define PIN_I2C_SCL     47

// Serial is the USB-C port, so the UART pad RXD (IO44) is free for port 1. TXD
// (IO43) is not used: it is U0TXD, and the chip's ROM prints its boot messages
// there on every reset - straight into the console of the attached device.
// Port 1 sends on IO21 instead (header pin 22): no power-up glitch, and the
// board's 4.7k pull-up keeps the line idle while the chip is in reset. IO44 can
// send as well (RX/TX swap); it stays an input during boot.
// Further header pins are left out until they are tried on the board.
#define PORT_DEFAULT_PINS {{44, 21}, {44, 21}, {44, 21}, {44, 21}}
#define PINS_RX  {21, 44}
#define PINS_TX  {21, 44}
#define PINS_ADC {5}              // battery voltage divider of the LiPo connector
#define PIN_NAMES "44:RXD,5:BAT"
#define PIN_PREFIX "IO"

#define BAT_PIN_DEFAULT   5       // BAT_ADC: divider on the board, measured against a LiPo (WAVESHARE.md)
#define BAT_TYPE_DEFAULT  0       // 0 = LiPo 1S, 1 = NiCd/NiMH 4 cells
#define BAT_DIV_DEFAULT  30       // 200k/100k
// Charger: ETA6096 on the LiPo connector, no status line to the processor (power.cpp).
#define CPU_MHZ_BATTERY  80       // clock while running from the battery (power.cpp: saver())
#define BAT_OFF_MV     3300       // on battery below this for BAT_OFF_S: switch off before the cell is drained
#define BAT_OFF_S        30
#ifndef FIXED_AP_PASS
#define FIXED_AP_PASS  "rs232mockup"
#endif
#define CPU_MHZ          240
#define TX_POWER_DEFAULT 34       // WIFI_POWER_8_5dBm: plenty for a few metres

// ---- Battery measurement ----------------------------------------------------
#define BAT_CAL          1.00f   // fine tune: real_voltage / displayed_voltage
#define BAT_LOW_PCT      10      // warning (display + web UI)

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
#define DEBUG_NET_LOG    1        // 1 = also log every DNS query (captive portal)
#endif
