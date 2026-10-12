#pragma once
// The language of this firmware. It is chosen when the firmware is built - one
// firmware per language (platformio.ini: -DUI_LANG=LANG_DE and so on) - and is
// English unless the build says otherwise.
//
// A text is written where it is used, in all three languages:
//
//     TR("Switch off", "Ausschalten", "Desligar")
//
// and only the one for this build ends up in the flash. Texts for the touch
// display are plain ASCII (its fonts have nothing else): "ae" for an umlaut, no
// accents. Texts for the web interface and the server are UTF-8.
//
// Not translated: the log on the USB serial port ([C2], [WLAN], [DIAG] ...).
// It is for whoever develops or debugs the device, and tools read it.
#define LANG_EN 0
#define LANG_DE 1
#define LANG_PT 2

#ifndef UI_LANG
#define UI_LANG LANG_EN
#endif

#if UI_LANG == LANG_DE
#define TR(en, de, pt) de
#define LANG_CODE "de"
#elif UI_LANG == LANG_PT
#define TR(en, de, pt) pt
#define LANG_CODE "pt"
#else
#define TR(en, de, pt) en
#define LANG_CODE "en"
#endif
