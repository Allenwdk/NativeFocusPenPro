# NativeFocusPenPro

Native bridge for the Xiaomi Focus Pen Pro (P81c, Bluetooth PnP 0022:5081). The current board integration targets Xiaomi Pad 7 (uke) and iPadOS 27.0.1 (24A446).

The driver combines pen coordinates from the panel with BLE pressure and gestures. It contains the HID report consumer, stroke state, kernel queue interface, signed runtime sources, magnetic attachment bridge and optional AccessorySetupKit pairing app. Existing service, source and exported symbol names retain their Uke ABI.

The board provides the exclusive panel scan and SPI/IRQ handling, HID device attachment to the display, and the PMIC Hall, address and charging snapshot. UniXNU-uke supplies the build and signing recipes, firmware/calibration inputs and Pencil-specific Apple system compatibility patches for its OS build. Its hover build consumes the separately supplied open-source Project-SunUEFI piano decoder and applies the Uke panel adaptations before compiling. This repository contains source code. Firmware, SDKs, Apple's system binaries, calibration files and owner trust material are supplied by the board build.

## Host checks

Run from this repository with a C11 and C++17 compiler. The same commands work with Clang.

```sh
cc -std=c11 -Wall -Wextra -Werror Test/uke_pen_radio_host.c UkePenRadioProtocol.c -o /tmp/native-focus-pen-radio
/tmp/native-focus-pen-radio
c++ -std=c++17 -Wall -Wextra -Werror Test/uke_pen_stroke_host.cpp -o /tmp/native-focus-pen-stroke
/tmp/native-focus-pen-stroke
```

`Test/uke_pen_compact_replay.c` uses saved NT36532 panel captures and the prepared Project-SunUEFI piano frame decoder from the Uke hover build.
