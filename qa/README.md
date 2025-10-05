# Whale Tag QA Testing Infrastructure

This directory contains testing infrastructure for whale tag firmware using LD_PRELOAD simulation.

## Quick Start

### 1. Build LD_PRELOAD Library

```bash
# The LD_PRELOAD library comes from qa/ld-preload-dev branch
# It will be available after merging that branch into your test branch

cd qa/libpigpio_sim
make
```

### 2. Run Tests

```bash
# Terminal 1: Start firmware with LD_PRELOAD
LD_PRELOAD=./qa/libpigpio_sim/build/libpigpio_sim.so \
  /opt/ceti-tag-data-capture/bin/cetiTagApp

# Terminal 2: Run test scripts
cd qa/test_scripts
python3 test_aprs_on_whale.py
```

## Directory Structure

```
qa/
├─ libpigpio_sim/          # LD_PRELOAD simulation library (from qa/ld-preload-dev)
│  ├─ libpigpio_sim.c
│  ├─ Makefile
│  └─ README.md
│
├─ test_scripts/           # Test automation scripts
│  ├─ test_aprs_on_whale.py
│  └─ common/              # Shared test utilities
│
├─ test_configs/           # Test configuration files
│  ├─ aprs_disabled.txt
│  └─ aprs_enabled.txt
│
└─ docs/                   # QA documentation
   ├─ TESTING_GUIDE.md
   └─ LD_PRELOAD_DEVELOPMENT.md
```

## Available Tests

### test_aprs_on_whale.py

Tests APRS on whale feature (Issue #109, PR #112):
- Recovery board sleeps when `aprs_on_whale=false` at surface
- Recovery board wakes when `aprs_on_whale=true` at surface
- Complete dive sequence simulation

**Usage:**
```bash
python3 test_scripts/test_aprs_on_whale.py
```

## Network Control

The LD_PRELOAD library listens on UDP port 9999 for sensor control:

```bash
# Set depth (meters)
echo 'DEPTH=10' | nc -u -w1 localhost 9999

# Set temperature (Celsius)
echo 'TEMP=15' | nc -u -w1 localhost 9999

# Set light (lux)
echo 'LIGHT=500' | nc -u -w1 localhost 9999

# Set battery (volts)
echo 'BATTERY=3.6' | nc -u -w1 localhost 9999
```

## Creating New Tests

1. Create Python script in `test_scripts/`
2. Use `WhaleTagSimulator` class for sensor control
3. Run firmware with LD_PRELOAD
4. Verify behavior in logs

Example:
```python
from common.whale_tag_sim import WhaleTagSimulator

def test_my_feature():
    sim = WhaleTagSimulator()
    sim.set_depth(10)  # 10m depth
    time.sleep(5)
    # Check logs for expected behavior
    sim.close()
```

## Limitations

LD_PRELOAD simulation tests **software logic only**:
- ✅ State machine behavior
- ✅ Config parsing
- ✅ Integration between components
- ✅ Thread coordination

It does **NOT** test:
- ❌ Real hardware timing
- ❌ I2C bus errors
- ❌ Power consumption
- ❌ FPGA behavior

**Hardware validation is still required before deployment!**

## Troubleshooting

### Port 9999 already in use
```bash
# Find process using port 9999
lsof -i :9999
# Kill it or use different port
```

### LD_PRELOAD not intercepting functions
```bash
# Check library is loaded
LD_DEBUG=libs LD_PRELOAD=./qa/libpigpio_sim/build/libpigpio_sim.so cetiTagApp 2>&1 | grep libpigpio_sim

# Verify symbols are exported
nm -D qa/libpigpio_sim/build/libpigpio_sim.so | grep gpio
```

### Firmware crashes
```bash
# Run with verbose LD_PRELOAD logging
LD_PRELOAD=./qa/libpigpio_sim/build/libpigpio_sim.so cetiTagApp 2>&1 | tee firmware.log
```

## Branch Strategy

This QA infrastructure lives in `qa/testing-environment` branch:
- **Never** merged to `main`
- **Never** pushed to upstream (Project-CETI)
- Only used in your fork for testing before PR submission

See `../QA_ENVIRONMENT_PLAN.md` for complete workflow.
