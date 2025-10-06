# LD_PRELOAD Simulation Library

This library simulates pigpio hardware functions for testing whale tag firmware without real sensors.

## Features

- **GPIO simulation**: Intercepts gpioInitialise, gpioWrite, gpioRead, etc.
- **I2C simulation**: Simulates sensor responses for:
  - Keller 4LD pressure sensor (0x40)
  - MAX17320 battery gauge (0x36)
  - LTR-329ALS light sensor (0x29)
  - BNO086 IMU (bit-bang I2C)
- **SPI simulation**: Stubs for FPGA/audio interface
- **Network control**: UDP port 9999 for remote sensor value updates

## Building

```bash
make
```

This creates `build/libpigpio_sim.so`.

## Usage

### Basic Usage

```bash
LD_PRELOAD=./build/libpigpio_sim.so /opt/ceti-tag-data-capture/bin/cetiTagApp
```

### Network Control

The library listens on UDP port 9999 for sensor control commands:

```bash
# Set depth to 10.5 meters (11.05 bar pressure)
echo 'DEPTH=10.5' | nc -u -w1 localhost 9999

# Set temperature to 15°C
echo 'TEMP=15.0' | nc -u -w1 localhost 9999

# Set light level to 500 lux
echo 'LIGHT=500' | nc -u -w1 localhost 9999

# Set battery voltage to 3.6V
echo 'BATTERY=3.6' | nc -u -w1 localhost 9999
```

### Testing Dive Scenarios

```bash
# Terminal 1: Run firmware with LD_PRELOAD
LD_PRELOAD=./build/libpigpio_sim.so /opt/ceti-tag-data-capture/bin/cetiTagApp

# Terminal 2: Simulate a dive
echo 'DEPTH=0' | nc -u -w1 localhost 9999     # Surface
sleep 5
echo 'DEPTH=10' | nc -u -w1 localhost 9999    # 10m depth
sleep 30
echo 'DEPTH=0' | nc -u -w1 localhost 9999     # Surface
```

## Simulated Sensors

### Pressure (Keller 4LD at 0x40)

- Formula: `pressure_bar = 1.0 + (depth_m / 10.0)`
- 1 bar = surface (0m)
- 2 bar = 10m depth
- 200 bar = 1990m depth (max range)

### Battery (MAX17320 at 0x36)

- Returns voltage in millivolts (16-bit)
- Default: 3.7V (3700mV)

### Light (LTR-329ALS at 0x29)

- Returns lux value (16-bit)
- Default: 1000 lux (daylight)
- 0 lux = darkness (underwater)

### IMU (BNO086 bit-bang I2C)

- Returns fake SHTP protocol responses
- Prevents firmware from hanging on IMU initialization

## Default Values

| Sensor | Parameter | Default | Range |
|--------|-----------|---------|-------|
| Pressure | Depth | 0m (1 bar) | 0-1990m |
| Temperature | Temp | 20°C | -40 to 85°C |
| Light | Lux | 1000 | 0-65535 |
| Battery | Voltage | 3.7V | 0-5V |

## Logging

All intercepted function calls are logged to stderr with `[LD_PRELOAD]` prefix:

```
[LD_PRELOAD] gpioInitialise() - Starting simulation
[LD_PRELOAD] Network control listening on UDP port 9999
[LD_PRELOAD] i2cOpen(bus=1, addr=0x40, flags=0)
[LD_PRELOAD] Set depth to 10.0m (2.00 bar)
[LD_PRELOAD] i2cReadDevice(handle=140/addr=0x40, count=2)
```

## Limitations

This simulation library provides:
- ✅ Software logic testing
- ✅ State machine validation
- ✅ Config parsing
- ✅ Integration testing

It does NOT simulate:
- ❌ Real hardware timing
- ❌ I2C bus errors
- ❌ Power consumption
- ❌ FPGA behavior
- ❌ Actual audio data

## Development

To add new sensors or commands:

1. Edit `libpigpio_sim.c`
2. Add sensor state to `SimState` struct
3. Add I2C address case in `i2cReadDevice()`
4. Add network command in `network_control_thread()`
5. Rebuild with `make`
