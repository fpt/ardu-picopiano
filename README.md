# ardu-picopiano

A C-major scale demo for **Seeed XIAO RP2350** (RP2350) with audio output,
I2C OLED display, and optional capacitive-key LEDs via Pimoroni Piano HAT.

## Hardware

| Component | Purpose |
|---|---|
| Seeed XIAO RP2350 | Main board |
| Adafruit MAX98357A breakout | I2S mono amplifier + speaker |
| SSD1306 128×64 I2C OLED | 0.96" monochrome display |
| Pimoroni Piano HAT | 13-key capacitive touch + per-key LEDs (optional) |

## Wiring

### I2C bus (shared: OLED + Piano HAT)

| Device | Address | XIAO pins |
|---|---|---|
| SSD1306 OLED | `0x3C` | D4 (SDA), D5 (SCL) |
| Piano HAT U1 | `0x28` | D4 (SDA), D5 (SCL) |
| Piano HAT U2 | `0x2B` | D4 (SDA), D5 (SCL) |

### SSD1306 OLED

| OLED | XIAO |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | D4 (GP6) |
| SCL | D5 (GP7) |

### MAX98357A amplifier

| MAX98357A | XIAO |
|---|---|
| VIN | 3V3 |
| GND | GND |
| BCLK | D0 (GP26) |
| LRC | D1 (GP27) *(auto = BCLK+1)* |
| DIN | D2 (GP28) |

### Pimoroni Piano HAT (optional)

Connect six wires from the Pi HAT header to the XIAO:

| Pi HAT pin | Signal | XIAO GPIO | XIAO pin |
|:---:|---|:---:|:---:|
| 1 | 3V3 | 3V3 | 3V3 |
| 6 | GND | GND | GND |
| 3 | I2C SDA | GP6 | D4 |
| 5 | I2C SCL | GP7 | D5 |
| 7 | ALERT U1 | GP29 | D3 |
| 13 | ALERT U2 | GP0 | D6 |

RST pads (soldered directly on HAT PCB, not via Pi header):

| CAP1188 pad | Signal | XIAO GPIO | XIAO pin |
|:---:|---|:---:|:---:|
| U1 RST | Reset U1 | GP1 | D7 |
| U2 RST | Reset U2 | GP2 | D8 |

> ALERT pins are configured as `INPUT_PULLUP`. Interrupt-driven detection is not yet implemented.
> RST pins are pulsed low on every boot to guarantee clean chip state.
> The HAT runs on 3.3 V; no level shifter needed.

Full wiring reference: see [WIRING.md](WIRING.md).

## Features

- Plays C-major scale (C4–C5) ascending and descending, looping
- OLED shows current note name and frequency
- OLED bottom strip shows which Piano HAT key is being touched (polled every 20 ms during rests)
- Piano HAT key LED lights while its note plays
- Piano HAT is optional — program runs normally without it; OLED shows detection status on boot

## OLED layout

```
┌────────────────────────────┐
│                            │
│            A4              │  ← note name  (size 3, centred)
│                            │
│        440.00 Hz           │  ← frequency  (size 2, centred)
│                            │
│         key: G#            │  ← touch strip (size 1, bottom)
└────────────────────────────┘
```

## Software setup

Requires [arduino-cli](https://arduino.github.io/arduino-cli/installation/).

```bash
# One-time setup
make install-core   # installs earlephilhower/arduino-pico board package
make install-libs   # installs Adafruit GFX + SSD1306 libraries

# Build and deploy
make                # compile only
make upload         # compile + upload (auto-detects port)
make upload PORT=/dev/cu.usbmodem14201   # macOS — explicit port
make upload PORT=/dev/ttyACM0            # Linux — explicit port
make monitor        # open serial monitor at 115200 baud
make clean          # remove build artefacts
```

Board FQBN: `rp2040:rp2040:seeed_xiao_rp2350`

## Serial output

```
Piano HAT detected          ← or "Piano HAT not found — U1:OK U2:MISS"
Ready — playing C-major scale
Playing C4
Playing D4
...
```

## Tuning

| Constant | Location | Default | Effect |
|---|---|---|---|
| `AMPLITUDE` | top of sketch | `4000` | Volume (0–32767) |
| `SAMPLE_RATE` | top of sketch | `22050` | Audio sample rate (Hz) |
| Note duration | `playNote` in `loop()` | `400` ms | Length of each note |
| Rest duration | `playNote` in `loop()` | `80` ms | Gap between notes |

## Dependencies

| Library | Install |
|---|---|
| earlephilhower/arduino-pico | `make install-core` |
| Adafruit GFX Library | `make install-libs` |
| Adafruit SSD1306 | `make install-libs` |
| Wire, I2S, math | built into board package |
