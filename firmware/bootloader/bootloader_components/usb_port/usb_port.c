// First thing the second-stage bootloader does, some tens of milliseconds
// after power-on: take the USB port away from the chip's serial/JTAG device.
//
// After power-on the chip puts that device on the port by itself, and the
// application only gets to change it half a second later (measured: a host had
// read all its descriptors and bound a serial driver by then). A Cisco switch
// crashes on exactly that device (WAVESHARE.md). A host waits at least 100 ms
// after it sees a device before it talks to it, so being gone by then is being
// never seen.
//
// "Away" is what Sd::usbStart() in the application does as well: the port goes
// to the USB controller, which nobody has started - no pull-up on D+. Only a
// port that nobody has chosen yet is touched (the bit is clear after power-on
// and after a reset of the RTC domain): the application's choice - dead port,
// stick, or the debug console when settings.usbDebug asks for it - is kept in
// these same bits and survives every other restart.
//
// The ROM's download mode (BOOT held at power-on) never gets here: flashing
// over the cable keeps working.
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

void bootloader_before_init(void) {
  if (!REG_GET_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL))
    REG_SET_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL);
}

// The hooks are weak references in the bootloader: without a symbol the linker
// is told to look for (CMakeLists.txt) it would leave this file out.
void bootloader_hooks_include(void) {}
