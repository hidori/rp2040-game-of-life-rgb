# arduino-cli with the project's configuration file
ARDUINO_CLI = arduino-cli --config-file arduino-cli.yaml

# Core platforms used by the project
CORES ?= \
	rp2040:rp2040

# Libraries used by the project
LIBS ?= \
	"Adafruit NeoPixel"

# Target boards
BOARDS = rp2040-zero rpi-pico

# FQBN for each target board
FQBN_rp2040-zero = rp2040:rp2040:waveshare_rp2040_zero
FQBN_rpi-pico    = rp2040:rp2040:rpipico

# Serial port for upload / monitor (e.g. make upload/rp2040-zero PORT=/dev/ttyACM0)
PORT ?= /dev/ttyACM0

.PHONY: all install tools/install core/install lib/install build $(addprefix build/,$(BOARDS)) $(addprefix upload/,$(BOARDS)) monitor clean

# Default target
all: $(addprefix build/,$(BOARDS))

# Install the required tools, the core platforms and the libraries
install: tools/install core/install lib/install

# Install required tools
tools/install:
	@echo "Installing required tools..."
	mise install || { echo "Failed to install required tools"; exit 1; }

# Install the core platforms
core/install:
ifeq ($(strip $(CORES)),)
	@echo "No cores defined. Skipping core/install."
else
	$(ARDUINO_CLI) core update-index
	$(ARDUINO_CLI) core install $(CORES)
endif

# Install the libraries
lib/install:
ifeq ($(strip $(LIBS)),)
	@echo "No libraries defined. Skipping lib/install."
else
	$(ARDUINO_CLI) lib update-index
	$(ARDUINO_CLI) lib install $(LIBS)
endif

# Export directory of arduino-cli for the board (build/<FQBN with ':' replaced by '.'>/)
BUILD_DIR = build/$(subst :,.,$(FQBN_$*))

# Build the sketch and export the binaries into $(BUILD_DIR)
$(addprefix build/,$(BOARDS)): build/%:
	$(ARDUINO_CLI) compile --fqbn $(FQBN_$*) --export-binaries .

# Upload $(BUILD_DIR) to the board
$(addprefix upload/,$(BOARDS)): upload/%:
	$(ARDUINO_CLI) upload --fqbn $(FQBN_$*) --port $(PORT) --input-dir $(BUILD_DIR) .

# Open the serial monitor
monitor:
	$(ARDUINO_CLI) monitor --port $(PORT)

# Remove build outputs and installed libraries
clean:
	rm -rf build .arduino
