# esp-trackpoint

Proof of concept: drive an IBM/Lenovo **TrackPoint module** (Philips
**PTPM754** controller — a PS/2 mouse) from an **ESP32-C6** and print decoded
pointer movement + button state to the USB serial console.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-ESP32--C6-orange.svg)
![Framework](https://img.shields.io/badge/framework-ESP--IDF%20%2F%20PlatformIO-green.svg)

**Status: Phase 1 — pad identification** (see [`PLAN.md`](PLAN.md)). The
module's connector pinout is undocumented, so `src/main.c` is a *scanner* that
identifies the PS/2 pads (GND / VCC / CLK / DATA / RST) using only the ESP32 —
no multimeter, logic analyzer or scope.

## Hardware

- **Board:** ESP32-C6-DevKitC-1 (3.3 V logic, native USB-Serial-JTAG).
- **Module:** ThinkPad-era TrackPoint, **PTPM754DB** controller — speaks
  standard PS/2 mouse protocol.

### Hardware notes (read before wiring)

- **Run it at 3.3 V, not 5 V.** The PTPM754 is built on the Philips P87LPC7xx
  (8051) family, rated 2.7–6 V, so 3.3 V is in spec. At 3.3 V the open-collector
  PS/2 lines idle at 3.3 V, so CLK/DATA connect **directly to the C6's GPIO with
  no level shifter** (the C6 is *not* 5 V tolerant — 5 V would need a BSS138).
- **PS/2 is open-collector → pull-ups required.** 4.7k–10k to 3V3 on CLK and
  DATA. Internal ~45k pulls may work on a short wire; external is safer.
- **Reset is active-HIGH** (8051 core): pulse RST HIGH ~10 ms then LOW.
- **C6 GPIOs to avoid:** 8/9/15 (strapping), 12/13 (USB D-/D+), 24–30
  (in-package SPI flash). Keep PS/2 wires < ~10 cm.
- **Console over UART0, not USB-JTAG.** This board's USB is a WCH **CH343
  USB-UART bridge** (`1a86:55d3`, shows as `/dev/ttyACM0`) wired to UART0 — *not*
  the C6 native USB-Serial-JTAG. The console is therefore set to UART0
  (`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`); routing it to USB-JTAG makes `printf`
  disappear (esptool still flashes, because flashing uses UART0 download).

## Build / flash / monitor

```sh
pio run                 # build
pio run -t upload       # flash over native USB
pio device monitor      # serial console (USB-Serial-JTAG)
```

> First build downloads the **pioarduino** platform + C6 toolchain (sizeable) —
> the official `espressif32` platform does not build for the C6. See
> `platformio.ini`.
