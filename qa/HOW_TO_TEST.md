# How to Test Whale Tag Firmware with LD_PRELOAD

## Summary

The QA environment is **ready** with LD_PRELOAD sensor simulation. Due to how `rpi-image run` works (non-persistent changes), the most practical testing approach is on real Raspberry Pi hardware.

## ✅ Recommended: Test on Raspberry Pi

This is the simplest and most reliable approach:

### Step 1: Build Everything
```bash
# On your development machine, in qa/testing-environment branch
make packages                    # Build firmware
cd qa/libpigpio_sim && make      # Build LD_PRELOAD library
```

### Step 2: Copy to Raspberry Pi
```bash
# Copy firmware and LD_PRELOAD library to Pi
scp out/ceti-tag-data-capture_*.deb pi@raspberrypi:~
scp qa/libpigpio_sim/build/libpigpio_sim.so pi@raspberrypi:~
```

### Step 3: Install and Test on Pi
```bash
# SSH into Pi
ssh pi@raspberrypi

# Install firmware
sudo dpkg -i ceti-tag-data-capture_*.deb

# Run with LD_PRELOAD (simulates all sensors)
LD_PRELOAD=./libpigpio_sim.so /opt/ceti-tag-data-capture/bin/cetiTagApp
```

### Step 4: Control Simulated Sensors
```bash
# In another terminal, send sensor commands via UDP
echo 'DEPTH=10' | nc -u -w1 raspberrypi 9999      # Dive to 10m
echo 'DEPTH=0.5' | nc -u -w1 raspberrypi 9999     # Surface
echo 'TEMP=15' | nc -u -w1 raspberrypi 9999       # 15°C water
echo 'LIGHT=0' | nc -u -w1 raspberrypi 9999       # Darkness (underwater)
echo 'BATTERY=3.5' | nc -u -w1 raspberrypi 9999   # Low battery
```

### Expected Output
You should see LD_PRELOAD messages in the logs:
```
[LD_PRELOAD] gpioInitialise() - Starting simulation
[LD_PRELOAD] Network control listening on UDP port 9999
[LD_PRELOAD] i2cOpen(bus=1, addr=0x40, flags=0)
[LD_PRELOAD] Set depth to 10.0m (2.00 bar)
```

## Alternative: Flash SD Card Image

If you want to test the complete SD card image:

### Step 1: Flash SD Card
```bash
# After building
sudo dd if=out/sdcard.img of=/dev/sdX bs=4M status=progress
```

### Step 2: Mount and Install LD_PRELOAD
```bash
# Mount the SD card root partition
sudo mount /dev/sdX2 /mnt

# Copy LD_PRELOAD library
sudo cp qa/libpigpio_sim/build/libpigpio_sim.so /mnt/usr/lib/

# Unmount
sudo umount /mnt
```

### Step 3: Boot and Test
```bash
# Insert SD card into Pi and boot
# SSH in and run:
LD_PRELOAD=/usr/lib/libpigpio_sim.so /opt/ceti-tag-data-capture/bin/cetiTagApp
```

## Why QEMU User-Mode Doesn't Work Well

We discovered that `rpi-image run` creates a temporary mount for each command, so changes don't persist:

```bash
# This copies the file...
build/rpi-image run --image sdcard.img cp /source/lib.so /usr/lib/

# But this run sees a fresh image (file is gone)
build/rpi-image run --image sdcard.img ls /usr/lib/lib.so  # Not found!
```

**Root cause:** Each `rpi-image run` creates a fresh mount of the image, so changes made in one run aren't visible to the next run.

**Solution:** Either modify the actual image file (requires mounting the filesystem), or test on real hardware.

## What We Have Working

✅ **LD_PRELOAD library** - Builds for ARM64, intercepts all pigpio calls
✅ **Firmware packages** - Built with Bookworm (Debian 12)
✅ **SD card images** - Bootable Raspbian with firmware pre-installed
✅ **Sensor simulation** - UDP control of depth, temp, light, battery
✅ **Documentation** - Complete testing guides

## Test Scenarios

### Scenario 1: APRS On Whale Feature
```bash
# Test with aprs_on_whale=false (default)
LD_PRELOAD=./libpigpio_sim.so /opt/ceti-tag-data-capture/bin/cetiTagApp

# In another terminal:
echo 'DEPTH=0.5' | nc -u -w1 localhost 9999  # At surface
# Check logs - recovery board should sleep

# Now test with aprs_on_whale=true
# Edit /data/config/ceti-config.txt, add: aprs_on_whale=true
# Restart cetiTagApp
echo 'DEPTH=0.5' | nc -u -w1 localhost 9999  # At surface
# Check logs - recovery board should wake
```

### Scenario 2: Dive Sequence
```bash
# Simulate a complete dive
LD_PRELOAD=./libpigpio_sim.so /opt/ceti-tag-data-capture/bin/cetiTagApp &

# In another terminal:
echo 'DEPTH=0.5' | nc -u -w1 localhost 9999  # Surface
sleep 10
echo 'DEPTH=5' | nc -u -w1 localhost 9999    # Shallow dive
sleep 30
echo 'DEPTH=15' | nc -u -w1 localhost 9999   # Deep dive
sleep 60
echo 'DEPTH=0.5' | nc -u -w1 localhost 9999  # Return to surface

# Check /data/ for logged sensor data
```

### Scenario 3: Depth-Aware Burnwire
```bash
# Test that burnwire only heats when underwater
LD_PRELOAD=./libpigpio_sim.so /opt/ceti-tag-data-capture/bin/cetiTagApp &

# At surface - burnwire should NOT heat
echo 'DEPTH=0.5' | nc -u -w1 localhost 9999

# Underwater - burnwire should heat (if timeout reached)
echo 'DEPTH=10' | nc -u -w1 localhost 9999

# Check logs for burnwire heating events
```

## Files Reference

| File | Description |
|------|-------------|
| `qa/libpigpio_sim/libpigpio_sim.c` | LD_PRELOAD library source |
| `qa/libpigpio_sim/build/libpigpio_sim.so` | Compiled ARM64 library |
| `out/ceti-tag-data-capture_*.deb` | Firmware Debian package |
| `out/sdcard.img` | Complete bootable SD card image |
| `qa/README.md` | QA environment overview |
| `QA_TESTING_RESULTS.md` | Technical findings and lessons learned |

## Troubleshooting

### "cannot open shared object file"
- **Cause:** LD_PRELOAD path is wrong or file doesn't exist
- **Fix:** Use absolute path: `LD_PRELOAD=/usr/lib/libpigpio_sim.so`

### "Sorry, this system does not appear to be a raspberry pi"
- **Cause:** LD_PRELOAD library not loaded, real libpigpio running
- **Fix:** Verify LD_PRELOAD path is correct

### Port 9999 already in use
```bash
# Find and kill process using port 9999
lsof -i :9999
kill <PID>
```

### No sensor data being logged
- Check `/data/` directory for CSV files
- Verify firmware has write permissions
- Check system logs: `journalctl -u ceti-tag-data-capture`

## Next Steps

1. **Test on Raspberry Pi** - Most practical approach
2. **Create test scripts** - Automated test scenarios in `qa/test_scripts/`
3. **CI/CD integration** - GitHub Actions with LD_PRELOAD testing
4. **More sensors** - Add GPS, ECG simulation to LD_PRELOAD library

## Questions?

See `QA_TESTING_RESULTS.md` for technical details about our testing approaches and what we learned about LD_PRELOAD with QEMU.
