CC = gcc
CC_ARM64 = aarch64-linux-gnu-gcc
CFLAGS = -Wall -Wextra -fPIC -shared -pthread
TARGET = build/libpigpio_sim.so
TARGET_ARM64 = build/libpigpio_sim_arm64.so

SRC = libpigpio_sim.c

all: $(TARGET)

$(TARGET): $(SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)
	@echo "LD_PRELOAD library built: $(TARGET)"
	@echo ""
	@echo "Usage:"
	@echo "  LD_PRELOAD=$(shell pwd)/$(TARGET) /opt/ceti-tag-data-capture/bin/cetiTagApp"
	@echo ""
	@echo "Network control (UDP port 9999):"
	@echo "  echo 'DEPTH=10.5' | nc -u -w1 localhost 9999    # Set depth to 10.5m"
	@echo "  echo 'TEMP=15.0' | nc -u -w1 localhost 9999     # Set temp to 15°C"
	@echo "  echo 'LIGHT=500' | nc -u -w1 localhost 9999     # Set light to 500 lux"
	@echo "  echo 'BATTERY=3.6' | nc -u -w1 localhost 9999   # Set battery to 3.6V"

arm64: $(TARGET_ARM64)

$(TARGET_ARM64): $(SRC)
	@mkdir -p build
	@if command -v $(CC_ARM64) > /dev/null 2>&1; then \
		$(CC_ARM64) $(CFLAGS) -o $(TARGET_ARM64) $(SRC); \
		echo "ARM64 LD_PRELOAD library built: $(TARGET_ARM64)"; \
	else \
		echo "Building ARM64 library using Docker..."; \
		docker run --rm -v $(shell pwd):/build -w /build debian:bookworm bash -c " \
			apt-get update -qq && apt-get install -y -qq gcc-aarch64-linux-gnu && \
			aarch64-linux-gnu-gcc $(CFLAGS) -o $(TARGET_ARM64) $(SRC)"; \
		echo "ARM64 LD_PRELOAD library built: $(TARGET_ARM64)"; \
	fi

clean:
	rm -rf build

.PHONY: all arm64 clean
