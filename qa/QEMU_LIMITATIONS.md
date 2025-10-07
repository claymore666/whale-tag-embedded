# QEMU User-Mode Limitations for Whale Tag Firmware Testing

## Problem Summary

When testing whale tag firmware using QEMU user-mode emulation in Docker, we discovered that **QEMU user-mode cannot write files to the filesystem**, even though the firmware runs and hardware simulation works correctly.

## Symptoms

1. **Firmware runs successfully**: Process is alive, consuming CPU, reading sensors via LD_PRELOAD
2. **Hardware simulation works**: LD_PRELOAD library intercepts all pigpio calls correctly
3. **No CSV files created**: Firmware expects to write to `/data/*.csv` but files never appear
4. **Directory permissions look correct**: `/data` exists with 777 permissions from host perspective
5. **QEMU cannot access directories**: From QEMU's perspective, `/data` shows as `d?????????` with inaccessible permissions

## Root Cause

**QEMU user-mode emulation has fundamental limitations with filesystem access when running inside Docker containers with complex mount configurations.**

### Technical Details

1. **Mount Point Isolation**: QEMU user-mode with `-L /mnt/img` creates a chroot-like environment, but mount points created AFTER the image is mounted are not visible or writable from QEMU's perspective

2. **File Descriptor Limitations**: Even directories that exist in the base filesystem and have correct permissions cannot be written to by QEMU

3. **Test Results**:
   ```bash
   # From host - works fine:
   docker exec container touch /mnt/img/data/test.txt  # SUCCESS

   # From QEMU - fails:
   docker exec container qemu-aarch64-static -L /mnt/img /mnt/img/bin/touch /data/test.txt
   # Error: cannot touch '/data/test.txt': No such file or directory

   # QEMU sees directory as inaccessible:
   docker exec container qemu-aarch64-static -L /mnt/img /mnt/img/bin/ls -la /
   # Output: d?????????   ? ?    ?       ?            ? data
   ```

## What We Tried (All Failed)

### Attempt 1: Direct Mount of Data Partition
```bash
mount /dev/mapper/loop31p2 /mnt/img          # rootfs
mount /dev/mapper/loop31p3 /mnt/img/data     # data partition
```
**Result**: QEMU cannot write to mounted partition

### Attempt 2: Bind Mount
```bash
mount /dev/mapper/loop31p3 /mnt/data_partition
mount --bind /mnt/data_partition /mnt/img/data
```
**Result**: QEMU still cannot write to bind-mounted directory

### Attempt 3: No Mount, Just Directory
```bash
mount /dev/mapper/loop31p2 /mnt/img
mkdir -p /mnt/img/data
chmod 777 /mnt/img/data
```
**Result**: QEMU cannot write even to regular directory with 777 permissions

### Attempt 4: World-Writable Permissions
```bash
chmod 777 /mnt/img/data
```
**Result**: Permissions look correct from host, but QEMU still sees `?????????`

## Firmware Behavior

The firmware runs normally despite being unable to write files:

- **Hardware polling works**: Continuously reads sensors (IMU, pressure, battery)
- **LD_PRELOAD interception works**: All pigpio calls are simulated correctly
- **State machine runs**: Firmware progresses through initialization
- **Silent failure**: No error messages visible because CETI_LOG uses syslog (not stderr)

Example firmware activity:
```
[LD_PRELOAD] Pressure sensor response: 1.00 bar, 20.0°C (status=0x40)
[LD_PRELOAD] bbI2CZip(SDA=23, inLen=9, outLen=4)
[LD_PRELOAD]   IMU: No data available (read #51279)
[LD_PRELOAD] IOX OUTPUT register: 0x01 (BURNWIRE_ON=0)
```

## Why This Matters

The whale tag firmware hardcodes data paths:
```c
#define STATEMACHINE_DATA_FILEPATH "/data/data_state.csv"
#define STATEMACHINE_BURNWIRE_TIMEOUT_START_TIME_FILEPATH "/data/burnwire_timeout_start_time_s.csv"
```

Without the ability to write to `/data/`, we cannot:
- Verify state machine transitions (`data_state.csv`)
- Test burnwire depth-aware behavior (`data_burnwire.csv`)
- Check sensor data logging (`data_pressure_temperature.csv`, etc.)

## Architectural Context

### Real Hardware (Raspberry Pi)
1. Systemd boots the system
2. `/etc/fstab` mounts partition 3 to `/data` at boot:
   ```
   /dev/disk/by-label/cetiData /data ext4 defaults,nofail 0 0
   ```
3. Firmware writes to `/data/` which resolves to mounted partition
4. All works correctly

### QEMU Test Environment
1. No systemd - just running single ARM64 binary with QEMU
2. `-L /mnt/img` makes QEMU treat `/mnt/img` as root filesystem
3. No fstab processing, no automatic mounts
4. `/data` directory exists but is somehow inaccessible to QEMU

## Configuration Used

```bash
# Host: Map SD card image partitions
kpartx -av out/sdcard.img
LOOP_DEV=loop31

# Docker: Start privileged container with /dev bound
docker run -d --name test --privileged -v "/dev:/dev" -v "$(pwd):/work" debian:bookworm

# Container: Mount rootfs
mount /dev/mapper/${LOOP_DEV}p2 /mnt/img
mkdir -p /mnt/img/data
chmod 777 /mnt/img/data

# Container: Run firmware with QEMU
qemu-aarch64-static -L /mnt/img \
  -E LD_PRELOAD=/work/qa/libpigpio_sim/build/libpigpio_sim_arm64.so \
  /mnt/img/opt/ceti-tag-data-capture/bin/cetiTagApp
```

## Potential Solutions (Not Yet Tested)

1. **Full System Emulation**: Use `qemu-system-aarch64` instead of `qemu-aarch64-static`
   - Boots full kernel and systemd
   - Processes fstab correctly
   - Much slower startup, more complex setup

2. **Modify Firmware**: Recompile with different data path
   - Not ideal - changes production code for testing

3. **strace Analysis**: Use strace to see exactly what system calls fail
   ```bash
   qemu-aarch64-static -L /mnt/img -strace /mnt/img/opt/ceti-tag-data-capture/bin/cetiTagApp
   ```

4. **Docker Capabilities**: Add specific Linux capabilities
   ```bash
   docker run --cap-add=ALL --security-opt seccomp=unconfined ...
   ```

5. **Different Mount Strategy**: Copy image contents to temporary directory instead of mounting
   - Requires more disk space
   - Slower setup

## Comparison with Simple E2E Test

The `qa/e2e-test.sh` script works for basic verification but:
- Only tests that firmware starts and LD_PRELOAD works
- Does not verify file output
- Does not test state machine progression
- Cannot validate depth-aware burnwire feature

## ⚠️ CRITICAL: Config File Path Mismatch

**Problem:** The test script writes config to `/mnt/img/data/config/ceti-config.txt` but firmware reads from `/data/config/ceti-config.txt` which gets redirected to `/tmp/qemu_data/config/ceti-config.txt`.

**Root Cause:** Two separate filesystems:
1. `/mnt/img/data/` - Mounted SD card image (where test script writes config)
2. `/tmp/qemu_data/` - LD_PRELOAD redirection target (where firmware reads)

**Impact:** Firmware loads default config values instead of test parameters:
- `timeout_s` defaults to large value instead of test value (10s)
- `burn_interval_s` defaults instead of 60s test value
- Burnwire never triggers during test because timeout never expires

**Solution:** Test script must copy config to BOTH locations with CORRECT parameter names:
```bash
# Write to mount point (for reference/debugging)
docker exec $CONTAINER_NAME bash -c "
    mkdir -p /mnt/img/data/config
    cat > /mnt/img/data/config/ceti-config.txt <<EOF
# NOTE: Config parser expects specific names (see config.c line 89):
timeout_release=10s    # NOT timeout_s; 's' suffix required (defaults to MINUTES!)
burn_interval=60s      # NOT burn_interval_s; 's' suffix required
burn_depth_threshold=4.0
surface_pressure=0.3
EOF
"

# ALSO write to LD_PRELOAD redirection target (for firmware to actually read)
docker exec $CONTAINER_NAME bash -c "
    mkdir -p /tmp/qemu_data/config
    cat > /tmp/qemu_data/config/ceti-config.txt <<EOF
timeout_release=10s
burn_interval=60s
burn_depth_threshold=4.0
surface_pressure=0.3
EOF
"
```

**CRITICAL #1:** The config parser (config.c:89) expects `timeout_release` and `burn_interval`, NOT `timeout_s` and `burn_interval_s`. Using wrong names causes firmware to use default values (timeout_release = 345600s = 96 hours!).

**CRITICAL #2:** The parser uses `strtotime_s()` (config.c:376) which supports unit suffixes (s/m/h/d) but **defaults to MINUTES if no unit specified!** Always append 's' for seconds: `timeout_release=10s` (not `timeout_release=10` which means 10 minutes = 600 seconds).

**Discovery:** Found by checking elapsed time vs timeout and examining config snapshot. Value of 600s in snapshot revealed parser interpreted `timeout_release=10` as 10 minutes (default unit) instead of 10 seconds.

## ✅ SOLUTION IMPLEMENTED: LD_PRELOAD File I/O Redirection

**Status**: WORKING - Problem solved!

### Implementation

We solved the QEMU user-mode filesystem limitation by extending the existing LD_PRELOAD library to intercept file I/O calls and redirect `/data/*` paths to `/tmp/qemu_data/*`.

### Code Changes

Added to `qa/libpigpio_sim/libpigpio_sim.c`:

```c
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <fcntl.h>

// Redirect /data/* paths to /tmp/qemu_data/* where QEMU can write
static void redirect_path(const char* original, char* redirected, size_t size) {
    if (strncmp(original, "/data/", 6) == 0) {
        const char* filename = original + 6;
        snprintf(redirected, size, "/tmp/qemu_data/%s", filename);

        static int dir_created = 0;
        if (!dir_created) {
            system("mkdir -p /tmp/qemu_data");
            dir_created = 1;
        }
    } else {
        snprintf(redirected, size, "%s", original);
    }
}

FILE* fopen(const char* path, const char* mode) {
    static FILE* (*real_fopen)(const char*, const char*) = NULL;
    if (!real_fopen) real_fopen = dlsym(RTLD_NEXT, "fopen");

    char redirected[512];
    redirect_path(path, redirected, sizeof(redirected));
    return real_fopen(redirected, mode);
}
```

### How It Works

1. **Interception**: LD_PRELOAD intercepts `fopen()` and `open()` before libc
2. **Path Translation**: `/data/data_state.csv` → `/tmp/qemu_data/data_state.csv`
3. **Transparent**: Firmware unchanged - still writes to `/data/*`
4. **QEMU Compatible**: `/tmp` is accessible to QEMU user-mode

### Verification

```bash
# Log shows redirection:
[LD_PRELOAD] File I/O redirected: /data/* -> /tmp/qemu_data/*
[LD_PRELOAD] fopen('/data/data_state.csv', 'at') -> '/tmp/qemu_data/data_state.csv'

# Files created successfully:
$ docker exec container ls /tmp/qemu_data/
data_state.csv              # State machine transitions
data_burnwire.csv          # Burnwire events
data_battery.csv           # Battery data
data_pressure_temperature.csv  # Depth/temp data
# ... 15+ CSV files total
```

### Results

**Before fix:**
- ❌ No CSV files created
- ❌ Cannot verify state machine
- ❌ Silent failure

**After fix:**
- ✅ All CSV files created
- ✅ State transitions logged: CONFIG → START → RECORD_DIVING → BRN_ON → SHUTDOWN
- ✅ Depth simulation working
- ✅ Full end-to-end testing possible

### Test Script Updates

```bash
# Collect files from redirected location:
docker exec container cat /tmp/qemu_data/data_state.csv
docker exec container cat /tmp/qemu_data/data_burnwire.csv
```

### Advantages

- **Non-invasive**: No firmware changes
- **Transparent**: Firmware unaware of redirection
- **Fast**: Minimal overhead, `/tmp` is RAM-backed
- **Simple**: Works with existing QEMU setup

## Current Status

**✅ RESOLVED**: End-to-end testing now fully functional!
- ✅ State machine transitions verified
- ✅ Burnwire event logging working
- ✅ CSV file creation successful
- ✅ Depth simulation confirmed
- ✅ Complete behavioral verification possible

## References

- QEMU User-Mode Documentation: https://www.qemu.org/docs/master/user/main.html
- Known issue: QEMU user-mode has limited filesystem access compared to full system emulation
- Docker + QEMU + mounts is a complex environment with interaction issues
