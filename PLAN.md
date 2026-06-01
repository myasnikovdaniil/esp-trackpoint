# esp-trackpoint — plan

Get a ThinkPad-era IBM/Lenovo TrackPoint module talking to an **ESP32-C6**, as a
proof of concept that prints decoded pointer movement and button state to the
serial console over USB.

> **STATUS: WORKING.** Pads traced with a multimeter (PTPM754 pinout). Final
> wiring CLK=GPIO1, DATA=GPIO7, RST=GPIO5, VCC=3V3, GND=GND+shield. `src/main.c`
> resets the module, sends 0xFF/0xF4, and streams decoded `dx/dy/L/M/R`.
> Console is UART0 via the board's CH343 bridge (`1a86:55d3`), not USB-JTAG.

## What the hardware is (verified)

- The controller chip is a **Philips/NXP PTPM754DB** — a dedicated *TrackPoint
  Pointing Module* MCU built on the Philips P87LPC7xx (8051-core) family.
- It **speaks standard PS/2 mouse protocol** (CLK + DATA, open-collector,
  bidirectional). So this is "read a PS/2 mouse", not an analog strain-gauge
  reverse-engineering job.
- The P87LPC7xx family is rated **2.7–6.0 V**, so the module **runs fine at
  3.3 V**. The ZMK PS/2 trackpoint driver runs these modules straight off 3.3 V
  nRF52/ESP32 boards with **CLK/DATA wired directly to GPIO, no level shifter**.
  This is the key result: at 3.3 V the open-collector lines idle at 3.3 V, which
  is safe for the C6's (non-5V-tolerant) GPIOs.

### Chip pin anchors (for sanity-checking, not strictly needed)
Cross-referenced from community reverse-engineering. Consistent across sources:
**DATA = chip pin 2, RST = chip pin 5, GND = chip pin 8**. CLK/VCC vary by
package (CLK = 24 or 28, VCC = 22 or 26); buttons = 29–31. We identify pads
empirically instead of trusting these.

## Hardware decisions

- **Power at 3.3 V**, direct GPIO connection, **no level shifter**.
  (Fallback only if 3.3 V proves flaky: 5 V + BSS138 bidirectional shifter.)
- **External pull-ups 4.7k–10k** on CLK and DATA to 3.3 V (PS/2 is
  open-collector). The ESP32 internal ~45k pulls may work on a short wire, but
  external is the safe call.
- **Reset**: drive RST from a GPIO (8051 reset is active-HIGH: pulse HIGH ~10ms
  then LOW). Lets firmware re-init if PS/2 desyncs. (Alt: on-board RC POR =
  2.2µF VCC→RST + 100k RST→GND.)
- **GPIO choice on the C6**: avoid **GPIO8/9/15** (strapping), **GPIO12/13**
  (USB D-/D+), **GPIO24–30** (in-package SPI flash). Keep PS/2 wires < ~10 cm.

### Final wiring (after pads are identified)
```
ESP32-C6              TrackPoint module
  3V3      ────────►  VCC
  GND      ────────►  GND  (+ metal shield)
  GPIO6    ◄───────   CLK   + 4.7k pull-up to 3V3
  GPIO7    ◄──────►   DATA  + 4.7k pull-up to 3V3
  GPIO10   ────────►  RST
```

## Phases

### Phase 1 — Identify the 5 pads  ← we are here
The pad pinout is module-specific and undocumented for this board, so it must be
found. No multimeter/scope on hand → the **ESP32 does it** via `src/main.c`
(the pad scanner).

1. Solder a wire from each connector pad to a scanner GPIO (`CAND[]` in
   `main.c`: GPIO 0,1,2,3,6,7,10,11). Note pad order → P0..P7.
2. ESP GND → module metal shield.
3. Flash + monitor. Read the `idle:` line — pads stuck **L** = GND (shield).
4. Feed **3V3 through a ~100Ω resistor** into one pad at a time (best first
   guess = widest trace after GND). Watch for a `>>> HIT` line: it names
   **RST / CLK / DATA**; VCC = the pad you're feeding.
5. Record the mapping.

### Phase 2 — Permanent wiring
Re-wire to the final layout above with the 4.7k pull-ups and RST on a GPIO.

### Phase 3 — PS/2 reader firmware
Bit-banged PS/2 host. Note: after reset the mouse is in stream mode but
**reporting is disabled** — must send `0xF4` (Enable Data Reporting) to get
motion, so the host needs bidirectional comms (inhibit clock >100µs, send byte,
read). Flow:
1. Pulse RST → expect `0xAA 0x00` (BAT pass + ID).
2. (Optional) `0xFF` reset, `0xF3` set sample rate.
3. `0xF4` enable reporting.
4. CLK falling-edge ISR assembles 11-bit frames → decode the 3-byte packet
   (button bits + signed X/Y deltas).
5. `printf` decoded `dx, dy, L/M/R` to the USB console.

### Phase 4 — Verify
Move the nub → deltas stream in the terminal; press buttons (if broken out) →
flags toggle.

## Risks / pitfalls
- **Fine-pitch soldering** of the connector pads — the main practical hurdle.
- **Wrong pad mapping** — Phase 1's powered HIT detection de-risks this before
  committing the final wiring.
- **PS/2 timing** on a fast C6 — interrupt-driven framing is timing-sensitive;
  lean on a known-good approach and adapt.
- **GPIO contention during the scan** — the ~100Ω on the VCC feed limits current
  on a wrong guess; `SKIP_LOW_PADS` protects the GND pad from the reset pulse.

## References
- alonswartz/trackpoint — chip pinout, reset circuit, identification heuristics
- infused-kim/kb_zmk_ps2_mouse_trackpoint_driver — 3.3 V direct-GPIO precedent
- mango.vg/post/4 — reverse-engineering an old ThinkPad trackpoint
- geekhack #115912, deskthority #7678 — PTPM754 pinouts per module
