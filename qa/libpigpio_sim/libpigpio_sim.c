/*
 * libpigpio_sim.c - LD_PRELOAD simulation library for whale tag testing
 *
 * This library intercepts pigpio function calls and simulates hardware behavior
 * without requiring real sensors or GPIO hardware.
 *
 * Also intercepts file I/O to work around QEMU user-mode filesystem limitations.
 *
 * Usage:
 *   LD_PRELOAD=./libpigpio_sim.so /opt/ceti-tag-data-capture/bin/cetiTagApp
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <libgen.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>

// Simulated sensor state
typedef struct {
    double pressure_bar;     // Pressure in bar (depth simulation)
    double temperature_c;    // Temperature in Celsius
    uint16_t light_lux;      // Light level in lux
    double battery_voltage;  // Battery voltage
    int gpio_states[32];     // GPIO pin states
    uint32_t rtc_counter;    // RTC counter (Unix timestamp)
    uint8_t iox_registers[0x50];  // IOX GPIO expander registers
} SimState;

static SimState g_sim_state = {
    .pressure_bar = 1.0,      // 1 bar = surface
    .temperature_c = 20.0,    // 20°C
    .light_lux = 1000,        // 1000 lux (daylight)
    .battery_voltage = 3.7,   // 3.7V (typical Li-ion)
    .rtc_counter = 0,         // Initialized at startup with time()
};

static pthread_mutex_t g_sim_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_udp_socket = -1;
static int g_gpio_initialized = 0;
static int g_bbi2c_open = 0;  // Track if bit-bang I2C is open
static uint8_t g_bbi2c_addr = 0;  // Current I2C address for bit-bang operations
static int g_imu_read_count = 0;  // Track number of IMU reads to send init packet once

// FPGA bitstream loading simulation
static int g_fpga_loading = 0;        // Track if FPGA bitstream loading is in progress
static int g_fpga_operation_count = 0; // Count GPIO operations during FPGA loading
static int g_fpga_done = 0;           // Simulate FPGA_DONE signal

// Pressure sensor simulation
static uint8_t g_pressure_cmd_pending = 0;  // Track if measurement command was sent

// RTC counter thread - increments counter every second
static void* rtc_counter_thread(void* arg) {
    fprintf(stderr, "[LD_PRELOAD] RTC counter thread started at %u\n", g_sim_state.rtc_counter);
    while (1) {
        sleep(1);
        pthread_mutex_lock(&g_sim_mutex);
        g_sim_state.rtc_counter++;
        pthread_mutex_unlock(&g_sim_mutex);
    }
    return NULL;
}

// Network control (UDP port 9999)
static void* network_control_thread(void* arg) {
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    char buffer[256];

    g_udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_socket < 0) {
        fprintf(stderr, "[LD_PRELOAD] Failed to create UDP socket\n");
        return NULL;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(9999);

    if (bind(g_udp_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        fprintf(stderr, "[LD_PRELOAD] Failed to bind UDP socket to port 9999\n");
        close(g_udp_socket);
        return NULL;
    }

    fprintf(stderr, "[LD_PRELOAD] Network control listening on UDP port 9999\n");
    fprintf(stderr, "[LD_PRELOAD] Commands: DEPTH=X.X, TEMP=X.X, LIGHT=XXX, BATTERY=X.X\n");

    while (1) {
        ssize_t n = recvfrom(g_udp_socket, buffer, sizeof(buffer)-1, 0,
                            (struct sockaddr*)&client_addr, &client_len);
        if (n > 0) {
            buffer[n] = '\0';

            pthread_mutex_lock(&g_sim_mutex);

            if (strncmp(buffer, "DEPTH=", 6) == 0) {
                g_sim_state.pressure_bar = 1.0 + (atof(buffer + 6) / 10.0);  // depth in meters
                fprintf(stderr, "[LD_PRELOAD] Set depth to %.1fm (%.2f bar)\n",
                        atof(buffer + 6), g_sim_state.pressure_bar);
            }
            else if (strncmp(buffer, "TEMP=", 5) == 0) {
                g_sim_state.temperature_c = atof(buffer + 5);
                fprintf(stderr, "[LD_PRELOAD] Set temperature to %.1f°C\n", g_sim_state.temperature_c);
            }
            else if (strncmp(buffer, "LIGHT=", 6) == 0) {
                g_sim_state.light_lux = atoi(buffer + 6);
                fprintf(stderr, "[LD_PRELOAD] Set light to %d lux\n", g_sim_state.light_lux);
            }
            else if (strncmp(buffer, "BATTERY=", 8) == 0) {
                g_sim_state.battery_voltage = atof(buffer + 8);
                fprintf(stderr, "[LD_PRELOAD] Set battery to %.2fV\n", g_sim_state.battery_voltage);
            }

            pthread_mutex_unlock(&g_sim_mutex);
        }
    }

    return NULL;
}

// pigpio simulation functions

int gpioInitialise(void) {
    if (g_gpio_initialized) {
        return 0;  // Already initialized
    }

    fprintf(stderr, "[LD_PRELOAD] gpioInitialise() - Starting simulation\n");

    // Initialize all sensor values with realistic defaults BEFORE firmware starts
    // This ensures firmware sees valid data from the very first read
    g_sim_state.pressure_bar = 1.0;          // Surface pressure (1 bar = ~0m depth)
    g_sim_state.temperature_c = 20.0;        // Room temperature
    g_sim_state.light_lux = 100;             // Moderate light
    g_sim_state.battery_voltage = 3.7;       // Nominal LiPo voltage
    memset(g_sim_state.gpio_states, 0, sizeof(g_sim_state.gpio_states));

    fprintf(stderr, "[LD_PRELOAD] Sensors initialized: %.1f bar, %.1f°C, %u lux, %.1fV\n",
            g_sim_state.pressure_bar, g_sim_state.temperature_c,
            g_sim_state.light_lux, g_sim_state.battery_voltage);

    // Initialize RTC counter with current Unix timestamp
    g_sim_state.rtc_counter = (uint32_t)time(NULL);
    fprintf(stderr, "[LD_PRELOAD] RTC counter initialized to %u\n", g_sim_state.rtc_counter);

    // Initialize IOX registers to default state
    memset(g_sim_state.iox_registers, 0, sizeof(g_sim_state.iox_registers));
    g_sim_state.iox_registers[0x03] = 0xFF;  // CONFIGURATION: all pins input by default

    // Start RTC counter thread
    pthread_t rtc_thread;
    pthread_create(&rtc_thread, NULL, rtc_counter_thread, NULL);
    pthread_detach(rtc_thread);

    // Start network control thread
    pthread_t net_thread;
    pthread_create(&net_thread, NULL, network_control_thread, NULL);
    pthread_detach(net_thread);

    g_gpio_initialized = 1;
    return 0;  // Success
}

void gpioTerminate(void) {
    fprintf(stderr, "[LD_PRELOAD] gpioTerminate()\n");
    if (g_udp_socket >= 0) {
        close(g_udp_socket);
        g_udp_socket = -1;
    }
    g_gpio_initialized = 0;
}

int gpioSetMode(unsigned gpio, unsigned mode) {
    fprintf(stderr, "[LD_PRELOAD] gpioSetMode(gpio=%u, mode=%u)\n", gpio, mode);
    return 0;  // Success
}

int gpioWrite(unsigned gpio, unsigned level) {
    // FPGA bitstream loading fast-forward
    // GPIO 20 = FPGA_DATA, GPIO 21 = FPGA_CLOCK
    if ((gpio == 20 || gpio == 21) && !g_fpga_loading) {
        g_fpga_loading = 1;
        fprintf(stderr, "[LD_PRELOAD] ========================================\n");
        fprintf(stderr, "[LD_PRELOAD] FPGA bitstream loading detected!\n");
        fprintf(stderr, "[LD_PRELOAD] Fast-forwarding FPGA initialization...\n");
        fprintf(stderr, "[LD_PRELOAD] ========================================\n");
    }

    if (g_fpga_loading && (gpio == 20 || gpio == 21)) {
        g_fpga_operation_count++;

        // After 1000 operations, signal completion and skip further GPIO writes
        if (g_fpga_operation_count >= 1000) {
            if (!g_fpga_done) {
                g_fpga_done = 1;
                fprintf(stderr, "[LD_PRELOAD] ========================================\n");
                fprintf(stderr, "[LD_PRELOAD] FPGA programming complete (simulated)\n");
                fprintf(stderr, "[LD_PRELOAD] Operations: %d (skipped %d)\n",
                        g_fpga_operation_count, 19443840 - g_fpga_operation_count);
                fprintf(stderr, "[LD_PRELOAD] ========================================\n");
            }
            // Skip further GPIO operations for speed
            return 0;
        }

        // Log progress every 100 operations
        if (g_fpga_operation_count % 100 == 0) {
            fprintf(stderr, "[LD_PRELOAD] FPGA loading progress: %d operations...\n",
                    g_fpga_operation_count);
        }
    } else {
        // Normal GPIO write logging for non-FPGA pins
        fprintf(stderr, "[LD_PRELOAD] gpioWrite(gpio=%u, level=%u)\n", gpio, level);
    }

    pthread_mutex_lock(&g_sim_mutex);
    if (gpio < 32) {
        g_sim_state.gpio_states[gpio] = level;
    }
    pthread_mutex_unlock(&g_sim_mutex);
    return 0;  // Success
}

int gpioRead(unsigned gpio) {
    pthread_mutex_lock(&g_sim_mutex);
    int level = (gpio < 32) ? g_sim_state.gpio_states[gpio] : 0;
    pthread_mutex_unlock(&g_sim_mutex);

    // GPIO 27 = FPGA_DONE signal
    // Return HIGH when FPGA loading is complete (simulated)
    if (gpio == 27 && g_fpga_done) {
        level = 1;  // Signal FPGA programming success
        fprintf(stderr, "[LD_PRELOAD] gpioRead(gpio=%u) = %d [FPGA_DONE]\n", gpio, level);
    } else {
        fprintf(stderr, "[LD_PRELOAD] gpioRead(gpio=%u) = %d\n", gpio, level);
    }

    return level;
}

int gpioSetISRFunc(unsigned gpio, unsigned edge, int timeout, void *f) {
    fprintf(stderr, "[LD_PRELOAD] gpioSetISRFunc(gpio=%u, edge=%u, timeout=%d)\n", gpio, edge, timeout);
    // For simulation, we don't trigger interrupts
    return 0;  // Success
}

// I2C simulation
int i2cOpen(unsigned i2cBus, unsigned i2cAddr, unsigned i2cFlags) {
    fprintf(stderr, "[LD_PRELOAD] i2cOpen(bus=%u, addr=0x%02X, flags=%u)\n", i2cBus, i2cAddr, i2cFlags);
    // Encode bus and address into handle: (bus << 16) | addr
    // This allows addresses 0x00-0xFF on any bus
    return (int)((i2cBus << 16) | i2cAddr);
}

int i2cClose(unsigned handle) {
    fprintf(stderr, "[LD_PRELOAD] i2cClose(handle=%u)\n", handle);
    return 0;  // Success
}

int i2cReadDevice(unsigned handle, char *buf, unsigned count) {
    unsigned addr = handle & 0xFF;  // Extract address from handle
    fprintf(stderr, "[LD_PRELOAD] i2cReadDevice(handle=%u/addr=0x%02X, count=%u)\n", handle, addr, count);

    pthread_mutex_lock(&g_sim_mutex);

    // Simulate sensor responses based on I2C address
    switch (addr) {
        case 0x40:  // Keller 4LD pressure sensor (primary address)
        case 0x44:  // Keller/MSR pressure sensor (alternate address)
            if (g_pressure_cmd_pending && count >= 3) {
                // MSR/Keller response format (3 or 5 bytes):
                // [0] = status byte
                // [1] = pressure MSB
                // [2] = pressure LSB
                // [3] = temperature MSB (if count >= 5)
                // [4] = temperature LSB (if count >= 5)

                // Status byte: 0b01000000 = valid measurement, not busy
                buf[0] = 0x40;

                // Convert pressure (bar) to raw 16-bit value
                // Formula from keller4ld.h: raw = (pressure / scale_factor) + 16384
                double scale_factor = (200.0 - 0.0) / 32768.0;  // PRESSURE_MAX=200, PRESSURE_MIN=0
                uint16_t pressure_raw = (uint16_t)((g_sim_state.pressure_bar / scale_factor) + 16384.0);
                buf[1] = (pressure_raw >> 8) & 0xFF;  // MSB
                buf[2] = pressure_raw & 0xFF;          // LSB

                if (count >= 5) {
                    // Temperature encoding: temp_raw = (temp_c + 50) / 0.05 + 24, then << 4
                    // From keller4ld.h: temp_c = ((raw >> 4) - 24) * 0.05 - 50
                    uint16_t temp_raw = (uint16_t)(((g_sim_state.temperature_c + 50.0) / 0.05 + 24.0)) << 4;
                    buf[3] = (temp_raw >> 8) & 0xFF;  // MSB
                    buf[4] = temp_raw & 0xFF;          // LSB
                }

                g_pressure_cmd_pending = 0;  // Clear pending flag

                fprintf(stderr, "[LD_PRELOAD] Pressure sensor response: %.2f bar, %.1f°C (status=0x%02X)\n",
                        g_sim_state.pressure_bar, g_sim_state.temperature_c, buf[0]);
            } else {
                // No pending command or invalid count - return zeros
                // This prevents returning uninitialized buffer data
                memset(buf, 0, count);
                fprintf(stderr, "[LD_PRELOAD] WARNING: Pressure sensor read without trigger (pending=%d, count=%u)\n",
                        g_pressure_cmd_pending, count);
            }
            break;

        case 0x36:  // MAX17320 battery gauge
            if (count >= 2) {
                // Return simulated voltage (in mV as 16-bit value)
                uint16_t voltage_mv = (uint16_t)(g_sim_state.battery_voltage * 1000);
                buf[0] = voltage_mv & 0xFF;          // LSB first
                buf[1] = (voltage_mv >> 8) & 0xFF;   // MSB
            }
            break;

        case 0x29:  // LTR-329ALS light sensor
            if (count >= 2) {
                buf[0] = g_sim_state.light_lux & 0xFF;         // LSB
                buf[1] = (g_sim_state.light_lux >> 8) & 0xFF;  // MSB
            }
            break;

        default:
            // Unknown device - return zeros
            memset(buf, 0, count);
            break;
    }

    pthread_mutex_unlock(&g_sim_mutex);

    return count;  // Return number of bytes read
}

int i2cWriteDevice(unsigned handle, char *buf, unsigned count) {
    unsigned addr = handle & 0xFF;  // Extract address from handle
    fprintf(stderr, "[LD_PRELOAD] i2cWriteDevice(handle=%u/addr=0x%02X, count=%u)\n", handle, addr, count);
    return 0;  // Success
}

int i2cReadByteData(unsigned handle, unsigned reg) {
    unsigned addr = handle & 0xFF;  // Extract address from handle
    fprintf(stderr, "[LD_PRELOAD] i2cReadByteData(handle=%u/addr=0x%02X, reg=0x%02X)\n", handle, addr, reg);

    pthread_mutex_lock(&g_sim_mutex);

    int result = 0;

    // Simulate sensor responses based on I2C address and register
    switch (addr) {
        case 0x68:  // RTC - Real-Time Clock
            // RTC counter is 32-bit value stored in registers 0-3 (little-endian)
            if (reg < 4) {
                result = (g_sim_state.rtc_counter >> (8 * reg)) & 0xFF;
            }
            break;

        case 0x21:  // IOX - GPIO Expander (PCA9536-compatible)
            // Read from IOX register array
            if (reg < sizeof(g_sim_state.iox_registers)) {
                result = g_sim_state.iox_registers[reg];
            }
            break;

        case 0x40:  // Keller 4LD pressure sensor
            // The Keller sensor uses i2cReadDevice for multi-byte reads
            // For single byte reads, return a reasonable value
            // Convert pressure (bar) to raw 16-bit value
            double scale_factor = (200.0 - 0.0) / 32768.0;
            int16_t raw = (int16_t)((g_sim_state.pressure_bar / scale_factor) + 16384.0);

            // Return MSB or LSB depending on register
            if (reg == 0) {
                result = (raw >> 8) & 0xFF;  // MSB
            } else {
                result = raw & 0xFF;         // LSB
            }
            break;

        case 0x36:  // MAX17320 battery gauge
            // Battery gauge registers vary by register number
            // For simplicity, return voltage data
            {
                uint16_t voltage_mv = (uint16_t)(g_sim_state.battery_voltage * 1000);
                if (reg == 0) {
                    result = voltage_mv & 0xFF;          // LSB
                } else {
                    result = (voltage_mv >> 8) & 0xFF;   // MSB
                }
            }
            break;

        case 0x29:  // LTR-329ALS light sensor
            if (reg == 0) {
                result = g_sim_state.light_lux & 0xFF;         // LSB
            } else {
                result = (g_sim_state.light_lux >> 8) & 0xFF;  // MSB
            }
            break;

        default:
            // Unknown device - return 0
            result = 0;
            break;
    }

    pthread_mutex_unlock(&g_sim_mutex);

    return result;
}

int i2cWriteByteData(unsigned handle, unsigned reg, unsigned value) {
    unsigned addr = handle & 0xFF;  // Extract address from handle
    fprintf(stderr, "[LD_PRELOAD] i2cWriteByteData(handle=%u/addr=0x%02X, reg=0x%02X, value=0x%02X)\n",
            handle, addr, reg, value);

    pthread_mutex_lock(&g_sim_mutex);

    // Handle device-specific writes
    switch (addr) {
        case 0x21:  // IOX - GPIO Expander
            if (reg < sizeof(g_sim_state.iox_registers)) {
                g_sim_state.iox_registers[reg] = value;

                // Log important register writes
                if (reg == 0x01) {  // OUTPUT register
                    fprintf(stderr, "[LD_PRELOAD] IOX OUTPUT register: 0x%02X (BURNWIRE_ON=%d)\n",
                            value, (value >> 4) & 1);
                }
            }
            break;

        case 0x68:  // RTC - Real-Time Clock
            // Allow writing to RTC counter (registers 0-3)
            if (reg < 4) {
                uint32_t mask = ~(0xFF << (8 * reg));
                g_sim_state.rtc_counter = (g_sim_state.rtc_counter & mask) | (value << (8 * reg));
                fprintf(stderr, "[LD_PRELOAD] RTC counter updated to %u\n", g_sim_state.rtc_counter);
            }
            break;

        default:
            // For other devices, just acknowledge the write
            break;
    }

    pthread_mutex_unlock(&g_sim_mutex);

    return 0;  // Success
}

int i2cReadWordData(unsigned handle, unsigned reg) {
    unsigned addr = handle & 0xFF;  // Extract address from handle
    fprintf(stderr, "[LD_PRELOAD] i2cReadWordData(handle=%u/addr=0x%02X, reg=0x%02X)\n", handle, addr, reg);

    pthread_mutex_lock(&g_sim_mutex);

    int result = 0;

    // Similar to i2cReadByteData but returns 16-bit word
    switch (addr) {
        case 0x40:  // Keller 4LD pressure sensor
            double scale_factor = (200.0 - 0.0) / 32768.0;
            result = (int16_t)((g_sim_state.pressure_bar / scale_factor) + 16384.0);
            break;

        case 0x36:  // MAX17320 battery gauge (I2C addr for regs 0x000-0x0FF)
        case 0x0b:  // MAX17320 upper range (I2C addr for regs 0x180-0x1FF)
            {
                uint16_t raw_value = 0;

                // Handle register-specific conversions per MAX17320 datasheet
                // Cell voltages: LSB = 0.000078125 V = 1/12800 V
                if (reg == 0xD8) {  // Cell 1 voltage (MAX17320_REG_CELL1_VOLTAGE)
                    raw_value = (uint16_t)(g_sim_state.battery_voltage / 0.000078125);
                }
                else if (reg == 0xD7) {  // Cell 2 voltage (MAX17320_REG_CELL2_VOLTAGE)
                    raw_value = (uint16_t)(g_sim_state.battery_voltage / 0.000078125);
                }
                // Current: LSB = 1.5625 µV / R_sense (R_sense = 10mΩ)
                // Positive = charging, negative = discharging
                else if (reg == 0x1C) {  // Battery current (MAX17320_REG_BATT_CURRENT)
                    // Simulate 100mA discharge: 100mA * 10mΩ = 1mV = 1000µV
                    // raw = 1000 / 1.5625 = 640
                    raw_value = (uint16_t)(100.0 / 0.15625);  // 100mA discharge
                }
                // Temperature: LSB = 1/256 °C
                else if (reg == 0x3A) {  // Cell 0 temperature (MAX17320_REG_TEMP)
                    raw_value = (uint16_t)(20.0 * 256);  // 20°C
                }
                else if (reg == 0x39) {  // Die temperature
                    raw_value = (uint16_t)(25.0 * 256);  // 25°C
                }
                // State of charge: LSB = 1/256 %
                else if (reg == 0x06) {  // State of charge (MAX17320_REG_REP_SOC)
                    raw_value = (uint16_t)(75.0 * 256);  // 75% SOC
                }
                // Capacity: LSB = 0.005 mVh / R_sense
                else if (reg == 0x05) {  // Remaining capacity
                    raw_value = (uint16_t)(1500.0 / 0.05);  // 1500 mAh
                }
                else if (reg == 0x10) {  // Full capacity
                    raw_value = (uint16_t)(2000.0 / 0.05);  // 2000 mAh
                }
                // Status registers
                else if (reg == 0x00) {  // Status (MAX17320_REG_STATUS)
                    raw_value = 0x0000;  // No errors, no alerts
                }
                else if (reg == 0xD0) {  // ProtStatus
                    raw_value = 0x0000;  // No protection alerts
                }
                else if (reg == 0xAF) {  // ProtAlrt
                    raw_value = 0x0000;  // No protection alerts
                }
                // Default for unhandled registers
                else {
                    raw_value = 0x0000;
                }

                result = raw_value;
            }
            break;

        default:
            result = 0;
            break;
    }

    pthread_mutex_unlock(&g_sim_mutex);

    return result;
}

int i2cWriteByte(unsigned handle, unsigned value) {
    unsigned addr = handle & 0xFF;  // Extract address from handle
    fprintf(stderr, "[LD_PRELOAD] i2cWriteByte(handle=%u/addr=0x%02X, value=0x%02X)\n", handle, addr, value);

    pthread_mutex_lock(&g_sim_mutex);

    // Handle pressure sensor measurement trigger
    if ((addr == 0x40 || addr == 0x44) && value == 0xAC) {
        // Keller 4LD measurement request command
        g_pressure_cmd_pending = 1;
        fprintf(stderr, "[LD_PRELOAD] Pressure sensor measurement triggered\n");
    }

    pthread_mutex_unlock(&g_sim_mutex);

    return 0;  // Success
}

int i2cWriteWordData(unsigned handle, unsigned reg, unsigned value) {
    unsigned addr = handle & 0xFF;  // Extract address from handle
    fprintf(stderr, "[LD_PRELOAD] i2cWriteWordData(handle=%u/addr=0x%02X, reg=0x%02X, value=0x%04X)\n",
            handle, addr, reg, value);
    return 0;  // Success
}

// Serial functions
int serOpen(char *sertty, unsigned baud, unsigned flags) {
    fprintf(stderr, "[LD_PRELOAD] serOpen(tty=%s, baud=%u, flags=%u)\n", sertty, baud, flags);
    // Return fake handle based on device name hash
    return (int)(sertty[0] + baud % 100);
}

int serClose(unsigned handle) {
    fprintf(stderr, "[LD_PRELOAD] serClose(handle=%u)\n", handle);
    return 0;  // Success
}

int serDataAvailable(unsigned handle) {
    // Return 0 (no data available) to prevent busy-waiting
    // Suppress logging to avoid spam
    return 0;
}

int serRead(unsigned handle, char *buf, unsigned count) {
    fprintf(stderr, "[LD_PRELOAD] serRead(handle=%u, count=%u)\n", handle, count);
    // Return 0 (no data) for simulation
    return 0;
}

int serWrite(unsigned handle, char *buf, unsigned count) {
    fprintf(stderr, "[LD_PRELOAD] serWrite(handle=%u, count=%u)\n", handle, count);
    return count;  // Pretend all data was written
}

// Bit-bang I2C (for BNO086 IMU)
int bbI2COpen(unsigned SDA, unsigned SCL, unsigned baud) {
    fprintf(stderr, "[LD_PRELOAD] bbI2COpen(SDA=%u, SCL=%u, baud=%u)\n", SDA, SCL, baud);
    g_bbi2c_open = 1;
    g_bbi2c_addr = 0;
    return 0;  // Success
}

int bbI2CClose(unsigned SDA) {
    fprintf(stderr, "[LD_PRELOAD] bbI2CClose(SDA=%u)\n", SDA);
    g_bbi2c_open = 0;
    g_bbi2c_addr = 0;
    return 0;  // Success
}

int bbI2CZip(unsigned SDA, char *inBuf, unsigned inLen, char *outBuf, unsigned outLen) {
    fprintf(stderr, "[LD_PRELOAD] bbI2CZip(SDA=%u, inLen=%u, outLen=%u)\n", SDA, inLen, outLen);

    // Parse command buffer and execute I2C operations
    unsigned i = 0;
    unsigned bytes_read = 0;
    int in_transaction = 0;

    while (i < inLen) {
        uint8_t cmd = (uint8_t)inBuf[i++];

        switch (cmd) {
            case 0x00:  // End
                fprintf(stderr, "[LD_PRELOAD]   CMD: END\n");
                goto done;

            case 0x01:  // Escape (next parameter is 2 bytes)
                fprintf(stderr, "[LD_PRELOAD]   CMD: ESCAPE\n");
                // Next command will use 2-byte parameter
                break;

            case 0x02:  // Start condition
                fprintf(stderr, "[LD_PRELOAD]   CMD: START (addr=0x%02X)\n", g_bbi2c_addr);
                in_transaction = 1;
                break;

            case 0x03:  // Stop condition
                fprintf(stderr, "[LD_PRELOAD]   CMD: STOP\n");
                in_transaction = 0;
                break;

            case 0x04:  // Set address
                if (i < inLen) {
                    g_bbi2c_addr = (uint8_t)inBuf[i++];
                    fprintf(stderr, "[LD_PRELOAD]   CMD: SET_ADDR 0x%02X\n", g_bbi2c_addr);
                }
                break;

            case 0x05:  // Set flags
                if (i + 1 < inLen) {
                    uint16_t flags = ((uint8_t)inBuf[i]) | (((uint8_t)inBuf[i+1]) << 8);
                    i += 2;
                    fprintf(stderr, "[LD_PRELOAD]   CMD: SET_FLAGS 0x%04X\n", flags);
                }
                break;

            case 0x06:  // Read bytes
                if (i + 1 < inLen) {
                    uint16_t read_len = ((uint8_t)inBuf[i]) | (((uint8_t)inBuf[i+1]) << 8);
                    i += 2;
                    fprintf(stderr, "[LD_PRELOAD]   CMD: READ %u bytes from 0x%02X\n", read_len, g_bbi2c_addr);

                    // Simulate IMU (BNO086) responses
                    if (outBuf && bytes_read + read_len <= outLen) {
                        if (g_bbi2c_addr == 0x4A || g_bbi2c_addr == 0x4B) {
                            // BNO086 IMU - Simulate initialization sequence
                            g_imu_read_count++;

                            if (read_len == 4) {
                                // Header read - return "no data" most of the time
                                // This tells firmware no data is ready
                                outBuf[bytes_read + 0] = 0x00;  // Length LSB (0 = no data)
                                outBuf[bytes_read + 1] = 0x00;  // Length MSB
                                outBuf[bytes_read + 2] = 0x00;  // Channel
                                outBuf[bytes_read + 3] = 0x00;  // Sequence
                                fprintf(stderr, "[LD_PRELOAD]   IMU: No data available (read #%d)\n", g_imu_read_count);
                            } else {
                                // Full packet read - should not happen if header says no data
                                // But if it does, return zeros
                                memset(&outBuf[bytes_read], 0, read_len);
                                fprintf(stderr, "[LD_PRELOAD]   IMU: Returning %u zero bytes\n", read_len);
                            }
                        } else {
                            // Unknown device - return zeros
                            memset(&outBuf[bytes_read], 0, read_len);
                        }
                        bytes_read += read_len;
                    }
                }
                break;

            case 0x07:  // Write bytes
                if (i < inLen) {
                    uint8_t write_len = (uint8_t)inBuf[i++];
                    fprintf(stderr, "[LD_PRELOAD]   CMD: WRITE %u bytes to 0x%02X\n", write_len, g_bbi2c_addr);
                    // Skip the data bytes
                    i += write_len;
                }
                break;

            default:
                fprintf(stderr, "[LD_PRELOAD]   CMD: UNKNOWN 0x%02X\n", cmd);
                break;
        }
    }

done:
    fprintf(stderr, "[LD_PRELOAD]   Returned %u bytes\n", bytes_read);
    return bytes_read;  // Return number of bytes read
}

// SPI simulation (for FPGA/audio)
int spiOpen(unsigned spiChan, unsigned baud, unsigned spiFlags) {
    fprintf(stderr, "[LD_PRELOAD] spiOpen(chan=%u, baud=%u, flags=%u)\n", spiChan, baud, spiFlags);
    return (int)spiChan;  // Return fake handle
}

int spiClose(unsigned handle) {
    fprintf(stderr, "[LD_PRELOAD] spiClose(handle=%u)\n", handle);
    return 0;  // Success
}

int spiRead(unsigned handle, char *buf, unsigned count) {
    fprintf(stderr, "[LD_PRELOAD] spiRead(handle=%u, count=%u)\n", handle, count);
    if (buf && count > 0) {
        memset(buf, 0, count);  // Return zeros (no audio data in simulation)
    }
    return count;
}

int spiWrite(unsigned handle, char *buf, unsigned count) {
    fprintf(stderr, "[LD_PRELOAD] spiWrite(handle=%u, count=%u)\n", handle, count);
    return count;
}

int spiXfer(unsigned handle, char *txBuf, char *rxBuf, unsigned count) {
    fprintf(stderr, "[LD_PRELOAD] spiXfer(handle=%u, count=%u)\n", handle, count);
    if (rxBuf && count > 0) {
        memset(rxBuf, 0, count);  // Return zeros
    }
    return count;
}

// Time functions
uint32_t gpioTick(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((ts.tv_sec * 1000000) + (ts.tv_nsec / 1000));
}

// ============================================================================
// File I/O Interception (workaround for QEMU user-mode filesystem limitations)
// ============================================================================

// Redirect /data/* paths to /tmp/data/* where QEMU can actually write
static void redirect_path(const char* original, char* redirected, size_t size) {
    if (strncmp(original, "/data/", 6) == 0) {
        // Extract filename from /data/path
        const char* filename = original + 6;  // Skip "/data/"
        snprintf(redirected, size, "/tmp/qemu_data/%s", filename);

        // Ensure /tmp/qemu_data exists
        static int dir_created = 0;
        if (!dir_created) {
            system("mkdir -p /tmp/qemu_data");
            dir_created = 1;
            fprintf(stderr, "[LD_PRELOAD] File I/O redirected: /data/* -> /tmp/qemu_data/*\n");
        }
    } else {
        snprintf(redirected, size, "%s", original);
    }
}

// Function pointers to real implementations
static FILE* (*real_fopen)(const char*, const char*) = NULL;
static int (*real_open)(const char*, int, ...) = NULL;
static int (*real_access)(const char*, int) = NULL;

// Override fopen
FILE* fopen(const char* path, const char* mode) {
    if (!real_fopen) {
        real_fopen = dlsym(RTLD_NEXT, "fopen");
    }

    char redirected[512];
    redirect_path(path, redirected, sizeof(redirected));

    if (strcmp(path, redirected) != 0) {
        fprintf(stderr, "[LD_PRELOAD] fopen('%s', '%s') -> '%s'\n", path, mode, redirected);
    }

    return real_fopen(redirected, mode);
}

// Override open
int open(const char* path, int flags, ...) {
    if (!real_open) {
        real_open = dlsym(RTLD_NEXT, "open");
    }

    char redirected[512];
    redirect_path(path, redirected, sizeof(redirected));

    if (strcmp(path, redirected) != 0) {
        fprintf(stderr, "[LD_PRELOAD] open('%s', %d) -> '%s'\n", path, flags, redirected);
    }

    // Handle variadic mode parameter for O_CREAT
    mode_t mode = 0;
    if (flags & 0100) {  // O_CREAT
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
        return real_open(redirected, flags, mode);
    }

    return real_open(redirected, flags);
}

// Override access
int access(const char* path, int mode) {
    if (!real_access) {
        real_access = dlsym(RTLD_NEXT, "access");
    }

    char redirected[512];
    redirect_path(path, redirected, sizeof(redirected));

    if (strcmp(path, redirected) != 0) {
        fprintf(stderr, "[LD_PRELOAD] access('%s', %d) -> '%s'\n", path, mode, redirected);
    }

    return real_access(redirected, mode);
}
