CC = gcc
CFLAGS = -Wall -Wextra -fPIC -shared -pthread
TARGET = build/libpigpio_sim.so

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

clean:
	rm -rf build

.PHONY: all clean
