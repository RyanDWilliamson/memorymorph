# Cassette LoFi Junky — build / flash helpers.
#
# BOARD selects the Daisy Seed revision (codec). CONFIRM YOUR REV before flashing:
#   seed      = original Seed  (AK4556)
#   seed_1_1  = Seed 1.1       (WM8731)   <- default
#   seed_1_2  = Seed 1.2       (PCM3060)
#
#   make build                 # compile (default features: seed_1_1 + 96 kHz)
#   make flash-dfu             # flash the full cassette firmware (Seed in DFU first)
#   make flash-driftwood       # flash the Driftwood firmware
#   make flash-bringup         # codec-independent board test (older, hidden LED)
#   make flash-validate        # codec-independent visible-LED HIL validation
#   make flash-dfu BOARD=seed
#   make flash                 # probe-rs (requires a debug probe)

CARGO := cargo
BOARD ?= seed_1_1
FEATURES := $(BOARD),sampling_rate_96khz
# The package ships several binaries (cassette + driftwood firmware, plus the
# bring-up/validate tests), so objcopy must be told which one to convert.
FIRMWARE_BIN := cassette-lofi-junky

.PHONY: build
build:
	$(CARGO) build --release --no-default-features --features $(FEATURES)

.PHONY: check
check:
	$(CARGO) check --no-default-features --features $(FEATURES)

.PHONY: clippy
clippy:
	$(CARGO) clippy --no-default-features --features $(FEATURES) -- -D warnings

# Enter DFU first: hold BOTH footswitches with wet/dry (KNOB_5) fully dry ~1.5 s,
# or (enclosure open) hold BOOT + tap RESET on the Seed. Confirm with `dfu-util -l`.

# NOTE: with `:leave`, the board resets into the app immediately, so dfu-util's
# trailing get_status fails ("Error during download get_status", exit 74) even
# though "File downloaded successfully" printed. That tail error is benign, so we
# swallow it — a real failure shows up earlier (no "File downloaded successfully").

# Flash the full cassette firmware.
.PHONY: flash-dfu
flash-dfu:
	$(CARGO) objcopy --release --no-default-features --features $(FEATURES) --bin $(FIRMWARE_BIN) -- -O binary target/program.bin
	dfu-util -a 0 -s 0x08000000:leave -D target/program.bin -d ,0483:df11 || true

# Flash the Driftwood firmware (dual-engine TIME/SPACE ambient pedal).
.PHONY: flash-driftwood
flash-driftwood:
	$(CARGO) objcopy --release --no-default-features --features $(FEATURES) --bin driftwood -- -O binary target/driftwood.bin
	dfu-util -a 0 -s 0x08000000:leave -D target/driftwood.bin -d ,0483:df11 || true

# Flash the codec-independent bring-up test (board health + DFU gesture).
.PHONY: flash-bringup
flash-bringup:
	$(CARGO) objcopy --release --no-default-features --features $(FEATURES) --bin bringup -- -O binary target/bringup.bin
	dfu-util -a 0 -s 0x08000000:leave -D target/bringup.bin -d ,0483:df11 || true

# Flash the visible-only HIL validation firmware (all 6 knobs / 3 toggles /
# 2 footswitches / DFU gesture verified on the two footswitch LEDs only).
.PHONY: flash-validate
flash-validate:
	$(CARGO) objcopy --release --no-default-features --features $(FEATURES) --bin validate -- -O binary target/validate.bin
	dfu-util -a 0 -s 0x08000000:leave -D target/validate.bin -d ,0483:df11 || true

# Flash via a debug probe (probe-rs), if one is attached.
.PHONY: flash
flash:
	$(CARGO) run --release --no-default-features --features $(FEATURES)
