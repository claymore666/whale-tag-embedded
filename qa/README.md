# QA Testing Environment

This directory contains the complete QA testing infrastructure for whale tag firmware, enabling **end-to-end testing without physical hardware** using LD_PRELOAD sensor simulation and QEMU ARM64 emulation.

## Quick Start

### Run End-to-End Test

```bash
./qa/e2e-test.sh
```

This will:
1. Build Docker container with QEMU and dependencies
2. Mount the SD card image using kpartx
3. Run cetiTagApp with LD_PRELOAD sensor simulation
4. Display output showing all intercepted GPIO/I2C calls

**Expected Output:**
```
[LD_PRELOAD] gpioInitialise() - Starting simulation
[LD_PRELOAD] Network control listening on UDP port 9999
[LD_PRELOAD] gpioSetMode(gpio=17, mode=1)
[LD_PRELOAD] gpioWrite(gpio=17, level=1)
...
```

## What's Included

### Core Components

| File/Directory | Purpose |
|----------------|---------|
| `e2e-test.sh` | **Main test script** - Automated end-to-end testing |
| `libpigpio_sim/` | **LD_PRELOAD library** - Simulates all hardware sensors |
| `E2E_SUCCESS.md` | Test results and architecture documentation |
| `HOW_TO_TEST.md` | Step-by-step testing instructions |
| `Dockerfile.e2e` | Multistage Docker image for E2E testing |

### Documentation

- **E2E_SUCCESS.md** - Proof of successful end-to-end test with output samples
- **E2E_TEST_SUMMARY.md** - Complete testing summary and approach comparison
- **HOW_TO_TEST.md** - Multiple testing approaches (QEMU, Docker, Raspberry Pi)

### Test Configurations

- `test_configs/` - Sample configuration files for different test scenarios
- `test_scripts/` - Additional test utilities

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
echo 'DEPTH=10' | nc -u -w1 localhost 9999

# Change water temperature
echo 'TEMP=15' | nc -u -w1 localhost 9999

# Simulate darkness (underwater)
echo 'LIGHT=0' | nc -u -w1 localhost 9999

# Set battery voltage
echo 'BATTERY=3.5' | nc -u -w1 localhost 9999
```

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

### Test Use Case Preparation

The e2e-test.sh script handles all the setup automatically:

1. **Image Preparation**
   - Uses `kpartx` to map SD card partitions
   - Mounts root filesystem to `/mnt/img`
   - No modification needed - read-only mounting works

2. **LD_PRELOAD Setup**
   - ARM64 library passed via `-E LD_PRELOAD=...` to QEMU
   - Library is accessed from host filesystem via bind mount
   - No need to install into image - dynamic linker finds it

3. **QEMU Execution**
   - `qemu-aarch64-static -L /mnt/img` uses image as root
   - ARM64 binaries execute transparently
   - All library dependencies resolved from mounted image

### Scenario 1: Basic Sensor Reading

```bash
# Run firmware with default sensor values
TIMEOUT=30 ./qa/e2e-test.sh

# Expected: Firmware initializes, reads sensors, logs data
```

### Scenario 2: Dive Simulation

```bash
# Start firmware in background
docker run --rm --privileged -d --name qa-dive-test \
  -v $(pwd):/work -w /work debian:bookworm \
  bash -c "
    apt-get update -qq && apt-get install -y -qq qemu-user-static kpartx netcat-openbsd file
    kpartx -av /work/out/sdcard.img
    LOOP=\$(kpartx -av /work/out/sdcard.img | head -1 | grep -oP 'loop\d+')
    mkdir -p /mnt/img
    mount /dev/mapper/\${LOOP}p2 /mnt/img
    qemu-aarch64-static -L /mnt/img \
      -E LD_PRELOAD=/work/qa/libpigpio_sim/build/libpigpio_sim_arm64.so \
      /mnt/img/opt/ceti-tag-data-capture/bin/cetiTagApp
  "

# Simulate dive sequence
sleep 5
echo 'DEPTH=0.5' | docker exec -i qa-dive-test nc -u -w1 localhost 9999
sleep 10
echo 'DEPTH=5' | docker exec -i qa-dive-test nc -u -w1 localhost 9999
sleep 30
echo 'DEPTH=15' | docker exec -i qa-dive-test nc -u -w1 localhost 9999
sleep 60
echo 'DEPTH=0.5' | docker exec -i qa-dive-test nc -u -w1 localhost 9999

# Check logs
docker exec qa-dive-test cat /mnt/img/data/data_pressure_temperature.csv

# Cleanup
docker stop qa-dive-test
```

### Scenario 3: APRS On Whale Testing

Test the `aprs_on_whale` feature (Issue #107):

```bash
# 1. Enable aprs_on_whale in config
mkdir -p test_data/config
echo 'aprs_on_whale=true' > test_data/config/ceti-config.txt

# 2. Run with custom config bind mount
docker run --rm --privileged \
  -v $(pwd):/work -v $(pwd)/test_data:/data \
  -w /work debian:bookworm \
  bash -c "
    apt-get update -qq && apt-get install -y -qq qemu-user-static kpartx
    kpartx -av /work/out/sdcard.img
    LOOP=\$(kpartx -av /work/out/sdcard.img | head -1 | grep -oP 'loop\d+')
    mkdir -p /mnt/img
    mount /dev/mapper/\${LOOP}p2 /mnt/img
    qemu-aarch64-static -L /mnt/img \
      -E LD_PRELOAD=/work/qa/libpigpio_sim/build/libpigpio_sim_arm64.so \
      /mnt/img/opt/ceti-tag-data-capture/bin/cetiTagApp
  "

# 3. Control depth to trigger recovery board behavior
echo 'DEPTH=0.5' | nc -u -w1 localhost 9999  # At surface - GPS should activate
echo 'DEPTH=5' | nc -u -w1 localhost 9999    # Underwater - GPS should sleep
```

## Troubleshooting

### "No space left on device" in /tmp

**Cause:** Docker container's /tmp is too small for bind mounts

**Solution:** Use kpartx + mount instead of rpi-image bind mounts (already implemented in e2e-test.sh)

### "LD_PRELOAD cannot be preloaded"

**Cause:** Library path is incorrect or architecture mismatch

**Solutions:**
1. Verify library exists: `ls -lh qa/libpigpio_sim/build/libpigpio_sim_arm64.so`
2. Check architecture: `file qa/libpigpio_sim/build/libpigpio_sim_arm64.so` (should say "ARM aarch64")
3. Ensure absolute path is used in LD_PRELOAD

### "Permission denied" mounting loop devices

**Cause:** Docker container needs privileged mode

**Solution:** Always use `--privileged` flag with docker run

### Test hangs or times out

**Cause:** Firmware waiting for user input or stuck in initialization

**Solutions:**
1. Use `timeout` command to limit execution time
2. Check if `/data` partition is mounted (firmware requires it)
3. Review logs for errors

## Development

### Adding New Sensor Simulations

1. Edit `qa/libpigpio_sim/libpigpio_sim.c`
2. Add sensor state to `SimState` struct
3. Implement I2C read/write handlers for sensor's I2C address
4. Add network control commands in `process_command()`
5. Rebuild: `cd qa/libpigpio_sim && make arm64`

Example:
```c
// Add to SimState
double new_sensor_value;

// Add to i2cReadDevice handler
case 0x50:  // New sensor at I2C address 0x50
    uint16_t raw_value = (uint16_t)(g_sim_state.new_sensor_value * 100);
    buf[0] = raw_value & 0xFF;
    buf[1] = (raw_value >> 8) & 0xFF;
    break;

// Add to process_command
if (strncmp(cmd, "NEWSENSOR=", 10) == 0) {
    g_sim_state.new_sensor_value = atof(cmd + 10);
}
```

### Running Tests on Real Hardware

Once simulation testing is complete, deploy to Raspberry Pi:

```bash
# 1. Build packages
make packages

# 2. Copy to Pi
scp out/ceti-tag-data-capture_*.deb pi@raspberrypi:~
scp qa/libpigpio_sim/build/libpigpio_sim.so pi@raspberrypi:~

# 3. Install and test
ssh pi@raspberrypi
sudo dpkg -i ceti-tag-data-capture_*.deb
LD_PRELOAD=./libpigpio_sim.so /opt/ceti-tag-data-capture/bin/cetiTagApp

# 4. Control sensors from another terminal
echo 'DEPTH=10' | nc -u -w1 raspberrypi 9999
```

## Technical Notes

### Why kpartx Instead of rpi-image?

`rpi-image run` creates temporary mounts that don't persist between invocations. For LD_PRELOAD testing, we need:

1. Persistent filesystem mount (kpartx provides this)
2. LD_PRELOAD library accessible to dynamic linker (via QEMU -E flag)
3. No image modification needed (read-only mount works)

### Why QEMU User-Mode Instead of System Emulation?

**User-mode advantages:**
- Faster startup (no full system boot)
- Direct access to host filesystem
- Easy integration with Docker
- Simpler debugging

**System emulation would require:**
- Full ARM64 kernel boot
- Network configuration
- More complex setup
- Slower execution

User-mode is perfect for testing application logic without full system simulation.

### Library Interception Order

```
cetiTagApp calls gpioSetMode()
    ↓
Dynamic linker checks LD_PRELOAD
    ↓
libpigpio_sim.so::gpioSetMode() executes (intercepted!)
    ↓
Returns simulated result
```

The real `libpigpio.so` is never called - our simulation completely replaces it.

## CI/CD Integration

### GitHub Actions Example

```yaml
name: QA Tests
on: [pull_request]

jobs:
  e2e-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build firmware
        run: make packages
      - name: Build LD_PRELOAD library
        run: cd qa/libpigpio_sim && make arm64
      - name: Run E2E tests
        run: ./qa/e2e-test.sh
```

## Related Documentation

- **E2E_SUCCESS.md** - Proof of successful testing with full output
- **E2E_TEST_SUMMARY.md** - Complete testing approach analysis
- **HOW_TO_TEST.md** - Alternative testing methods (Pi hardware, manual QEMU)
- **libpigpio_sim/README.md** - LD_PRELOAD library implementation details

## Support

For issues or questions:
1. Check documentation in this directory
2. Review test output logs
3. Examine LD_PRELOAD library source code
4. Test on real Raspberry Pi hardware if QEMU issues persist
