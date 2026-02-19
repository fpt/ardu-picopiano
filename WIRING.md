# ardu-picopiano — Wiring Reference

Seeed XIAO RP2350 with:
- Pimoroni Piano HAT (capacitive keys)
- MAX98357A I2S amplifier
- SSD1306 128×64 I2C OLED

---

## I2C bus (shared: OLED + Piano HAT)

Both the SSD1306 OLED (address `0x3C`) and the two CAP1188 touch ICs on the
Piano HAT (`0x28`, `0x2B`) share a single I2C bus on **Wire1** (I2C1).

| Device         | I2C address |
|----------------|-------------|
| SSD1306 OLED   | `0x3C`      |
| Piano HAT U1   | `0x28`      |
| Piano HAT U2   | `0x2B`      |

---

## SSD1306 OLED → XIAO RP2350

Typical 4-pin 128×64 I2C OLED module.

| OLED pin | Signal      | XIAO pin | GPIO  |
|----------|-------------|----------|-------|
| VCC      | 3.3 V       | 3V3      | —     |
| GND      | Ground      | GND      | —     |
| SDA      | I2C data    | D4       | GP6   |
| SCL      | I2C clock   | D5       | GP7   |

---

## Piano HAT internals

Piano HAT carries **two CAP1188** 8-channel capacitive touch ICs on a single
I2C bus.  Each chip asserts an active-low **ALERT** line when a key changes
state.

| CAP1188 | I2C address | Keys                                              | ALERT on RPi GPIO |
|---------|-------------|---------------------------------------------------|-------------------|
| U1      | `0x28`      | C, C#, D, D#, E, F, F#, G                        | GPIO 4  (pin 7)   |
| U2      | `0x2B`      | G#, A, A#, B, C2, Instrument, Octave−, Octave+   | GPIO 27 (pin 13)  |

---

## Piano HAT → XIAO RP2350

Connect six wires from the Pi HAT header to the XIAO.

> **No level shifter needed** — the CAP1188 operates at 1.8 V–3.6 V;
> both the RPi 3V3 rail and the XIAO 3V3 rail are 3.3 V.

| Pi HAT header pin | Pi GPIO  | Signal          | XIAO GPIO | XIAO pin |
|:-----------------:|----------|-----------------|:---------:|:--------:|
| 1                 | 3V3      | Power 3.3 V     | 3V3 (out) | 3V3      |
| 6                 | GND      | Ground          | GND       | GND      |
| 3                 | GPIO 2   | I2C SDA         | **GP6**   | D4       |
| 5                 | GPIO 3   | I2C SCL         | **GP7**   | D5       |
| 7                 | GPIO 4   | ALERT U1 (0x28) | **GP29**  | D3       |
| 13                | GPIO 27  | ALERT U2 (0x2B) | **GP0**   | D6       |

ALERT pins are configured as `INPUT_PULLUP`. Interrupt-driven touch detection is not yet implemented; polling via I2C is used instead.

The RST pins are connected directly to the CAP1188 RST pads on the HAT PCB (not via the Pi header):

| CAP1188 pad | Signal        | XIAO GPIO | XIAO pin |
|:-----------:|---------------|:---------:|:--------:|
| U1 RST      | Reset U1      | **GP1**   | D7       |
| U2 RST      | Reset U2      | **GP2**   | D8       |

---

## MAX98357A I2S amplifier → XIAO RP2350

| MAX98357A | Signal     | XIAO GPIO | XIAO pin |
|-----------|------------|:---------:|:--------:|
| VIN       | 3.3 V      | 3V3 (out) | 3V3      |
| GND       | Ground     | GND       | GND      |
| BCLK      | I2S clock  | **GP26**  | D0       |
| LRC       | I2S L/R    | **GP27**  | D1  *(auto = BCLK+1)* |
| DIN       | I2S data   | **GP28**  | D2       |

---

## Full XIAO RP2350 GPIO map

| XIAO pin | GPIO  | Role                        | Peripheral        |
|:--------:|:-----:|-----------------------------|-------------------|
| D0       | GP26  | I2S BCLK                    | MAX98357A         |
| D1       | GP27  | I2S LRCLK                   | MAX98357A         |
| D2       | GP28  | I2S DIN                     | MAX98357A         |
| D3       | GP29  | ALERT input U1 (active-low) | Piano HAT U1      |
| D4       | GP6   | I2C1 SDA (Wire1)            | OLED + Piano HAT  |
| D5       | GP7   | I2C1 SCL (Wire1)            | OLED + Piano HAT  |
| D6       | GP0   | ALERT input U2 (active-low) | Piano HAT U2      |
| D7       | GP1   | RST output U1 (active-low)  | Piano HAT U1      |
| D8       | GP2   | RST output U2 (active-low)  | Piano HAT U2      |
| D9       | GP4   | —                           | free              |
| D10      | GP3   | —                           | free              |
| 3V3      | —     | 3.3 V power                 | OLED, AMP, HAT    |
| GND      | —     | Ground                      | all               |

---

## XIAO RP2350 pinout

```
        ┌───────────────────────────────┐
        │         Seeed XIAO RP2350      │
   D0 ──┤  1                        14  ├── 3V3
   D1 ──┤  2                        13  ├── GND
   D2 ──┤  3                        12  ├── RST
   D3 ──┤  4                        11  ├── 5V (USB)
   D4 ──┤  5  SDA                   10  ├── D10
   D5 ──┤  6  SCL                    9  ├── D9
   D6 ──┤  7                         8  ├── D8 SCK
        └───────────────────────────────┘
```

---

## Pi HAT header pinout (for reference)

```
                         USB
                    ┌────┴────┐
    3V3  →  pin  1  │ 1     2 │  5V
    SDA  →  pin  3  │ 3     4 │  5V
    SCL  →  pin  5  │ 5     6 │  GND  ← GND
  ALERT1 → pin  7  │ 7     8 │  ...
             ...    │ ...  .. │
  ALERT2 → pin 13  │13    14 │  GND
             ...    │ ...     │
                    └─────────┘
```
