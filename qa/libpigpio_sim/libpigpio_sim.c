/*
 * libpigpio_sim.c - LD_PRELOAD simulation library for whale tag testing
 *
 * This library intercepts pigpio function calls and simulates hardware behavior
 * without requiring real sensors or GPIO hardware.
 *
 * Usage:
 *   LD_PRELOAD=./libpigpio_sim.so /opt/ceti-tag-data-capture/bin/cetiTagApp
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Simulated sensor state
typedef struct {
    double pressure_bar;     // Pressure in bar (depth simulation)
    double temperature_c;    // Temperature in Celsius
    uint16_t light_lux;      // Light level in lux
    double battery_voltage;  // Battery voltage
    int gpio_states[32];     // GPIO pin states
} SimState;

static SimState g_sim_state = {
    .pressure_bar = 1.0,      // 1 bar = surface
    .temperature_c = 20.0,    // 20°C
    .light_lux = 1000,        // 1000 lux (daylight)
    .battery_voltage = 3.7,   // 3.7V (typical Li-ion)
};

static pthread_mutex_t g_sim_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_udp_socket = -1;
static int g_gpio_initialized = 0;

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

    // Start network control thread
    pthread_t thread;
    pthread_create(&thread, NULL, network_control_thread, NULL);
    pthread_detach(thread);

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
    fprintf(stderr, "[LD_PRELOAD] gpioWrite(gpio=%u, level=%u)\n", gpio, level);
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
    fprintf(stderr, "[LD_PRELOAD] gpioRead(gpio=%u) = %d\n", gpio, level);
    return level;
}

// I2C simulation
int i2cOpen(unsigned i2cBus, unsigned i2cAddr, unsigned i2cFlags) {
    fprintf(stderr, "[LD_PRELOAD] i2cOpen(bus=%u, addr=0x%02X, flags=%u)\n", i2cBus, i2cAddr, i2cFlags);
    return (int)(i2cBus * 100 + i2cAddr);  // Return fake handle
}

int i2cClose(unsigned handle) {
    fprintf(stderr, "[LD_PRELOAD] i2cClose(handle=%u)\n", handle);
    return 0;  // Success
}

int i2cReadDevice(unsigned handle, char *buf, unsigned count) {
    unsigned addr = handle % 100;
    fprintf(stderr, "[LD_PRELOAD] i2cReadDevice(handle=%u/addr=0x%02X, count=%u)\n", handle, addr, count);

    pthread_mutex_lock(&g_sim_mutex);

    // Simulate sensor responses based on I2C address
    switch (addr) {
        case 0x40:  // Keller 4LD pressure sensor
            if (count >= 2) {
                // Convert pressure (bar) to raw 16-bit value
                // Formula from keller4ld.h: raw = (pressure / scale_factor) + 16384
                double scale_factor = (200.0 - 0.0) / 32768.0;  // PRESSURE_MAX=200, PRESSURE_MIN=0
                int16_t raw = (int16_t)((g_sim_state.pressure_bar / scale_factor) + 16384.0);
                buf[0] = (raw >> 8) & 0xFF;  // MSB
                buf[1] = raw & 0xFF;          // LSB
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
    unsigned addr = handle % 100;
    fprintf(stderr, "[LD_PRELOAD] i2cWriteDevice(handle=%u/addr=0x%02X, count=%u)\n", handle, addr, count);
    return 0;  // Success
}

int i2cReadByteData(unsigned handle, unsigned reg) {
    unsigned addr = handle % 100;
    fprintf(stderr, "[LD_PRELOAD] i2cReadByteData(handle=%u/addr=0x%02X, reg=0x%02X)\n", handle, addr, reg);

    pthread_mutex_lock(&g_sim_mutex);

    int result = 0;

    // Simulate sensor responses based on I2C address and register
    switch (addr) {
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

// Bit-bang I2C (for BNO086 IMU)
int bbI2COpen(unsigned SDA, unsigned SCL, unsigned baud) {
    fprintf(stderr, "[LD_PRELOAD] bbI2COpen(SDA=%u, SCL=%u, baud=%u)\n", SDA, SCL, baud);
    return SDA;  // Return fake handle (use SDA pin number)
}

int bbI2CClose(unsigned SDA) {
    fprintf(stderr, "[LD_PRELOAD] bbI2CClose(SDA=%u)\n", SDA);
    return 0;  // Success
}

int bbI2CZip(unsigned SDA, char *inBuf, unsigned inLen, char *outBuf, unsigned outLen) {
    fprintf(stderr, "[LD_PRELOAD] bbI2CZip(SDA=%u, inLen=%u, outLen=%u)\n", SDA, inLen, outLen);

    // Simulate BNO086 IMU responses (basic SHTP protocol)
    if (outBuf && outLen > 0) {
        // Return fake SHTP header or sensor data
        memset(outBuf, 0, outLen);
        if (outLen >= 4) {
            outBuf[0] = outLen & 0xFF;        // Packet length LSB
            outBuf[1] = (outLen >> 8) & 0xFF; // Packet length MSB
            outBuf[2] = 0x00;                 // Channel number
            outBuf[3] = 0x00;                 // Sequence number
        }
    }

    return 0;  // Success
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
