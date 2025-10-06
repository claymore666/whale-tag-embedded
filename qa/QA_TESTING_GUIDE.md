# QA Testing Guide - Whale Tag Firmware

Complete guide for testing whale tag firmware without physical hardware using LD_PRELOAD simulation and QEMU.

## Prerequisites

### Required Software

```bash
# Install Docker (if not already installed)
curl -fsSL https://get.docker.com -o get-docker.sh
sudo sh get-docker.sh

# Verify installation
docker --version
```

### Required Files

- `out/sdcard.img` - Built SD card image (from `make build`)
- `qa/libpigpio_sim/build/libpigpio_sim_arm64.so` - ARM64 LD_PRELOAD library

## Quick Start

### 1. Build SD Card Image

**IMPORTANT:** Image building requires sudo and must be run interactively:

```bash
make build
```

This creates `out/sdcard.img` (~3.2GB).

### 2. Expand Image for Testing (One-Time Setup)

The default image is sized exactly for the content. For QA testing with log files, expand it:

```bash
sudo ./qa/expand_image.sh
```

This will:
- Create backup at `out/sdcard.img.backup`
- Add 1GB to the image
- Expand the `/data` partition (partition 3)

**Backup Management:**
- Backup is created only once
- To force new backup: `rm out/sdcard.img.backup`
- To restore corrupted image: `sudo cp out/sdcard.img.backup out/sdcard.img`

### 3. Build LD_PRELOAD Library

```bash
cd qa/libpigpio_sim
make arm64
```

This creates `build/libpigpio_sim_arm64.so`.

### 4. Run Tests

```bash
./test_prs_complete_e2e.sh
```

## LD_PRELOAD Library Development

### Workflow

When adding or modifying pigpio functions in the LD_PRELOAD library:

1. **Stash current work** (if needed):
   ```bash
   git stash
   ```

2. **Switch to feature branch**:
   ```bash
   git checkout qa/ld-preload-dev
   ```

3. **Edit library**:
   ```bash
   # Edit qa/libpigpio_sim/libpigpio_sim.c
   ```

4. **Rebuild and test**:
   ```bash
   cd qa/libpigpio_sim
   make clean && make arm64
   ```

5. **Commit to feature branch**:
   ```bash
   git add qa/libpigpio_sim/libpigpio_sim.c
   git commit -m "Description of changes"
   ```

6. **Merge to testing environment**:
   ```bash
   git checkout qa/testing-environment
   git merge qa/ld-preload-dev --no-edit
   ```

7. **Rebuild in testing environment**:
   ```bash
   cd qa/libpigpio_sim
   make clean && make arm64
   ```

8. **Restore stashed work** (if needed):
   ```bash
   git stash pop
   ```

**Key Principles:**
- ✅ Always commit to `qa/ld-preload-dev` first
- ✅ Merge to `qa/testing-environment` for integration testing
- ❌ Never commit LD_PRELOAD changes directly to `qa/testing-environment`

### Adding Missing pigpio Functions

If firmware shows "pigpio uninitialised" errors:

1. **Identify missing functions**:
   ```bash
   grep "pigpio uninitialised" test.log | awk '{print $3}' | sed 's/://' | sort | uniq -c
   ```

2. **Add function to library**:
   ```c
   // Example: Adding i2cWriteByteData
   int i2cWriteByteData(unsigned handle, unsigned reg, unsigned value) {
       unsigned addr = handle % 100;
       fprintf(stderr, "[LD_PRELOAD] i2cWriteByteData(handle=%u/addr=0x%02X, reg=0x%02X, value=0x%02X)\n",
               handle, addr, reg, value);
       return 0;  // Success
   }
   ```

3. **Follow workflow above** to commit and merge

### Currently Implemented Functions

**GPIO:**
- `gpioInitialise`, `gpioTerminate`
- `gpioSetMode`, `gpioWrite`, `gpioRead`
- `gpioSetISRFunc` (stubbed - no interrupts in simulation)

**I2C:**
- `i2cOpen`, `i2cClose`
- `i2cReadDevice`, `i2cWriteDevice`
- `i2cReadByteData`, `i2cWriteByteData`
- `i2cReadWordData`, `i2cWriteWordData`
- `i2cWriteByte`

**Serial:**
- `serOpen`, `serClose`
- `serDataAvailable` (returns 0 - no spam)
- `serRead`, `serWrite`

**Bit-Bang I2C:**
- `bbI2COpen`, `bbI2CClose`, `bbI2CZip`

**SPI:**
- `spiOpen`, `spiClose`
- `spiRead`, `spiWrite`, `spiXfer`

## Sensor Simulation

### Simulated Sensors

| Sensor | I2C Address | Simulated Data |
|--------|-------------|----------------|
| Keller 4LD Pressure | 0x40 | Depth in bar (1.0 = surface) |
| MAX17320 Battery | 0x36 | Voltage in mV |
| LTR-329ALS Light | 0x29 | Lux value |
| BNO086 IMU | 0x4A/0x4B | Fake SHTP responses |

### Network Control (UDP Port 9999)

Control simulated sensors at runtime:

```bash
# Simulate dive to 10 meters
echo 'DEPTH=10' | nc -u -w1 127.0.0.1 9999

# Surface (0.5m)
echo 'DEPTH=0.5' | nc -u -w1 127.0.0.1 9999

# Set water temperature
echo 'TEMP=15' | nc -u -w1 127.0.0.1 9999

# Simulate darkness (underwater)
echo 'LIGHT=0' | nc -u -w1 127.0.0.1 9999

# Set battery voltage
echo 'BATTERY=3.5' | nc -u -w1 127.0.0.1 9999
```

**Depth Formula:** `pressure_bar = 1.0 + (depth_meters / 10.0)`

Examples:
- 0m (surface): 1.0 bar
- 5m: 1.5 bar
- 10m: 2.0 bar
- 15m: 2.5 bar

## Firmware Logging System

### Understanding CETI_LOG Output

**Critical Discovery:** The firmware uses `syslog()` for logging, NOT stdout/stderr!

```c
// From logging.h:
#define CETI_LOG(FMT_STR, ...) syslog(LOG_DEBUG, ...)
```

**Where logs go:**
- **Bookworm (current):** `/data/journal/` (systemd-journald)
- **Bullseye (upstream):** `/var/log/syslog` (rsyslog)
- **NOT captured by:** stdout/stderr redirection in test scripts

### Accessing Firmware Logs

**Method 1: During test run (attach to running container)**
```bash
# Find container ID
docker ps

# Access journald logs
docker exec -it <container_id> journalctl -f

# Or access specific service
docker exec -it <container_id> journalctl -u ceti-tag-data-capture -f

# Or read from mounted /data/journal
docker exec -it <container_id> ls -la /mnt/img/data/journal/
```

**Method 2: After test completes (mount image)**
```bash
# Mount image manually
sudo kpartx -av out/sdcard.img
sudo mount /dev/mapper/loopXp3 /mnt/test-data

# Read journal logs
sudo journalctl --directory=/mnt/test-data/journal/

# Cleanup
sudo umount /mnt/test-data
sudo kpartx -dv out/sdcard.img
```

**Method 3: Extract logs in test script**
```bash
# Add to test script before cleanup
docker exec pr-e2e-complete journalctl -u ceti-tag-data-capture --no-pager > firmware.log
```

### What LD_PRELOAD Shows vs. Firmware Logs

**LD_PRELOAD output (visible in test logs):**
- `[LD_PRELOAD]` prefixed messages
- Hardware interface calls: `gpioWrite`, `i2cReadDevice`, `spiOpen`, etc.
- Sensor simulation: pressure readings, I2C operations, GPIO states
- FPGA bitstream fast-forward progress
- Network control commands (UDP port 9999)

**Firmware logs (hidden in syslog - requires journalctl):**
- `CETI_LOG()` messages from application code
- State machine transitions: `CONFIG → START → RECORD_SURFACE → BRN_ON`
- Sensor initialization: pressure, battery, IMU, light
- Configuration loading: `ceti-config.txt` parsing
- Thread lifecycle: audio, battery, IMU, pressure threads starting
- Burnwire activation logic and depth-aware decision making
- Error conditions: sensor failures, low battery, disk full

### Why Firmware Appears "Stuck"

**The firmware IS running, logs are just hidden!**

Evidence of operation even without visible CETI_LOG output:
1. **Sensor polling patterns** - Regular I2C reads indicate active threads:
   ```
   [LD_PRELOAD] i2cReadDevice(handle=.../addr=0x40, count=5)  # Pressure sensor
   [LD_PRELOAD] i2cReadByteData(handle=.../addr=0x21, reg=0x00)  # IOX (burnwire)
   [LD_PRELOAD] bbI2CZip(SDA=2, inLen=..., outLen=4)  # IMU header read
   ```

2. **IOX register updates** - OUTPUT register changes show state machine activity:
   ```
   [LD_PRELOAD] IOX OUTPUT register: 0x10 (BURNWIRE_ON=1)
   ```

3. **Incrementing counters** - IMU read count shows continuous operation:
   ```
   [LD_PRELOAD] IMU: No data available (read #27354)
   ```

4. **CSV file writes** - Data files appear in `/mnt/img/data/`:
   - `data_pressure_temperature.csv`
   - `data_battery.csv`
   - `data_state.csv`

**To verify state machine progression, access journald logs using methods above.**

## Test Architecture

```
┌─────────────────────────────────────────────────────┐
│  Docker Container (Debian Bookworm, --privileged)   │
│                                                       │
│  ┌────────────────────────────────────────────────┐ │
│  │  kpartx                                        │ │
│  │  - Maps SD card image partitions              │ │
│  │  - /dev/mapper/loopXp2 → rootfs (/mnt/img)    │ │
│  │  - /dev/mapper/loopXp3 → data partition       │ │
│  └────────────────────────────────────────────────┘ │
│                                                       │
│  ┌────────────────────────────────────────────────┐ │
│  │  QEMU User-Mode (qemu-aarch64-static)         │ │
│  │  - Executes ARM64 binaries on x86_64          │ │
│  │  - LD_PRELOAD intercepts pigpio calls         │ │
│  │  - UDP listener on port 9999                  │ │
│  └────────────────────────────────────────────────┘ │
│                                                       │
│  ┌────────────────────────────────────────────────┐ │
│  │  cetiTagApp (ARM64 firmware)                  │ │
│  │  - Runs as if on real Raspberry Pi            │ │
│  │  - All sensors simulated via LD_PRELOAD       │ │
│  │  - Logs to /mnt/img/data/*.csv                │ │
│  └────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────┘
```

## Test Scripts

### test_prs_complete_e2e.sh

Complete end-to-end test for PRs #111 (Depth-Aware Burnwire) and #112 (APRS On Whale).

**What it does:**
1. Creates Docker container with QEMU and dependencies
2. Mounts SD card image using kpartx
3. Injects test configuration (30s burnwire timeout)
4. Starts firmware with LD_PRELOAD
5. Waits for burnwire timeout to trigger
6. Simulates depth changes
7. Collects evidence (logs, CSV files)
8. Cleans up

**Usage:**
```bash
./test_prs_complete_e2e.sh 2>&1 | tee test-results.log
```

**Expected Evidence:**
- `data_state.csv` - State machine transitions (CONFIG → START → RECORD_SURFACE → BRN_ON)
- `data_burnwire.csv` - Burnwire activation log with depth-aware behavior
- Firmware logs showing LD_PRELOAD sensor reads

### depth_simulation_test.sh

Simple depth simulation with delays (assumes container already running).

**Usage:**
```bash
# After starting container manually
./depth_simulation_test.sh
```

## Common Issues and Solutions

### Issue: "No space left on device" in /data partition

**Solution:** Run `sudo ./qa/expand_image.sh` to add 1GB space

### Issue: "pigpio uninitialised" errors

**Cause:** Missing function in LD_PRELOAD library

**Solution:**
1. Identify function: `grep "uninitialised" log | awk '{print $3}' | sort | uniq`
2. Add to `qa/libpigpio_sim/libpigpio_sim.c` (follow workflow above)
3. Rebuild and test

### Issue: kpartx fails / loop device errors

**Solution:** Remove stale containers and try fresh:
```bash
docker rm -f pr-e2e-complete
./test_prs_complete_e2e.sh
```

### Issue: Image corruption after expand_image.sh

**Solution:** Restore from backup:
```bash
sudo cp out/sdcard.img.backup out/sdcard.img
```

### Issue: Make build requires sudo password

**Cause:** Build process uses loop devices for partition manipulation

**Solution:** Run `make build` in an interactive terminal (not through automation)

### Issue: Build fails with "the input device is not a TTY"

**Cause:** Running in non-interactive environment (e.g., Claude Code)

**Solution:** Run `make build` directly in your terminal

## Branch Structure

```
main (upstream)
  ├── qa/ld-preload-dev (LD_PRELOAD library development)
  │   └── Changes to qa/libpigpio_sim/*
  │
  └── qa/testing-environment (integration testing)
      ├── Merges from qa/ld-preload-dev
      ├── Merges from PR branches (e.g., depth-aware-burnwire, aprs-on-whale)
      └── Test scripts and configurations
```

**Workflow:**
1. Develop LD_PRELOAD features on `qa/ld-preload-dev`
2. Merge PR branches into `qa/testing-environment`
3. Merge `qa/ld-preload-dev` into `qa/testing-environment`
4. Run tests on `qa/testing-environment`
5. When ready, create PRs from feature branches to `main`

## CI/CD Considerations

### GitHub Actions Limitations

- ❌ Requires `sudo` for image building
- ❌ Needs privileged Docker (loop devices)
- ❌ Interactive TTY required for `make build`

### Possible Solutions

1. **Pre-built images:** Build SD card images manually, upload as artifacts
2. **Mock testing:** Use LD_PRELOAD on pre-compiled binaries
3. **Self-hosted runners:** Use dedicated hardware with sudo access

## Files Reference

| File | Purpose |
|------|---------|
| `qa/QA_TESTING_GUIDE.md` | This file - comprehensive testing guide |
| `qa/README.md` | QA environment overview and architecture |
| `qa/libpigpio_sim/libpigpio_sim.c` | LD_PRELOAD library source |
| `qa/libpigpio_sim/Makefile` | Build system (x86_64 and ARM64) |
| `qa/expand_image.sh` | SD card image expansion utility |
| `qa/resize_filesystem.sh` | Filesystem resize helper |
| `test_prs_complete_e2e.sh` | Complete E2E test script |
| `depth_simulation_test.sh` | Simple depth simulation |

## Next Steps

1. **Complete E2E test debugging** - Firmware should reach all states
2. **Add test assertions** - Verify expected state transitions
3. **Create test matrix** - Different configurations and scenarios
4. **Document expected outputs** - What success looks like for each PR
5. **Automate evidence collection** - Extract key metrics from logs

## Resources

- **CLAUDE.md** - Project-wide instructions and build system
- **RUNTIME_ARCHITECTURE.md** - Firmware state machine and data formats
- **ISSUES_ASSESSMENT.md** - GitHub issues with scientific context

For questions or issues, check the test logs first, then review the LD_PRELOAD library source code.
