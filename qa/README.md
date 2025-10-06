# QA Testing Environment

This directory contains the complete QA testing infrastructure for whale tag firmware, enabling **end-to-end testing without physical hardware** using LD_PRELOAD sensor simulation and QEMU ARM64 emulation.

## Documentation

📖 **[QA_TESTING_GUIDE.md](QA_TESTING_GUIDE.md)** - **START HERE** - Complete testing guide with setup, workflows, and troubleshooting

## Quick Start

```bash
# 1. Build SD card image (requires interactive terminal for sudo)
make build

# 2. Expand image for testing (one-time setup)
sudo ./qa/expand_image.sh

# 3. Build LD_PRELOAD library
cd qa/libpigpio_sim && make arm64

# 4. Run complete E2E test
./test_prs_complete_e2e.sh
```

## What's Included

### Core Components

| File/Directory | Purpose |
|----------------|---------|
| `QA_TESTING_GUIDE.md` | **Main documentation** - Complete testing guide |
| `libpigpio_sim/` | **LD_PRELOAD library** - Simulates all hardware sensors |
| `expand_image.sh` | Utility to expand SD card image for testing |
| `resize_filesystem.sh` | Helper to resize filesystems on expanded partitions |
| `test_prs_complete_e2e.sh` | Complete E2E test for PRs #111 and #112 |
| `depth_simulation_test.sh` | Simple depth simulation script |

### Documentation

- **QA_TESTING_GUIDE.md** - **Main guide** - Complete setup and workflow documentation
- **libpigpio_sim/README.md** - LD_PRELOAD library implementation details
- **E2E_SUCCESS.md** - Historical proof of concept (reference only)
- **E2E_TEST_SUMMARY.md** - Historical testing approach comparison (reference only)

## How It Works

### Architecture

```
┌─────────────────────────────────────────┐
│  Docker Container (Privileged)          │
│                                          │
│  ┌────────────────────────────────────┐ │
│  │  kpartx                            │ │
│  │  Maps SD card image partitions    │ │
│  │  /dev/mapper/loopXp2 → /mnt/img   │ │
│  └────────────────────────────────────┘ │
│                                          │
│  ┌────────────────────────────────────┐ │
│  │  QEMU User-Mode (qemu-aarch64)    │ │
│  │  - Executes ARM64 binaries         │ │
│  │  - LD_PRELOAD=libpigpio_sim.so     │ │
│  │  - Intercepts pigpio library calls │ │
│  └────────────────────────────────────┘ │
│                                          │
│  ┌────────────────────────────────────┐ │
│  │  cetiTagApp (ARM64 firmware)      │ │
│  │  - Believes it's on real hardware  │ │
│  │  - All sensors simulated           │ │
│  └────────────────────────────────────┘ │
└─────────────────────────────────────────┘
```

### LD_PRELOAD Sensor Simulation

The `libpigpio_sim` library intercepts all pigpio calls and simulates:

- **Pressure Sensor** (Keller 4LD at I2C 0x40)
- **Battery Gauge** (MAX17320 at I2C 0x36)
- **Light Sensor** (LTR-329ALS at I2C 0x29)
- **IMU** (BNO086 at I2C 0x4A/0x4B)
- **GPIO** pins (all modes and states)
- **SPI** devices (FPGA communication)

### Network Control

The LD_PRELOAD library listens on **UDP port 9999** for runtime sensor control:

```bash
# Simulate dive to 10m depth
echo 'DEPTH=10' | nc -u -w1 127.0.0.1 9999

# Change water temperature
echo 'TEMP=15' | nc -u -w1 127.0.0.1 9999

# Simulate darkness (underwater)
echo 'LIGHT=0' | nc -u -w1 127.0.0.1 9999

# Set battery voltage
echo 'BATTERY=3.5' | nc -u -w1 127.0.0.1 9999
```

**Note:** Use `127.0.0.1` when running in Docker, or `localhost` when testing locally.

## LD_PRELOAD Development Workflow

**Feature Branch:** `qa/ld-preload-dev`
**Testing Branch:** `qa/testing-environment`

```bash
# 1. Work on feature branch
git checkout qa/ld-preload-dev
# Edit qa/libpigpio_sim/libpigpio_sim.c
cd qa/libpigpio_sim && make clean && make arm64
git commit -am "Add new pigpio function"

# 2. Merge to testing environment
git checkout qa/testing-environment
git merge qa/ld-preload-dev --no-edit
cd qa/libpigpio_sim && make clean && make arm64

# 3. Run tests
./test_prs_complete_e2e.sh
```

See **QA_TESTING_GUIDE.md** for detailed workflow.

## Building

### Build LD_PRELOAD Library

```bash
cd qa/libpigpio_sim
make              # Build for x86_64 (local testing)
make arm64        # Cross-compile for ARM64 (Pi/QEMU)
```

Output:
- `build/libpigpio_sim.so` - x86_64 version
- `build/libpigpio_sim_arm64.so` - ARM64 version

### Build Firmware

From repository root:

```bash
make packages     # Build Debian packages
make build        # Build complete SD card image
```

Output:
- `out/ceti-tag-data-capture_*.deb` - Firmware package
- `out/sdcard.img` - Bootable SD card image

## Testing Scenarios

See **[QA_TESTING_GUIDE.md](QA_TESTING_GUIDE.md)** for complete testing scenarios including:

- PR #111: Depth-Aware Burnwire Control
- PR #112: APRS On Whale Configuration
- Dive simulations
- Battery low scenarios

### Quick Test Example

```bash
# Run complete E2E test
./test_prs_complete_e2e.sh

# Or run with custom depth simulation
docker exec pr-e2e-complete bash -c 'echo "DEPTH=10" | nc -u -w1 127.0.0.1 9999'
```

## Troubleshooting

Common issues and solutions are documented in **[QA_TESTING_GUIDE.md](QA_TESTING_GUIDE.md#common-issues-and-solutions)**, including:

- "No space left on device" → Run `sudo ./qa/expand_image.sh`
- "pigpio uninitialised" errors → Add missing functions to LD_PRELOAD library
- kpartx/loop device failures → Clean up stale containers
- Image corruption → Restore from `out/sdcard.img.backup`

## Key Technical Notes

### LD_PRELOAD Library Interception

```
cetiTagApp → gpioSetMode()
    ↓
Dynamic linker checks LD_PRELOAD
    ↓
libpigpio_sim.so::gpioSetMode() (intercepted!)
    ↓
Returns simulated result
```

The real `libpigpio.so` is never called - our simulation completely replaces it.

### Why kpartx?

- `rpi-image run` creates temporary mounts (changes don't persist)
- kpartx provides persistent loop device mounts
- Allows LD_PRELOAD library access via QEMU -E flag
- No image modification needed

### Why QEMU User-Mode?

- ✅ Fast startup (no full system boot)
- ✅ Direct host filesystem access
- ✅ Simple Docker integration
- ❌ System emulation would require kernel boot, networking, etc.

## Documentation Index

| Document | Purpose |
|----------|---------|
| **QA_TESTING_GUIDE.md** | **START HERE** - Complete testing guide |
| README.md | This file - Overview and quick reference |
| libpigpio_sim/README.md | LD_PRELOAD library implementation |
| E2E_SUCCESS.md | Historical proof of concept |
| E2E_TEST_SUMMARY.md | Historical approach comparison |

## Support

1. Read **[QA_TESTING_GUIDE.md](QA_TESTING_GUIDE.md)**
2. Check test output logs
3. Review LD_PRELOAD library source code
4. Consult CLAUDE.md for build system questions
