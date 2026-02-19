# ── ardu-picopiano / Makefile ────────────────────────────────────────────────
#
# Targets
#   make install-core   (one-time) install rp2040 board package via arduino-cli
#   make                compile the sketch
#   make upload         compile + upload  (set PORT= if auto-detect fails)
#   make monitor        open serial monitor at 115200 baud
#   make clean          remove build artefacts
#
# Port examples
#   macOS:  make upload PORT=/dev/cu.usbmodem14201
#   Linux:  make upload PORT=/dev/ttyACM0
# ─────────────────────────────────────────────────────────────────────────────

CLI       := arduino-cli
FQBN      := rp2040:rp2040:seeed_xiao_rp2350
BOARD_URL := https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
BAUD      := 115200
SKETCH    := .

# Auto-detect the first connected Pico; override with PORT=...
PORT ?= $(shell $(CLI) board list 2>/dev/null \
          | grep -i 'pico\|RP2' \
          | awk '{print $$1}' | head -1)

.PHONY: all build upload monitor clean install-core

all: build

build:
	$(CLI) compile --fqbn $(FQBN) $(SKETCH)

upload: build
	@if [ -z "$(PORT)" ]; then \
	  echo ""; \
	  echo "ERROR: Pico port not found.  Pass PORT= explicitly, e.g.:"; \
	  echo "  make upload PORT=/dev/cu.usbmodem14201   # macOS"; \
	  echo "  make upload PORT=/dev/ttyACM0            # Linux"; \
	  echo ""; \
	  echo "Tip: run '$(CLI) board list' to see available ports."; \
	  exit 1; \
	fi
	$(CLI) upload --fqbn $(FQBN) --port $(PORT) $(SKETCH)

monitor:
	$(CLI) monitor --port $(PORT) --config baudrate=$(BAUD)

clean:
	$(RM) -r build/

# One-time setup: add board URL, refresh index, install rp2040 core
install-core:
	$(CLI) config add board_manager.additional_urls $(BOARD_URL)
	$(CLI) core update-index
	$(CLI) core install rp2040:rp2040

# One-time setup: install required Arduino libraries
install-libs:
	$(CLI) lib install "Adafruit GFX Library"
	$(CLI) lib install "Adafruit SSD1306"
