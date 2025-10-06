# Hardware Simulation Reference

This document provides a complete mapping of hardware interfaces simulated by the LD_PRELOAD library for QA testing. This serves as a "virtual lab bench" reference showing how the software interacts with physical hardware.

## I2C Devices

### I2C Bus 1 (Main Sensor Bus)

| Address | Device | Purpose | Registers/Interface |
|---------|--------|---------|---------------------|
| 0x21 | IOX - GPIO Expander | 8-pin I/O expander for power control and burnwire | See IOX Pin Mapping below |
| 0x29 | LTR-329ALS | Ambient light sensor | 16-bit lux reading |
| 0x36 | MAX17320 | Battery fuel gauge | Voltage, current, capacity registers |
| 0x40 | Keller 4LD | Pressure/depth sensor | 16-bit pressure reading (bar) |
| 0x68 | RTC | Real-time clock counter | 32-bit counter (4 bytes, registers 0-3) |
| 0x4A/0x4B | BNO086 | 9-axis IMU (via bit-bang I2C) | SHTP protocol, quaternions, accel, gyro, mag |

### IOX GPIO Expander (0x21) - Pin Mapping

The IOX is a PCA9536-compatible 8-pin GPIO expander. Critical for power management and burnwire control.

**Pin Assignments:**

| Pin | Signal Name | Direction | Purpose |
|-----|-------------|-----------|---------|
| 0 | 5V_EN | Output | Enable 5V power rail |
| 1 | BOOT0 | Output | STM32 boot mode selection |
| 2 | 3V3_RF_EN | Output | Enable 3.3V RF power (recovery board) |
| 3 | (unused) | - | Available for future use |
| 4 | BURNWIRE_ON | Output | **Burnwire activation** (depth-aware control) |
| 5 | (unused) | - | Available for future use |
| 6 | ECG_LOD_N | Input | ECG lead-off detect (negative) |
| 7 | ECG_LOD_P | Input | ECG lead-off detect (positive) |

**IOX Registers:**

| Register | Address | Purpose |
|----------|---------|---------|
| INPUT | 0x00 | Read pin states |
| OUTPUT | 0x01 | Write output pin states |
| POLARITY | 0x02 | Invert input polarity |
| CONFIGURATION | 0x03 | Set pin direction (1=input, 0=output) |
| STRENGTH_0 | 0x40 | Output drive strength (pins 0-3) |
| STRENGTH_1 | 0x41 | Output drive strength (pins 4-7) |
| INPUT_LATCH | 0x42 | Input latch enable |
| PUPD_ENABLE | 0x43 | Pull-up/pull-down enable |
| PUPD_SELECT | 0x44 | Pull-up/pull-down select |
| INTERRUPT_MASK | 0x45 | Interrupt mask |
| INTERRUPT_STATUS | 0x46 | Interrupt status |
| OUTPUT_PORT_CONFIG | 0x4F | Output port configuration |

### RTC - Real-Time Clock (0x68)

**Purpose:** Maintains a running counter for timekeeping even when main CPU is off.

**Interface:**
- 32-bit counter value
- Split across 4 byte registers (0-3)
- Little-endian format: register 0 = LSB, register 3 = MSB

**Example Read:**
```c
// Read 32-bit counter
uint32_t rtc_count = 0;
rtc_count |= i2cReadByteData(fd, 0);      // LSB
rtc_count |= i2cReadByteData(fd, 1) << 8;
rtc_count |= i2cReadByteData(fd, 2) << 16;
rtc_count |= i2cReadByteData(fd, 3) << 24; // MSB
```

**Expected Behavior:**
- Counter increments automatically
- Persists across power cycles (in real hardware)
- Used for mission timestamps

## Raspberry Pi GPIO Pins

### Direct GPIO (via pigpio)

| GPIO | Signal Name | Direction | Purpose |
|------|-------------|-----------|---------|
| 17 | RECOVERY_ON | Output | Power enable for recovery board |
| 20 | SPI_MISO (custom) | Output | Custom SPI for GPIO expander control |
| 21 | SPI_MOSI (custom) | Output | Custom SPI for GPIO expander control |
| 22 | UART_RX | Input | Serial data from recovery board |
| 23 | I2C_SDA (bit-bang) | Bidirectional | Bit-bang I2C for BNO086 IMU |
| 24 | I2C_SCL (bit-bang) | Output | Bit-bang I2C clock for BNO086 IMU |
| 26 | SPI_CLK (custom) | Output | Custom SPI clock |
| 27 | GPIO_INPUT | Input | General purpose input |

### Hardware SPI (FPGA Communication)

| Channel | Purpose | Details |
|---------|---------|---------|
| SPI0 | FPGA Control | Audio data transfer from hydrophone array |

### Hardware UART (Recovery Board)

| Device | Baud Rate | Purpose |
|--------|-----------|---------|
| /dev/ttyAMA0 | 9600 | Recovery board communication (GPS, VHF) |

## Sensor Data Formats

### Pressure Sensor (Keller 4LD @ 0x40)

**Raw Data Format:**
- 16-bit signed integer
- Range: -16384 to +16384 (centered at 16384)
- Scale: 200 bar / 32768 counts = 0.0061 bar/count

**Conversion Formula:**
```c
double scale_factor = 200.0 / 32768.0;  // PRESSURE_MAX=200, PRESSURE_MIN=0
double pressure_bar = (raw_value - 16384) * scale_factor;
```

**Depth Calculation:**
```c
// Approximate depth from pressure (1 bar = ~10m depth)
double depth_meters = (pressure_bar - 1.0) * 10.0;
```

### Battery Gauge (MAX17320 @ 0x36)

**Voltage Reading:**
- 16-bit unsigned integer
- Units: millivolts (mV)
- Range: 0-65535 mV

**Current Reading:**
- 16-bit signed integer
- Units: milliamps (mA)

**Capacity:**
- Various registers for state of charge, remaining capacity
- Refer to MAX17320 datasheet for complete register map

### Light Sensor (LTR-329ALS @ 0x29)

**Data Format:**
- 16-bit unsigned integer
- Units: lux
- Range: 0-65535 lux

**Typical Values:**
- 0 lux: Complete darkness (deep underwater)
- 100-500 lux: Dim/indoor light
- 1000+ lux: Daylight at surface

### IMU (BNO086 @ 0x4A/0x4B)

**Communication Protocol:**
- SHTP (Sensor Hub Transport Protocol)
- Bit-bang I2C on GPIO 23/24
- Complex packet structure with headers

**Sensor Data Types:**
- Quaternions (rotation)
- Linear acceleration
- Gyroscope (angular velocity)
- Magnetometer (compass heading)

## Power States & Control Sequences

### Burnwire Activation Sequence

**Controlled by:** IOX pin 4 (BURNWIRE_ON)

**Depth-Aware Behavior (PR #111):**
1. Timeout expires → Enter ST_BRN_ON state
2. While depth < threshold (default 4m):
   - Set BURNWIRE_ON = 0 (OFF)
   - Log "paused - at surface"
3. While depth ≥ threshold:
   - Set BURNWIRE_ON = 1 (ON)
   - Log "active - underwater"
   - Increment active burn time

**Purpose:** Prevents wasting burnwire heating when whale surfaces to breathe.

### Recovery Board Power (APRS On Whale - PR #112)

**Controlled by:**
- GPIO 17 (RECOVERY_ON) - Main power
- IOX pin 2 (3V3_RF_EN) - RF power

**Configuration:** `aprs_on_whale` in `/data/config/ceti-config.txt`

**Behavior:**
- `aprs_on_whale=false` (default): GPS active at surface only
- `aprs_on_whale=true`: GPS runs continuously (even underwater)

**State Machine Control:**
- ST_RECORD_SURFACE: Recovery board wakes up
- Other states: Recovery board sleeps (if aprs_on_whale=false)

## Simulation State in LD_PRELOAD

### Current Simulated State

```c
typedef struct {
    double pressure_bar;     // Pressure in bar (1.0 = surface)
    double temperature_c;    // Temperature in Celsius
    uint16_t light_lux;      // Light level in lux
    double battery_voltage;  // Battery voltage in volts
    int gpio_states[32];     // GPIO pin states
} SimState;
```

### Runtime Control (UDP Port 9999)

**Commands:**
```bash
# Simulate depth changes
echo 'DEPTH=10' | nc -u -w1 127.0.0.1 9999    # 10m deep (2.0 bar)
echo 'DEPTH=0.5' | nc -u -w1 127.0.0.1 9999   # Surface (1.05 bar)

# Environmental conditions
echo 'TEMP=15' | nc -u -w1 127.0.0.1 9999     # 15°C water
echo 'LIGHT=0' | nc -u -w1 127.0.0.1 9999     # Darkness (underwater)
echo 'LIGHT=1000' | nc -u -w1 127.0.0.1 9999  # Daylight (surface)

# Battery state
echo 'BATTERY=3.5' | nc -u -w1 127.0.0.1 9999 # Low battery (3.5V)
echo 'BATTERY=4.2' | nc -u -w1 127.0.0.1 9999 # Full charge (4.2V)
```

## Implementation Status

### ✅ Fully Implemented

#### 1. FPGA Bitstream Loading (Fast-Forward)
**Status:** ✅ Complete
**Performance:** Reduced from 30min-54hr to <1 second
**Implementation:** libpigpio_sim.c:133-176

**Details:**
- Detects FPGA programming pattern on GPIO 20/21 (FPGA_DATA, FPGA_CLOCK)
- Skips 19.4 million operations after 100 samples
- Returns FPGA_DONE=HIGH (GPIO 27) to signal completion
- Logs progress every 100 operations

**Testing:**
```bash
# Check logs for fast-forward
grep "FPGA programming complete" test.log
# Expected: "Operations: 1000 (skipped 19442840)"
```

#### 2. RTC Counter (0x68)
**Status:** ✅ Complete
**Implementation:** libpigpio_sim.c:54-63, 109-120, 213-282

**Details:**
- 32-bit Unix timestamp counter
- Background thread increments every second
- Little-endian format across registers 0-3
- Initialized with `time(NULL)` at startup

**Testing:**
```bash
# Check RTC initialization
grep "RTC counter initialized" test.log
# Expected: "[LD_PRELOAD] RTC counter initialized to 1234567890"
```

#### 3. IOX GPIO Expander (0x21)
**Status:** ✅ Complete
**Implementation:** libpigpio_sim.c:113-116, 284-322

**Details:**
- Full register state management (INPUT, OUTPUT, CONFIGURATION, etc.)
- Tracks output pin writes (especially BURNWIRE_ON on pin 4)
- Returns proper values on register reads
- Logs OUTPUT register writes with burnwire state

**Testing:**
```bash
# Check burnwire activation
grep "IOX OUTPUT register" test.log
# Expected: "IOX OUTPUT register: 0x10 (BURNWIRE_ON=1)"
```

#### 4. Pressure Sensor - Keller 4LD (0x40)
**Status:** ✅ Complete
**Implementation:** libpigpio_sim.c:214-278

**Details:**
- 5-byte MSR protocol response (status + 16-bit pressure + 16-bit temperature)
- Supports both `i2cReadDevice()` (multi-byte) and `i2cReadByteData()` (single-byte)
- Converts pressure_bar to raw 16-bit value using scale factor
- Formula: `raw = (pressure_bar / scale_factor) + 16384`

**Sensor Datasheet:** [Keller 4LD FXP3.pdf](https://www.msr.ch/media/pdf/sensors/FXP3.pdf)

**Testing:**
```bash
# Check pressure reads
grep "i2cReadDevice.*addr=0x40" test.log | head -5
# Expected: Regular 5-byte reads from pressure sensor
```

#### 5. bbI2CZip Protocol Parser (BNO086 IMU)
**Status:** ✅ Complete
**Implementation:** libpigpio_sim.c:410-509

**Details:**
- Full command parser for bit-bang I2C protocol
- Supports: START, STOP, SET_ADDR, READ, WRITE, ESCAPE
- Returns "no data" response (length=0) to prevent busy-waiting
- Tracks IMU read count for debugging

**Command Types:**
- 0x00: END
- 0x02: START
- 0x03: STOP
- 0x04: SET_ADDR
- 0x06: READ bytes
- 0x07: WRITE bytes

**Testing:**
```bash
# Check IMU initialization
grep "bbI2CZip.*IMU" test.log | head -10
# Expected: Regular header reads with "No data available" responses
```

#### 6. Sensor Initialization Defaults
**Status:** ✅ Complete
**Implementation:** Sensor defaults set BEFORE firmware starts reading

**Default Values (set in `gpioInitialise()`):**
- Pressure: 1.0 bar (surface)
- Temperature: 20.0°C
- Light: 100 lux (moderate light)
- Battery: 3.7V (nominal LiPo voltage)

**Rationale:**
- Ensures firmware sees valid data from first read
- Prevents undefined behavior during initialization
- Allows UDP commands to override values later

### ✅ Fully Simulated (Basic Implementation)

#### Battery Gauge - MAX17320 (0x36)
**Status:** ✅ Basic simulation complete
**Implementation:** libpigpio_sim.c:252-263, 339-341

**Details:**
- Returns simulated voltage in mV
- 16-bit unsigned integer
- Supports both byte and word reads

**Limitations:**
- Only voltage register simulated
- No current, capacity, or state-of-charge registers
- Sufficient for basic firmware operation

#### Light Sensor - LTR-329ALS (0x29)
**Status:** ✅ Basic simulation complete
**Implementation:** libpigpio_sim.c:265-271

**Details:**
- Returns 16-bit lux value
- Supports both byte and word reads
- Controlled via UDP: `LIGHT=<lux>`

### ⚠️ Stubbed (Returns Safe Defaults)

#### Serial Port - Recovery Board
**Status:** ⚠️ Stubbed
**Implementation:** libpigpio_sim.c:367-393

**Details:**
- `serOpen()`: Returns fake handle
- `serDataAvailable()`: Returns 0 (no data)
- `serRead()`: Returns 0 bytes
- `serWrite()`: Pretends success

**Impact:**
- Recovery board communication not simulated
- GPS NMEA sentences not available
- APRS transmissions not tested
- Sufficient for basic state machine testing

**Future Enhancement:**
- Simulate GPS NMEA sentences
- Implement VHF radio responses
- Add command/response protocol

#### SPI - FPGA Audio
**Status:** ⚠️ Stubbed
**Implementation:** libpigpio_sim.c:512-571

**Details:**
- `spiOpen()`: Returns fake handle
- `spiRead()`: Returns zeros (no audio data)
- `spiWrite()`: Pretends success
- `spiXfer()`: Returns zeros

**Impact:**
- No audio data captured during tests
- Audio threads may run but produce empty files
- Sufficient for state machine and sensor testing

**Future Enhancement:**
- Simulate synthetic audio samples (sine wave, noise)
- Generate realistic hydrophone data
- Test FLAC compression

### 🎯 Testing Insights

#### Critical Discovery: Firmware Logging System

**The firmware IS running, but logs are hidden!**

The firmware uses `syslog()` for all `CETI_LOG()` output, which means:
- ✅ LD_PRELOAD output is visible (sensor reads, I2C, GPIO)
- ❌ Firmware logs are NOT in stdout/stderr
- ❌ State machine transitions are NOT visible in test output

**Where firmware logs go:**
- **Bookworm (current):** `/data/journal/` (systemd-journald)
- **Bullseye (upstream):** `/var/log/syslog` (rsyslog)

**How to access logs:**
```bash
# During test (attach to container)
docker exec -it <container_id> journalctl -u ceti-tag-data-capture -f

# After test (mount /data partition)
sudo journalctl --directory=/mnt/img/data/journal/
```

See **qa/QA_TESTING_GUIDE.md** for complete logging access instructions.

#### Evidence of Firmware Operation

Even without visible `CETI_LOG()` output, we can confirm firmware is running by observing:

1. **Sensor polling patterns** - Regular I2C reads indicate active threads:
   ```
   [LD_PRELOAD] i2cReadDevice(handle=.../addr=0x40, count=5)  # Pressure sensor
   [LD_PRELOAD] i2cReadByteData(handle=.../addr=0x21, reg=0x00)  # IOX
   [LD_PRELOAD] bbI2CZip(SDA=2, inLen=..., outLen=4)  # IMU header read
   ```

2. **IOX register updates** - OUTPUT register changes show state machine activity:
   ```
   [LD_PRELOAD] IOX OUTPUT register: 0x10 (BURNWIRE_ON=1)
   ```

3. **Pressure responses** - Measurement triggers show depth monitoring working:
   ```
   [LD_PRELOAD] i2cReadDevice(handle=140/addr=0x40, count=5)
   ```

4. **IMU read count** - Incrementing counter shows continuous IMU thread operation:
   ```
   [LD_PRELOAD] IMU: No data available (read #27354)
   ```

5. **CSV file creation** - Data files appear in `/mnt/img/data/`:
   - `data_pressure_temperature.csv`
   - `data_battery.csv`
   - `data_state.csv`
   - `data_burnwire.csv`

#### Debugging Strategy

When testing firmware:

1. **Check LD_PRELOAD output** - Confirms hardware interface calls are working
2. **Access journald logs** - See actual firmware state machine and CETI_LOG output
3. **Inspect CSV files** - Verify data is being written correctly
4. **Monitor IOX OUTPUT** - Track burnwire activation state
5. **Count sensor reads** - Ensure threads are running continuously

## Missing Simulations (To Be Implemented)

### None - All Critical Hardware Simulated!

All hardware interfaces required for basic firmware operation are now implemented:
- ✅ RTC counter with background thread
- ✅ IOX GPIO expander with register state
- ✅ Pressure sensor with MSR protocol
- ✅ IMU with bbI2CZip parser
- ✅ FPGA fast-forward
- ✅ Battery, light sensors
- ⚠️ Serial and SPI stubbed (safe defaults)

**Future Enhancements (optional):**
- Serial port: GPS NMEA simulation, VHF radio
- SPI: Synthetic audio data generation

## Hardware Versions

The tag has evolved through several hardware revisions:

| Version | Platform | Key Differences |
|---------|----------|-----------------|
| v0 | RPi Zero W | OctoBoard soundcard, basic sensors |
| v2 | RPi Zero W | Custom bonnets, FPGA hydrophones, IOX expander |
| v2_2 | RPi Zero 2 W | ARM64 quad-core, electrical/mechanical updates |

**Note:** LD_PRELOAD simulation targets v2_2 (current deployment hardware).

## Testing Scenarios

### Typical Dive Profile Simulation

```bash
# Tag deployment
echo 'DEPTH=0.5' | nc -u -w1 127.0.0.1 9999
echo 'LIGHT=1000' | nc -u -w1 127.0.0.1 9999
sleep 30

# Whale dives
echo 'DEPTH=5' | nc -u -w1 127.0.0.1 9999
echo 'LIGHT=100' | nc -u -w1 127.0.0.1 9999
sleep 60

# Deep dive
echo 'DEPTH=15' | nc -u -w1 127.0.0.1 9999
echo 'LIGHT=0' | nc -u -w1 127.0.0.1 9999
sleep 120

# Return to surface
echo 'DEPTH=0.5' | nc -u -w1 127.0.0.1 9999
echo 'LIGHT=800' | nc -u -w1 127.0.0.1 9999
```

### Burnwire Depth-Aware Testing

```bash
# Set short timeout for testing
# Edit /data/config/ceti-config.txt: timeout_s=30

# Wait for timeout to trigger
sleep 35

# Simulate surface (burnwire should pause)
echo 'DEPTH=0.5' | nc -u -w1 127.0.0.1 9999
# Check logs: burnwire should be OFF

# Simulate underwater (burnwire should activate)
echo 'DEPTH=10' | nc -u -w1 127.0.0.1 9999
# Check logs: burnwire should be ON

# Expected in data_burnwire.csv:
# timestamp, depth, burnwire_state, active_burn_time
```

## References

**Source Code Locations:**
- IOX driver: `packages/ceti-tag-data-capture/src/cetiTagApp/device/iox.c`
- RTC driver: `packages/ceti-tag-data-capture/src/cetiTagApp/device/rtc.c`
- Pressure sensor: `packages/ceti-tag-data-capture/src/cetiTagApp/sensors/keller4ld.c`
- Battery gauge: `packages/ceti-tag-data-capture/src/cetiTagApp/battery.c`
- State machine: `packages/ceti-tag-data-capture/src/cetiTagApp/state_machine.c`

**Hardware Datasheets:**
- PCA9536 GPIO Expander (IOX)
- Keller 4LD Pressure Sensor
- MAX17320 Battery Fuel Gauge
- BNO086 IMU
- LTR-329ALS Light Sensor

**Related Documentation:**
- `qa/QA_TESTING_GUIDE.md` - Complete testing procedures
- `qa/libpigpio_sim/README.md` - LD_PRELOAD library implementation
- `RUNTIME_ARCHITECTURE.md` - Firmware state machine and data formats
