/**
 * ardu-picopiano — C-major scale demo for Seeed XIAO RP2350
 * Audio output via MAX98357A I2S amplifier
 * Display via SSD1306 128×64 I2C OLED
 * Key LEDs via Pimoroni Piano HAT (two CAP1188 over I2C; optional)
 *
 * I2S wiring:
 *   GP26 -> MAX98357A BCLK
 *   GP27 -> MAX98357A LRC   (auto = BCLK+1)
 *   GP28 -> MAX98357A DIN
 *
 * I2C wiring (Wire1 / I2C1, shared by OLED and Piano HAT):
 *   GP6  -> SDA
 *   GP7  -> SCL
 *
 * Piano HAT ALERT pins (active-low, INPUT_PULLUP):
 *   GP29 (D3) -> ALERT U1 (0x28, keys C-G)
 *   GP0  (D6) -> ALERT U2 (0x2B, keys G#-C2+)
 *
 * Board package: earlephilhower/arduino-pico
 * FQBN:          rp2040:rp2040:seeed_xiao_rp2350
 */

#include <Wire.h>
#include <I2S.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>
#include <pico/mutex.h>

// ---------------------------------------------------------------------------
// OLED
// ---------------------------------------------------------------------------
#define OLED_WIDTH   128
#define OLED_HEIGHT   64
#define OLED_ADDR   0x3C

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire1, -1);

// ---------------------------------------------------------------------------
// I2S
// ---------------------------------------------------------------------------
I2S       i2s(OUTPUT);
const int SAMPLE_RATE   = 22050;
const int BIT_DEPTH     = 16;
const int AMPLITUDE_MAX = 32700;

// ---------------------------------------------------------------------------
// Full chromatic note table C4-C5
// key: Piano HAT index (matches pianohat.py: 0=C … 12=C2)
// ---------------------------------------------------------------------------
struct Note {
  const char *name;
  float       freq;
  uint8_t     key;
};

static const Note NOTES[] = {
  { "C4",  261.63f,  0 },
  { "C#4", 277.18f,  1 },
  { "D4",  293.66f,  2 },
  { "D#4", 311.13f,  3 },
  { "E4",  329.63f,  4 },
  { "F4",  349.23f,  5 },
  { "F#4", 369.99f,  6 },
  { "G4",  392.00f,  7 },
  { "G#4", 415.30f,  8 },
  { "A4",  440.00f,  9 },
  { "A#4", 466.16f, 10 },
  { "B4",  493.88f, 11 },
  { "C5",  523.25f, 12 },
};
static const int NOTES_LEN = (int)(sizeof(NOTES) / sizeof(NOTES[0]));

// ---------------------------------------------------------------------------
// Instrument modes
// ---------------------------------------------------------------------------
#define MODE_PIANO    0   // sine wave
#define MODE_CHIPTUNE 1   // square wave
#define MODE_VOLUME   2   // oct± buttons change volume instead of octave

#define VOICE_COUNT  4

// ADSR envelope parameters (seconds)
//   Piano: gentle attack, clear decay to warm sustain, medium release
static const float PIANO_ATK=0.006f, PIANO_DEC=0.35f, PIANO_SUS=0.55f, PIANO_REL=0.20f;
//   Chiptune: instant attack, punchy decay, medium sustain, short release
static const float CHIP_ATK =0.002f, CHIP_DEC =0.12f, CHIP_SUS =0.60f, CHIP_REL =0.06f;

// ---------------------------------------------------------------------------
// Voice — one polyphonic voice (instrument type frozen at note-on)
// ---------------------------------------------------------------------------
struct Voice {
  bool    active       = false;
  bool    releasing    = false;
  int8_t  keyIdx       = -1;
  int8_t  instrument   = MODE_PIANO;  // frozen at note-on; immune to mode switches mid-chord
  float   fc           = 0.0f;
  float   carrierInc   = 0.0f;
  float   carrierPhase = 0.0f;
  float   modInc       = 0.0f;
  float   modPhase     = 0.0f;
  float   modRange     = 0.0f;
  float   modDecay     = 1.0f;
  float   chipSmooth   = 0.0f;
  float   envLevel     = 0.0f;   // last computed env level (captured at release)
  float   releaseLevel = 0.0f;
  long    sampleAge    = 0;
  long    releaseAge   = 0;
};

// ---------------------------------------------------------------------------
// Piano HAT — two CAP1188 capacitive touch ICs
//
// Key index mapping (matches pianohat.py):
//   U1 (0x28): keys 0-7  → C, C#, D, D#, E, F, F#, G
//   U2 (0x2B): keys 8-15 → G#, A, A#, B, C2, Instr, Oct-, Oct+
// ---------------------------------------------------------------------------
#define CAP_U1    0x28
#define CAP_U2    0x2B
#define ALERT_U1  29    // GP29 = XIAO D3 — active-low
#define ALERT_U2   0    // GP0  = XIAO D6 — active-low

// CAP1188 registers
#define R_MAIN_CONTROL     0x00   // bit 0 = INT flag; write 0 to clear
#define R_INPUT_STATUS     0x03   // bit n = key n touched (read-only)
#define R_SENSITIVITY      0x1F   // bits [6:4] = DELTA_SENSE (lower = more sensitive)
#define R_CS1_THRESHOLD    0x30   // per-channel threshold CS1…CS8 at 0x30…0x37
#define R_LED_LINKING      0x72
#define R_LED_OUTPUT_CON   0x74   // bitmask: bit n = LED n on/off
#define R_LED_BEHAVIOUR_1  0x81   // 2 bits per LED: 00=direct, others=pulse/breathe
#define R_LED_BEHAVIOUR_2  0x82
#define R_LED_DIRECT_RAMP  0x94   // ramp rate for direct mode (0 = immediate)

// ---------------------------------------------------------------------------
// Touch sensitivity tuning
//
// DELTA_SENSE  (global, both chips):
//   0=128x  1=64x  2=32x(default)  3=16x  4=8x  5=4x  6=2x  7=1x
//   Lower number = more sensitive.  Raise to reduce cross-talk / false triggers.
//
// Per-channel THRESHOLD  (0x01–0x7F, default 0x40):
//   Higher = less sensitive (harder to trigger).
//   Lower  = more sensitive (easier to trigger).
//
//   U1 channels:  C   C#   D   D#   E    F   F#   G
//   U2 channels:  G#   A   A#   B   C2  Oct- Oct+  Inst
// ---------------------------------------------------------------------------
#define DELTA_SENSE  6    // 2x — less sensitive than default (reduces cross-talk)

static const uint8_t THRESHOLDS_U1[8] = {
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x50,  // C…F# normal; G raised
};
static const uint8_t THRESHOLDS_U2[8] = {
  0x40, 0x50, 0x40, 0x30, 0x50, 0x50, 0x50, 0x50,  // A raised; B lowered; Oct-/Oct+/Inst raised
};

// Key names indexed by Piano HAT key number (0-15)
static const char* const KEY_NAMES[16] = {
  "C", "C#", "D", "D#", "E", "F", "F#", "G",          // U1 (0x28) bits 0-7
  "G#", "A", "A#", "B", "C2", "Oct-", "Oct+", "Inst"   // U2 (0x2B) bits 0-7
};

static bool                 _hatPresent    = false;
static volatile const Note* _currentNote   = nullptr;
static volatile int8_t      _octaveShift   = 0;     // -2 to +2
static volatile int8_t      _instrument    = MODE_PIANO;
static volatile uint8_t     _volumePct     = 8;     // powers of 2: 1–64
static Voice                _voices[VOICE_COUNT];
static volatile bool        _oledDirty     = false; // Core 1 redraws when true
static recursive_mutex_t    _i2cMutex;              // guards Wire1 between both cores
static volatile bool        _coreSetupDone = false; // Core 1 waits until Core 0 setup done

// ---------------------------------------------------------------------------
// CAP1188 helpers  (all on Wire1)
// ---------------------------------------------------------------------------
static void capWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire1.beginTransmission(addr);
  Wire1.write(reg);
  Wire1.write(val);
  Wire1.endTransmission();
}

static bool capProbe(uint8_t addr) {
  Wire1.beginTransmission(addr);
  return Wire1.endTransmission() == 0;
}

// Apply global sensitivity and per-channel thresholds.
// SENSITIVITY_CONTROL (0x1F): bits [6:4] = DELTA_SENSE; bits [2:0] = BASE_SHIFT (keep 7).
static void capSetSensitivity(uint8_t addr, const uint8_t thresh[8]) {
  capWrite(addr, R_SENSITIVITY, (DELTA_SENSE << 4) | 0x0F);
  for (int i = 0; i < 8; i++)
    capWrite(addr, R_CS1_THRESHOLD + i, thresh[i]);
}

static void capInitLEDs(uint8_t addr) {
  capWrite(addr, R_LED_BEHAVIOUR_1, 0x00);  // all 8 LEDs → direct mode
  capWrite(addr, R_LED_BEHAVIOUR_2, 0x00);
  capWrite(addr, R_LED_LINKING,     0x00);  // unlink LEDs from touch inputs
  capWrite(addr, R_LED_DIRECT_RAMP, 0x00);  // no ramp — LEDs change immediately
  capWrite(addr, R_LED_OUTPUT_CON,  0x00);  // all LEDs off
}

// Read which keys are currently touched; clear INT flag so the status
// register continues to update on subsequent state changes (CAP1188 §6.4:
// status bits are frozen until INT is cleared).
static uint8_t capReadStatus(uint8_t addr) {
  Wire1.beginTransmission(addr);
  Wire1.write(R_INPUT_STATUS);
  Wire1.endTransmission();
  Wire1.requestFrom(addr, (uint8_t)1);
  uint8_t status = Wire1.available() ? Wire1.read() : 0;
  capWrite(addr, R_MAIN_CONTROL, 0x00);   // clear INT
  return status;
}

// ---------------------------------------------------------------------------
// I2C init  (shared bus: GP6=SDA, GP7=SCL, 400 kHz)
//
// Must run before oledInit() and hatInit().
// Includes bus-recovery: if a Pico reset left a CAP1188 mid-transaction
// (SDA held low), bit-banging 9 SCL pulses + STOP releases the bus.
// ---------------------------------------------------------------------------
void i2cInit() {
  const int sda = 6, scl = 7;
  pinMode(scl, OUTPUT);
  pinMode(sda, INPUT_PULLUP);
  digitalWrite(scl, HIGH);
  delayMicroseconds(10);
  for (int i = 0; i < 9; i++) {
    if (digitalRead(sda) == HIGH) break;   // bus already free
    digitalWrite(scl, LOW);  delayMicroseconds(5);
    digitalWrite(scl, HIGH); delayMicroseconds(5);
  }
  // STOP condition: SDA rises while SCL is HIGH
  pinMode(sda, OUTPUT);
  digitalWrite(sda, LOW);  delayMicroseconds(5);
  digitalWrite(scl, HIGH); delayMicroseconds(5);
  digitalWrite(sda, HIGH); delayMicroseconds(5);

  Wire1.setSDA(sda);
  Wire1.setSCL(scl);
  Wire1.begin();
  Wire1.setClock(2000000);
  delay(20);   // give I2C devices time to power up

  // ALERT inputs — active-low, pulled up internally
  pinMode(ALERT_U1, INPUT_PULLUP);
  pinMode(ALERT_U2, INPUT_PULLUP);
}

// ---------------------------------------------------------------------------
// OLED helpers
// ---------------------------------------------------------------------------

// Build the display RAM buffer from current state (no I2C write).
// OLED layout (128×64):
//   y=  4 : note name  (size 3, 24 px tall)
//   y= 32 : frequency  (size 2, 16 px tall)
//   y= 56 : mode strip (size 1,  8 px tall)
static void oledBuildBuffer() {
  // Snapshot volatile globals once (called from Core 1)
  const Note* note  = (const Note*)_currentNote;
  int8_t      oct   = _octaveShift;
  int8_t      inst  = _instrument;
  uint8_t     vol   = _volumePct;

  oled.clearDisplay();

  if (note) {
    // Note name with octave shift applied — size 3, centred
    // Name format is e.g. "C4", "C#4", "C5" — octave digit is always last char
    char name[8];
    strncpy(name, note->name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    int len = strlen(name);
    if (len > 0) name[len - 1] += oct;   // shift the octave digit

    oled.setTextSize(3);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor((OLED_WIDTH - len * 18) / 2, 4);
    oled.print(name);

    // Shifted frequency — size 2, centred
    float freq = note->freq * powf(2.0f, oct);
    char buf[12];
    snprintf(buf, sizeof(buf), "%.2f Hz", freq);
    oled.setTextSize(2);
    int freqWidth = strlen(buf) * 12;
    oled.setCursor((OLED_WIDTH - freqWidth) / 2, 32);
    oled.print(buf);
  }

  // Bottom strip — size 1, centred: mode / octave / volume info
  oled.setTextSize(1);
  char bot[16] = "";
  if (inst == MODE_VOLUME)
    snprintf(bot, sizeof(bot), "Vol: %d%%", (int)vol);
  else if (inst == MODE_CHIPTUNE && oct != 0)
    snprintf(bot, sizeof(bot), "Chip Oct%+d", oct);
  else if (inst == MODE_CHIPTUNE)
    snprintf(bot, sizeof(bot), "Chip");
  else if (oct != 0)
    snprintf(bot, sizeof(bot), "Oct %+d", oct);
  if (bot[0]) {
    oled.setCursor((OLED_WIDTH - (int)strlen(bot) * 6) / 2, 56);
    oled.print(bot);
  }
}

// Full blocking redraw — used during setup paths where audio is not running.
static void oledRedraw() {
  oledBuildBuffer();
  oled.display();
}


void oledInit() {
  // Wire1 already initialised by i2cInit()
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("WARNING: SSD1306 OLED not found");
  }
  oled.clearDisplay();
  oled.display();
}

void oledShowNote(const Note &note) {
  _currentNote = &note;
  oledRedraw();
}

// ---------------------------------------------------------------------------
// Piano HAT
// ---------------------------------------------------------------------------

// Probe both chips; initialise LEDs if found.  Draws a status splash.
// Must be called after oledInit().
void hatInit() {
  bool u1 = capProbe(CAP_U1);
  bool u2 = capProbe(CAP_U2);
  _hatPresent = u1 && u2;

  oled.clearDisplay();
  oled.setTextSize(2);
  if (_hatPresent) {
    capInitLEDs(CAP_U1);
    capInitLEDs(CAP_U2);
    capSetSensitivity(CAP_U1, THRESHOLDS_U1);
    capSetSensitivity(CAP_U2, THRESHOLDS_U2);
    // Startup blink: all LEDs on briefly to confirm they work
    capWrite(CAP_U1, R_LED_OUTPUT_CON, 0xFF);
    capWrite(CAP_U2, R_LED_OUTPUT_CON, 0xFF);
    int w = 6 * 2 * 6;   // "HAT OK" = 6 chars × 12px
    oled.setCursor((OLED_WIDTH - w) / 2, 24);
    oled.print("HAT OK");
    oled.display();
    Serial.println("Piano HAT detected");
    delay(100);
    capWrite(CAP_U1, R_LED_OUTPUT_CON, 0x00);
    capWrite(CAP_U2, R_LED_OUTPUT_CON, 0x00);
  } else {
    int w = 6 * 2 * 6;   // "No HAT" = 6 chars × 12px
    oled.setCursor((OLED_WIDTH - w) / 2, 16);
    oled.print("No HAT");
    oled.setTextSize(1);
    oled.setCursor(20, 38);
    oled.print("U1(0x28): ");
    oled.print(u1 ? "OK" : "miss");
    oled.setCursor(20, 50);
    oled.print("U2(0x2B): ");
    oled.print(u2 ? "OK" : "miss");
    oled.display();
    Serial.print("Piano HAT not found — U1:");
    Serial.print(u1 ? "OK" : "MISS");
    Serial.print(" U2:");
    Serial.println(u2 ? "OK" : "MISS");
  }
  delay(1500);
}

// Light the LED for one piano key; turn off all others.
// key: 0-7 on U1, 8-15 on U2  (matching pianohat.py index)
void hatSetLED(uint8_t key, bool on) {
  if (!_hatPresent) return;

  if (on) {
    uint8_t addr      = (key < 8) ? CAP_U1 : CAP_U2;
    uint8_t otherAddr = (key < 8) ? CAP_U2 : CAP_U1;
    capWrite(otherAddr, R_LED_OUTPUT_CON, 0x00);
    capWrite(addr,      R_LED_OUTPUT_CON, 1 << (key % 8));
  } else {
    capWrite(CAP_U1, R_LED_OUTPUT_CON, 0x00);
    capWrite(CAP_U2, R_LED_OUTPUT_CON, 0x00);
  }
}

// ---------------------------------------------------------------------------
// Audio helpers
// ---------------------------------------------------------------------------
static inline int amplitude() { return (int)_volumePct * (AMPLITUDE_MAX / 100); }

// DX7-style 2-operator FM piano:
//   carrier fc = note freq | modulator fm = fc × 14 (classic DX7 ratio, clamped to Nyquist)
//   mod index decays FM_I0 → FM_I1 over FM_TAU seconds (bright attack → warm sustain)
// Modulator frequency is clamped to 0.47 × sample rate to prevent aliasing at high octaves.
static const float FM_RATIO = 8.0f;
static const float FM_I0    = 1.0f;    // initial modulation index (bright, metallic attack)
static const float FM_I1    = 0.05f;   // sustain modulation index  (warm, mellow tone)
static const float FM_TAU   = 0.4f;    // mod index decay time constant (seconds)

// Compute per-sample FM state init values for a given carrier frequency.
// modDecay is multiplied into modRange every sample; avoids expf() in the hot loop.
static inline float fmModDecay()  { return expf(-1.0f / (FM_TAU * SAMPLE_RATE)); }
static inline float fmModInc(float fc) {
  float fm = fc * FM_RATIO;
  if (fm > SAMPLE_RATE * 0.47f) fm = SAMPLE_RATE * 0.47f;
  return 2.0f * (float)M_PI * fm / SAMPLE_RATE;
}

// ---------------------------------------------------------------------------
// Voice management and polyphonic mixer
// ---------------------------------------------------------------------------

// Compute one ADSR envelope sample; advance state counters.
// Uses v.instrument (frozen at note-on) so mode switches don't glitch held notes.
static float voiceEnv(Voice &v) {
  float atk_s, dec_s, sus, rel_s;
  if (v.instrument == MODE_CHIPTUNE) {
    atk_s=CHIP_ATK; dec_s=CHIP_DEC; sus=CHIP_SUS; rel_s=CHIP_REL;
  } else {
    atk_s=PIANO_ATK; dec_s=PIANO_DEC; sus=PIANO_SUS; rel_s=PIANO_REL;
  }
  long atk = (long)(atk_s * SAMPLE_RATE);
  long dec = (long)(dec_s * SAMPLE_RATE);
  long rel = (long)(rel_s * SAMPLE_RATE);

  float level;
  if (v.releasing) {
    long ra = v.releaseAge++;
    if (ra >= rel) { v.active = false; return 0.0f; }
    level = v.releaseLevel * (1.0f - (float)ra / rel);
  } else {
    long age = v.sampleAge++;
    if (age < atk)
      level = atk > 0 ? (float)(age + 1) / atk : 1.0f;
    else {
      age -= atk;
      level = (age < dec) ? 1.0f - (1.0f - sus) * (float)age / dec : sus;
    }
  }
  v.envLevel = level;
  return level;
}

// Compute one waveform sample scaled by envelope; advance all phases.
static float voiceSample(Voice &v) {
  float env = voiceEnv(v);
  if (!v.active) return 0.0f;

  float sampleF;
  if (v.instrument == MODE_CHIPTUNE) {
    float raw = (v.carrierPhase < (float)M_PI) ? 1.0f : -1.0f;
    v.chipSmooth += 0.2f * (raw - v.chipSmooth);
    sampleF = v.chipSmooth;
  } else {
    sampleF = sinf(v.carrierPhase + (FM_I1 + v.modRange) * sinf(v.modPhase));
    v.modRange *= v.modDecay;
    v.modPhase += v.modInc;
    if (v.modPhase >= 2.0f * (float)M_PI) v.modPhase -= 2.0f * (float)M_PI;
  }
  v.carrierPhase += v.carrierInc;
  if (v.carrierPhase >= 2.0f * (float)M_PI) v.carrierPhase -= 2.0f * (float)M_PI;
  return env * sampleF;
}

// Find a free voice slot (steal longest-running if all busy) and initialise it.
static void spawnVoice(int8_t keyIdx) {
  int vIdx = -1;
  for (int i = 0; i < VOICE_COUNT; i++)
    if (!_voices[i].active) { vIdx = i; break; }
  if (vIdx < 0) {
    long oldest = -1;
    for (int i = 0; i < VOICE_COUNT; i++)
      if (_voices[i].sampleAge > oldest) { oldest = _voices[i].sampleAge; vIdx = i; }
  }
  Voice &v       = _voices[vIdx];
  float  fc      = NOTES[keyIdx].freq * powf(2.0f, _octaveShift);
  v.active       = true;
  v.releasing    = false;
  v.keyIdx       = keyIdx;
  v.instrument   = _instrument;
  v.fc           = fc;
  v.carrierInc   = 2.0f * (float)M_PI * fc / SAMPLE_RATE;
  v.carrierPhase = 0.0f;
  v.modInc       = fmModInc(fc);
  v.modPhase     = 0.0f;
  v.modRange     = FM_I0 - FM_I1;
  v.modDecay     = fmModDecay();
  v.chipSmooth   = 0.0f;
  v.envLevel     = 0.0f;
  v.releaseLevel = 0.0f;
  v.sampleAge    = 0;
  v.releaseAge   = 0;
  _currentNote = &NOTES[keyIdx];
  _oledDirty   = true;   // defer OLED write until after the next audio batch
  Serial.print("Note on: "); Serial.println(NOTES[keyIdx].name);
}

// Begin release phase for a voice.
static void releaseVoice(int idx) {
  _voices[idx].releasing    = true;
  _voices[idx].releaseAge   = 0;
  _voices[idx].releaseLevel = _voices[idx].envLevel;
  Serial.print("Note off: "); Serial.println(NOTES[_voices[idx].keyIdx].name);
}

// Mix all active voices and write `count` samples to I2S.
// Per-voice amplitude; clamp to int16 range (clips only on loud chords at high volume).
static void mixAndWrite(int count) {
  float amp = (float)amplitude();
  for (int i = 0; i < count; i++) {
    float mix = 0.0f;
    for (int v = 0; v < VOICE_COUNT; v++)
      if (_voices[v].active) mix += voiceSample(_voices[v]);
    float s = mix * amp;
    if (s >  32767.0f) s =  32767.0f;
    if (s < -32767.0f) s = -32767.0f;
    i2s.write16((int16_t)s, (int16_t)s);
  }
}

// Update HAT LEDs: light every key with an active (non-releasing) voice.
static void updateLEDs() {
  if (!_hatPresent) return;
  uint8_t leds1 = 0, leds2 = 0;
  for (int i = 0; i < VOICE_COUNT; i++) {
    if (!_voices[i].active || _voices[i].releasing) continue;
    int k = _voices[i].keyIdx;
    if (k < 8) leds1 |= (1 << k);
    else       leds2 |= (1 << (k - 8));
  }
  capWrite(CAP_U1, R_LED_OUTPUT_CON, leds1);
  capWrite(CAP_U2, R_LED_OUTPUT_CON, leds2);
}

void playTone(float freq, int durationMs) {
  const long  total      = (long)SAMPLE_RATE * durationMs / 1000;
  const long  fadeLen    = SAMPLE_RATE / 100;
  float carrierInc       = 2.0f * (float)M_PI * freq / SAMPLE_RATE;
  float carrierPhase     = 0.0f;

  float modPhase = 0.0f, modRange = FM_I0 - FM_I1;
  float modDecay = fmModDecay(), modInc = fmModInc(freq);
  float chipSmooth = 0.0f;

  for (long i = 0; i < total; i++) {
    float env = 1.0f;
    if (i < fadeLen)
      env = (float)i / fadeLen;
    else if (i > total - fadeLen)
      env = (float)(total - i) / fadeLen;

    float sampleF;
    if (_instrument == MODE_CHIPTUNE) {
      float raw = (carrierPhase < (float)M_PI) ? 1.0f : -1.0f;
      chipSmooth += 0.75f * (raw - chipSmooth);
      sampleF = chipSmooth;
    } else {
      sampleF = sinf(carrierPhase + (FM_I1 + modRange) * sinf(modPhase));
      modRange *= modDecay;
      modPhase += modInc;
      if (modPhase >= 2.0f * (float)M_PI) modPhase -= 2.0f * (float)M_PI;
    }

    int16_t s = (int16_t)(amplitude() * env * sampleF);
    carrierPhase += carrierInc;
    if (carrierPhase >= 2.0f * (float)M_PI) carrierPhase -= 2.0f * (float)M_PI;
    i2s.write16(s, s);
  }
}

// Write N ms of silence to keep the I2S FIFO fed.
static void silence(int ms) {
  long n = (long)SAMPLE_RATE * ms / 1000;
  for (long i = 0; i < n; i++) i2s.write16(0, 0);
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000);

  recursive_mutex_init(&_i2cMutex);

  i2cInit();   // I2C bus recovery + Wire1 init (shared by OLED and Piano HAT)
  oledInit();  // must come before hatInit() — hatInit() draws on the OLED
  hatInit();   // hatInit() already flashes all LEDs as a blink test

  i2s.setBCLK(26);           // BCLK on GP26; LRCLK auto = GP27
  i2s.setDATA(28);           // DIN  on GP28
  i2s.setBitsPerSample(BIT_DEPTH);
  if (!i2s.begin(SAMPLE_RATE)) {
    Serial.println("ERROR: I2S init failed");
    oled.clearDisplay();
    oled.setTextSize(2);
    oled.setCursor(10, 24);
    oled.print("I2S FAIL");
    oled.display();
    while (true);
  }

  // Startup melody: C-E-G (one pass)
  const int melodyKeys[] = { 0, 4, 7 };
  const int melodyDur[]  = { 200, 200, 300 };
  for (int i = 0; i < 3; i++) {
    const Note &n = NOTES[melodyKeys[i]];
    hatSetLED(n.key, true);
    oledShowNote(n);
    playTone(n.freq, melodyDur[i]);
    hatSetLED(n.key, false);
    silence(60);
  }

  _currentNote = nullptr;
  oledRedraw();
  Serial.println("Ready — press a key");
  _coreSetupDone = true;   // release Core 1
}

// ---------------------------------------------------------------------------
// Loop — polyphonic note management + sample mixer
// ---------------------------------------------------------------------------
void loop() {
  static uint8_t prevT1 = 0, prevT2 = 0;

  recursive_mutex_enter_blocking(&_i2cMutex);
  uint8_t t1 = capReadStatus(CAP_U1);
  uint8_t t2 = capReadStatus(CAP_U2);
  recursive_mutex_exit(&_i2cMutex);

  // Special keys: rising-edge only (fires once per press, no blocking wait)
  uint8_t risen2 = t2 & ~prevT2;
  if (risen2 & 0x20) {   // Oct- (U2 bit 5)
    if (_instrument == MODE_VOLUME) {
      int v = (int)_volumePct / 2; if (v < 1) v = 1;
      _volumePct = (uint8_t)v;
      Serial.print("Volume: "); Serial.print(_volumePct); Serial.println("%");
    } else {
      if (_octaveShift > -2) _octaveShift--;
      Serial.print("Octave: "); Serial.println(_octaveShift);
    }
    _currentNote = nullptr; _oledDirty = true;
  }
  if (risen2 & 0x40) {   // Oct+ (U2 bit 6)
    if (_instrument == MODE_VOLUME) {
      int v = (int)_volumePct * 2; if (v > 64) v = 64;
      _volumePct = (uint8_t)v;
      Serial.print("Volume: "); Serial.print(_volumePct); Serial.println("%");
    } else {
      if (_octaveShift < 2) _octaveShift++;
      Serial.print("Octave: "); Serial.println(_octaveShift);
    }
    _currentNote = nullptr; _oledDirty = true;
  }
  if (risen2 & 0x80) {   // Inst (U2 bit 7)
    _instrument = (_instrument + 1) % 3;
    _currentNote = nullptr; _oledDirty = true;
    Serial.print("Instrument: ");
    Serial.println(_instrument == MODE_PIANO ? "Piano" :
                   _instrument == MODE_CHIPTUNE ? "Chip" : "Volume");
  }

  // Note release: level-triggered (release as soon as finger lifts)
  for (int i = 0; i < VOICE_COUNT; i++) {
    if (!_voices[i].active || _voices[i].releasing) continue;
    int k = _voices[i].keyIdx;
    bool held = (k < 8) ? (bool)(t1 & (1 << k)) : (bool)(t2 & (1 << (k - 8)));
    if (!held) releaseVoice(i);
  }

  // Note on: rising-edge triggered (spawn once per press)
  for (int k = 0; k < NOTES_LEN; k++) {
    bool now  = (k < 8) ? (bool)(t1 & (1 << k)) : (bool)(t2 & (1 << (k - 8)));
    bool prev = (k < 8) ? (bool)(prevT1 & (1 << k)) : (bool)(prevT2 & (1 << (k - 8)));
    if (now && !prev) spawnVoice(k);
  }

  mixAndWrite(SAMPLE_RATE / 20);   // generate ~50 ms of audio

  recursive_mutex_enter_blocking(&_i2cMutex);
  updateLEDs();
  recursive_mutex_exit(&_i2cMutex);

  prevT1 = t1;
  prevT2 = t2;
}

// ---------------------------------------------------------------------------
// Core 1 — OLED rendering
// Runs entirely independently of Core 0's audio loop.
// Wire1 access is serialised with _i2cMutex so CAP reads and OLED writes
// never overlap.
// ---------------------------------------------------------------------------
void setup1() {
  while (!_coreSetupDone);   // wait until Core 0 has finished setup()
}

void loop1() {
  if (!_oledDirty) { delay(1); return; }
  _oledDirty = false;
  oledBuildBuffer();                              // pure RAM — no I2C, no blocking
  recursive_mutex_enter_blocking(&_i2cMutex);
  oled.display();                                 // full I2C write, isolated from Core 0
  recursive_mutex_exit(&_i2cMutex);
}
