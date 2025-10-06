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

## Missing Simulations (To Be Implemented)

### RTC Counter Simulation

**Status:** ❌ Not implemented
**Impact:** Firmware may be waiting for RTC initialization
**Required Implementation:**
- Maintain a running 32-bit counter
- Increment counter in background thread (1 Hz or similar)
- Return counter value when registers 0-3 are read

### IOX Register Simulation

**Status:** ⚠️ Partially implemented (returns 0)
**Impact:** Firmware may be stuck waiting for IOX status
**Required Implementation:**
- Maintain register state (INPUT, OUTPUT, CONFIGURATION, etc.)
- Track output pin states written by firmware
- Return appropriate values when registers are read
- Simulate pin state changes

### Serial Port (Recovery Board)

**Status:** ⚠️ Stubbed (returns no data)
**Impact:** Recovery board communication not simulated
**Required Implementation:**
- Simulate GPS NMEA sentences
- Simulate VHF radio responses
- Implement command/response protocol

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
