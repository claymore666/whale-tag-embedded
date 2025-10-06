# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## User Preferences

**Commit Workflow:**
- User prefers to review commits and commit messages before pushing
- NEVER push to remote without explicit user approval
- Always present commit message for review before committing
- User will explicitly say "push" or "push it" when ready

**Command Execution:**
- NEVER attempt sudo commands - always ask user to execute them
- Commands requiring sudo (like `make build`) must be run by the user in an interactive terminal
- If a command needs sudo, explicitly tell the user to run it

## Supplemental Documentation (Local Only - NOT in Git)

These files provide additional context and are maintained locally:

- **ISSUES_ASSESSMENT.md**: Detailed analysis of open GitHub issues with scientific context from CETI publications
  - Priority rankings and implementation feasibility
  - Field deployment context and use cases
  - Updated as issues are addressed
  - Tracks implementation status

- **RUNTIME_ARCHITECTURE.md**: Technical documentation of tag runtime behavior
  - State machine details
  - Data file formats and logging
  - Sensor integration
  - Power management

## QA Testing Environment

**Location**: `qa/` directory

The QA environment enables end-to-end firmware testing without physical hardware using LD_PRELOAD sensor simulation and QEMU ARM64 emulation.

**Documentation**: See **`qa/QA_TESTING_GUIDE.md`** for complete setup and testing procedures.

**Quick Reference**:
```bash
# Build and test workflow
make build                           # Build SD card image (requires interactive terminal + sudo)
sudo ./qa/expand_image.sh            # Expand image for testing (one-time setup)
cd qa/libpigpio_sim && make arm64    # Build LD_PRELOAD library
./test_prs_complete_e2e.sh           # Run E2E tests
```

**Key Components**:
- **`qa/libpigpio_sim/`** - LD_PRELOAD library that simulates all hardware sensors
- **`qa/expand_image.sh`** - Utility to expand SD card images for testing
- **`test_prs_complete_e2e.sh`** - Complete E2E test script
- **`qa/QA_TESTING_GUIDE.md`** - Comprehensive testing guide

**Branch Structure**:
- `qa/ld-preload-dev` - Feature branch for LD_PRELOAD library development
- `qa/testing-environment` - Integration testing branch (merges from qa/ld-preload-dev + PR branches)

**Important Notes**:
- Image building requires sudo and must be run interactively (not through automation)
- Always commit LD_PRELOAD changes to `qa/ld-preload-dev` first, then merge to `qa/testing-environment`
- See `qa/QA_TESTING_GUIDE.md` for complete LD_PRELOAD development workflow

## Project Overview

This repository contains the embedded software for CETI (Project CETI) whale tags deployed on sperm whales for data collection. The tags are built on Raspberry Pi hardware with custom bonnets and FPGA-driven hydrophones.

## Hardware Versions and Branches

- **main branch**: Targets actively deployed hardware in the field
- **v0**: Raspberry Pi Zero W with OctoBoard soundcard
- **v2**: Raspberry Pi Zero W with three custom bonnets, FPGA-driven hydrophones (Jan'22 deployment)
- **v2_2**: Raspberry Pi Zero 2 W (arm64 quad-core) with electrical/mechanical changes
- Releases are tagged for each hardware version

## OS Version Migration (Bookworm)

**✅ Completed - Tracked in Issue #113**

- **Upstream (Project-CETI/main)**: Currently uses **Raspbian Bullseye** (Debian 11)
- **Migration branch** (`fix/migrate-bullseye-to-bookworm`): **Raspbian Bookworm** (Debian 12)
  - Bullseye images are no longer available from Raspberry Pi Foundation
  - Migration includes switching from rsyslog to journald (systemd-journal)
  - Journald stores persistent logs in `/data/journal/` instead of `/data/logs/`
  - See **Issue #113** for complete migration details and status

**Key Changes from Bullseye → Bookworm:**
- `Makefile:117`: `raspios-bullseye-arm64-lite` → `raspios-bookworm-arm64-lite`
- `build/Dockerfile`: `debian:bullseye` → `debian:bookworm` (for e2fsck 1.47+ support)
- `build/Dockerfile`: Package renames: `qemu` → `qemu-user-static`, `netcat` → `netcat-openbsd`
- `build/setup_image.sh`: rsyslog → journald with symlink to `/data/journal`
- **All hardware drivers verified compatible** (I2C, SPI, GPIO, UART - same kernel 6.1.x)

## Build System

### Prerequisites Installation

```bash
sudo apt-get remove docker docker-engine docker.io containerd runc
curl -fsSL https://get.docker.com -o get-docker.sh
sudo sh get-docker.sh
sudo apt install dos2unix binfmt-support qemu-system-common qemu-user-static
```

### Build Commands

**IMPORTANT:** `make build` requires sudo access and must be run in an interactive terminal (not through Claude Code or automation).

```bash
# Full build (Debian packages + SD card image) - REQUIRES INTERACTIVE TERMINAL
make build

# Build only Debian packages
make packages

# Clean build artifacts
make clean

# Deep clean (removes Docker images)
make deep_clean

# Run tests
make test

# Lint code
make lint

# Lint and auto-fix
make lint_fix

# Open Docker shell for debugging
make docker-shell
```

The build process runs in Docker using QEMU to emulate ARM64 architecture, downloads the latest Raspbian Lite image, builds Debian packages, and creates a complete SD card image in `out/`.

### Package-Specific Builds

To build individual packages within the data capture package:

```bash
cd packages/ceti-tag-data-capture
make build          # Build all binaries
make debug          # Build with debug flags
make test           # Run unit tests (Unity framework with stubs/mocks/fakes)
make clean          # Clean build artifacts
```

**Test Structure**: Tests use Unity framework with organized test doubles:
- `tests/src/`: Test source files (*.test.c)
- `tests/stubs/`: Stub implementations (*.stub.c)
- `tests/mocks/`: Mock implementations (*.mock.c)
- `tests/fakes/`: Fake implementations (*.fake.c)
- Test dependencies defined in `Test.mk`

## Architecture

### Debian Package Structure

The system uses Debian packages for deployment:

1. **ceti-tag-set-hostname** (`packages/ceti-tag-set-hostname`): Systemd service runs `/opt/ceti-tag-set-hostname/ceti-tag-set-hostname.sh` at device start to set unique hostname based on MAC address (format: `wt-<MAC>`). Uses Wi-Fi MAC if available, falls back to Ethernet MAC, then CPU serial. Critical for avoiding hostname collisions and enabling data pipeline to uniquely identify tag sources.

2. **ceti-tag-data-capture**: Main data collection application (see below)

### Data Capture Application (`ceti-tag-data-capture`)

**Location**: `packages/ceti-tag-data-capture`

**Entry Point**: Systemd service launches `/opt/ceti-tag-data-capture/ipc/tagMonitor.sh` (auto-restarts on failure for maximum reliability)

**Core Architecture**:
- **IPC**: Shell scripts in `ipc/` directory use named pipes (`cetiCommand`, `cetiResponse`) for process communication
- **Main Apps**: Two C applications in `src/`
  - `cetiTagApp`: Main data acquisition and control
  - `cetiHWTest`: Hardware testing utility
- **Subcommands**: Modular command system in `src/cetiTagApp/subcommands/`:
  - `cmd_audio.c`: Audio data acquisition
  - `cmd_battery.c`: Battery management
  - `cmd_burnwire.c`: Burnwire control
  - `cmd_fpga.c`: FPGA interaction
  - `cmd_imu.c`: IMU sensor control
  - `cmd_mission.c`: Mission state management
  - `cmd_recovery.c`: Recovery operations
- **Shared Memory**: System uses POSIX shared memory for sensor data exchange (audio, battery, ECG, IMU, light, pressure, recovery)
- **State Machine**: `state_machine.c` manages tag operational states
- **Data Storage**: All data written to `/data` directory

**Dependencies**: `pigpio`, `libFLAC`, pthread, math, realtime libraries

### FPGA Integration

FPGA code in `packages/ceti-tag-data-capture/FPGA_v2p1/` drives hydrophone array (v2.1 hardware).

### Overlay System

`overlay/` directory contains filesystem overlays applied during image build for system configuration.

## Deployment

### Full SD Card

```bash
# After make build
dd if=out/sdcard.img of=/dev/sdX bs=4M
```

### Updating Individual Packages

```bash
scp ceti-tag-data-capture_X.X-X_all.deb pi@raspberrypi:~
ssh pi@raspberrypi
sudo dpkg -i ceti-tag-data-capture_X.X-X_all.deb
```

### Updating Package Versions

Edit `debian/changelog` in the package directory, increment version, add description, then rebuild.

## Data Storage Structure

The `/data` partition is a separate ext4 partition labeled `cetiData` that stores all collected sensor data and logs.

### `/data` Directory Layout

```
/data/
├── <epoch_ms>.flac                 # Audio files
├── <epoch_ms>.raw                  # Raw audio files (if FLAC disabled)
├── data_audio_status.csv           # Audio acquisition status/metadata
├── data_battery.csv                # Battery gauge readings
├── data_ecg_<NN>.csv               # ECG sensor data (multi-file)
├── data_gps.csv                    # GPS recovery data
├── data_imu_quat_<NN>.csv          # IMU quaternion data (multi-file)
├── data_imu_accel_<NN>.csv         # IMU accelerometer data (multi-file)
├── data_imu_gyro_<NN>.csv          # IMU gyroscope data (multi-file)
├── data_imu_mag_<NN>.csv           # IMU magnetometer data (multi-file)
├── data_light.csv                  # Light sensor readings
├── data_pressure_temperature.csv   # Pressure/temperature sensor data
├── data_state.csv                  # State machine transitions
├── data_burnwire.csv               # Burnwire depth-aware event log
├── data_systemMonitor.csv          # System resource monitoring (CPU, RAM, disk, temps)
├── data_config_<unix_epoch_s>.txt  # Deployment configuration snapshot
├── data_tag_info_<unix_epoch_s>.yaml  # Tag metadata/info snapshot
├── burnwire_timeout_start_time_s.csv  # Burnwire timeout persistence (single timestamp)
├── config/
│   └── ceti-config.txt             # Runtime config overrides
├── logs/ or journal/               # System logs (rsyslog or journald)
│   └── syslog                      # Main system log
└── swap/                           # Swap file location (if configured)
    └── swapfile
```

### Filename Format Specifications

**Audio Files:**
- Format: `<epoch_ms>.flac` or `<epoch_ms>.raw`
- `<epoch_ms>`: Unix epoch milliseconds at recording start
- Example: `1678901234567.flac`
- Calculated: `(tv_sec * 1000) + (tv_usec / 1000)`

**Multi-file Datasets (ECG, IMU):**
- Format: `data_<sensor>_<NN>.csv` or `data_imu_<type>_<NN>.csv`
- `<NN>`: Zero-padded 2-digit counter (00, 01, 02, ...)
- Counter increments when file size exceeds limit
- Examples:
  - `data_ecg_00.csv`, `data_ecg_01.csv`
  - `data_imu_quat_00.csv`, `data_imu_accel_00.csv`, `data_imu_gyro_00.csv`, `data_imu_mag_00.csv`
- IMU file size limit: 1024 MB per file
- Counter persists across file rollovers to avoid filename collisions

**Timestamped Metadata Files:**
- Format: `data_<type>_<unix_epoch_s>.<ext>`
- `<unix_epoch_s>`: Unix epoch seconds at mission/deployment start
- Examples:
  - `data_config_1678901234.txt` - Deployment configuration
  - `data_tag_info_1678901234.yaml` - Tag hardware/software metadata

**Single CSV Files:**
- Fixed names: `data_battery.csv`, `data_light.csv`, `data_gps.csv`, etc.
- Append-mode, continuously written during mission
- Include headers written at file creation

### Data File Details

- **Audio**: FLAC-compressed files named by Unix epoch milliseconds at recording start
- **CSV files**: Timestamped sensor readings with headers
- **Multi-file datasets** (ECG, IMU): Split into multiple files with counters when size limits are reached
- **Logs**:
  - **Bookworm (current)**: journald persistent storage in `/data/journal/`
  - **Bullseye (GitHub main)**: rsyslog files in `/data/logs/`
- **Config**: Deployment parameters logged at mission start for reproducibility

### Storage Management

- Minimum free space threshold: 1 GiB (triggers state changes)
- System monitoring tracks `/data` partition usage in `data_systemMonitor.csv`
- Journald limited to 500M total, 50M per file (if used)

## Important Notes

- **Depth-Aware Burnwire** (v2.3+): Burnwire only heats when tag is underwater (>4m depth by default), preventing ineffective surface heating during whale breathing intervals. Active burn time tracked separately from calendar time. See RUNTIME_ARCHITECTURE.md for details.
- Build targets can bypass Docker by directly specifying file targets, but Docker build is recommended for consistency
- The `/data` partition is mounted by label `cetiData` and created during image build
- All sensor data files use CSV format except audio (FLAC)
