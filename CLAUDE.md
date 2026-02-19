# CLAUDE.md — ardu-picopiano project notes

## Project overview
Arduino sketch for Seeed XIAO RP2350 (RP2350) that plays a C-major scale,
displays the current note on a SSD1306 I2C OLED, and optionally lights Piano HAT keys.
Board package: **earlephilhower/arduino-pico**, FQBN `rp2040:rp2040:seeed_xiao_rp2350`.

## Build & deploy
```bash
make install-core   # one-time: installs rp2040 board package
make install-libs   # one-time: installs Adafruit GFX + SSD1306 libraries
make                # compile
make upload         # compile + upload (auto-detects port)
make upload PORT=/dev/cu.usbmodem14201  # explicit port
make monitor        # serial monitor at 115200 baud
```

## Hardware
| Peripheral | Interface | Key pins |
|---|---|---|
| MAX98357A amplifier | I2S (PIO) | GP26=BCLK, GP27=LRCLK(auto), GP28=DIN |
| SSD1306 128×64 OLED | I2C1 (Wire1) | GP6=SDA, GP7=SCL, addr 0x3C |
| Pimoroni Piano HAT | I2C1 (Wire1, shared) | GP6=SDA, GP7=SCL |

Piano HAT is optional. ALERT pins are not yet wired.

## I2C architecture
OLED and Piano HAT share **Wire1** (I2C1) on GP6/GP7.
- SSD1306: `0x3C`
- CAP1188 U1: `0x28`
- CAP1188 U2: `0x2B`

`i2cInit()` does bus recovery + `Wire1.begin()` — call it before `oledInit()` and `hatInit()`.
`oledInit()` and `hatInit()` both assume Wire1 is already up; neither calls Wire1.begin() themselves.

`i2cInit()` / `hatInit()` recovery order:
1. `i2cInit()`: RESET_U1/U2 set OUTPUT HIGH (idle); ALERT pins INPUT_PULLUP
2. `i2cInit()`: SCL bit-bang 9 pulses + STOP — frees any stuck device (e.g. OLED mid-xfer)
3. `i2cInit()`: Wire1.begin()
4. `hatInit()`: pulse RESET_U1/U2 LOW for 1 ms, then HIGH — CAP1188 come out of reset onto a live Wire1 bus; wait 10 ms for internal init

## SSD1306 OLED API (Adafruit_SSD1306)
- Constructor: `Adafruit_SSD1306 oled(128, 64, &Wire1, -1)` — pass `&Wire1`, reset=-1
- Init: `oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)`
- All drawing goes to a RAM buffer; call `oled.display()` to flush to the panel
- Monochrome only: use `SSD1306_WHITE` / `SSD1306_BLACK`
- Text sizes: size N → 6N px wide, 8N px tall per character
- Since the whole buffer is sent on each `display()` call, track `_currentNote` and
  `_lastTouched` globally so `oledRedraw()` can repaint both zones from either update path

## OLED layout (128×64)
```
y=  4  note name  (size 3, 24 px tall)
y= 32  frequency  (size 2, 16 px tall)
y= 56  touch key  (size 1,  8 px tall)
```

## Known gotchas

### Arduino preprocessor ordering
The Arduino preprocessor inserts forward declarations for all sketch functions
near the top of the file, **before any user code**. If a `struct` is defined
between two functions, the generated forward declaration for a function that
takes that struct will appear before the struct definition → compile error.

**Fix:** always define structs and global constants at the very top of the
sketch, before any function definitions.

### I2S API (arduino-pico v4.5.2)
- `begin()` takes **only sample rate**: `i2s.begin(sampleRate)`
- Bit depth is set separately: `i2s.setBitsPerSample(16)`
- Stereo write: `i2s.write16(left, right)`
- BCLK pin → set with `setBCLK(pin)`; LRCLK is **auto-assigned to BCLK+1**
- Data pin → set with `setDATA(pin)`

### Audio quality — use a phase accumulator
Computing `sinf(2π × freq × i / rate)` for large `i` loses float precision
and introduces audible noise. Instead, accumulate a small phase increment:
```cpp
float phase = 0, phaseInc = 2*PI*freq/SAMPLE_RATE;
// per sample:
sample = sinf(phase);
phase += phaseInc;
if (phase >= 2*PI) phase -= 2*PI;
```

### I2C bus recovery after Pico reset
Pressing reset (not power-cycle) can leave CAP1188 slaves mid-transaction,
holding SDA low.

**Primary fix:** pulse RESET_U1/RESET_U2 LOW in `i2cInit()` — hardware reset
forces the chips to release the bus immediately.

**Fallback:** bit-bang up to 9 SCL pulses in GPIO mode (handles any other
stuck device such as the OLED). Done in `i2cInit()` after the RST pulse:
```cpp
pinMode(scl, OUTPUT); pinMode(sda, INPUT_PULLUP);
for (int i = 0; i < 9; i++) {
    if (digitalRead(sda) == HIGH) break;
    digitalWrite(scl, LOW);  delayMicroseconds(5);
    digitalWrite(scl, HIGH); delayMicroseconds(5);
}
// then send STOP: SDA rises while SCL HIGH
```

### CAP1188 LED direct control
Required register sequence for software-controlled LEDs:
```
R_LED_BEHAVIOUR_1 (0x81) = 0x00  // direct mode (not pulse/breathe)
R_LED_BEHAVIOUR_2 (0x82) = 0x00
R_LED_LINKING     (0x72) = 0x00  // unlink from touch inputs
R_LED_DIRECT_RAMP (0x94) = 0x00  // immediate change (no ramp delay)
R_LED_OUTPUT_CON  (0x74) = bitmask  // bit n = LED n on/off
```
Since only one note plays at a time, write the full bitmask directly to
`R_LED_OUTPUT_CON` — no read-modify-write needed.

### I2C read without repeated start
Some read operations that used `Wire.endTransmission(false)` (repeated start)
can fail silently on RP2040. Prefer a plain STOP + new START instead:
```cpp
Wire1.beginTransmission(addr); Wire1.write(reg); Wire1.endTransmission();
Wire1.requestFrom(addr, (uint8_t)1);
```

### hatInit() must run after oledInit()
`hatInit()` draws a status splash on the OLED. Call `oledInit()` first in
`setup()`, then `hatInit()`.

### CAP1188 touch polling
Touch input status is in `R_INPUT_STATUS` (0x03) — one bit per key.
After reading, clear the INT flag: `capWrite(addr, R_MAIN_CONTROL, 0x00)`.
Polling is done inside `playRest()` every ~20 ms (SAMPLE_RATE/50 samples).
`playTone()` is not polled to avoid I2S timing disruption.

### ST7735S colour byte-swap (historical — Waveshare LCD, no longer used)
The Waveshare ST7735S panel expected RGB565 bytes in swapped order.
This is no longer relevant with SSD1306, which is monochrome.
