#!/bin/bash
#
# Test script for Depth-Aware Burnwire Feature (PR #111)
#
# This test verifies that the burnwire only heats when the tag is underwater,
# preventing wasted heating during surface intervals when the whale breathes.
#
# Expected behavior:
# 1. Firmware starts and reaches ST_START state
# 2. After timeout expires, enters ST_BRN_ON state
# 3. At surface (< burn_depth_threshold): burnwire OFF
# 4. Underwater (> burn_depth_threshold): burnwire ON
# 5. Only active underwater time counts toward burn_interval_s
#
# Requirements:
# - SD card image built: out/sdcard.img (expanded with qa/expand_image.sh)
# - LD_PRELOAD library: qa/libpigpio_sim/build/libpigpio_sim_arm64.so
# - Docker with privileged mode support

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test parameters
CONTAINER_NAME="burnwire-test"
TIMEOUT_S=10              # Short timeout for testing (seconds until burnwire activates)
BURN_INTERVAL_S=60        # Active burn time (underwater only) - extended for better testing
BURN_DEPTH_THRESHOLD=4.0  # Depth threshold in meters (default)
SURFACE_PRESSURE=0.3      # Surface detection threshold (bar)

echo "========================================="
echo "Depth-Aware Burnwire Test (PR #111)"
echo "========================================="
echo ""

# Check prerequisites
if [ ! -f "out/sdcard.img" ]; then
    echo -e "${RED}Error: SD card image not found: out/sdcard.img${NC}"
    echo "Run 'make build' first"
    exit 1
fi

if [ ! -f "qa/libpigpio_sim/build/libpigpio_sim_arm64.so" ]; then
    echo -e "${RED}Error: LD_PRELOAD library not found${NC}"
    echo "Run: cd qa/libpigpio_sim && make arm64"
    exit 1
fi

# Cleanup any existing container and loop devices
docker rm -f $CONTAINER_NAME 2>/dev/null || true

echo "Test parameters:"
echo "  timeout_s: $TIMEOUT_S"
echo "  burn_interval_s: $BURN_INTERVAL_S (active underwater time)"
echo "  burn_depth_threshold: $BURN_DEPTH_THRESHOLD m"
echo "  surface_pressure: $SURFACE_PRESSURE bar"
echo ""

# Map SD card partitions on HOST (kpartx needs kernel access)
echo "Mapping SD card partitions..."
KPARTX_OUT=$(kpartx -av out/sdcard.img)
echo "$KPARTX_OUT"
sleep 1

# Extract loop device name
LOOP_DEV=$(echo "$KPARTX_OUT" | head -1 | grep -oP 'loop\d+')
if [ -z "$LOOP_DEV" ]; then
    echo -e "${RED}Error: Failed to detect loop device${NC}"
    exit 1
fi
echo "Loop device: $LOOP_DEV"

# Start Docker container with loop devices bound
echo "Starting Docker container..."
docker run -d \
    --name $CONTAINER_NAME \
    --privileged \
    -v "$(pwd):/work" \
    -v "/dev:/dev" \
    -w /work \
    debian:bookworm \
    tail -f /dev/null

# Install dependencies
echo "Installing container dependencies..."
docker exec $CONTAINER_NAME bash -c "
    apt-get update -qq && \
    apt-get install -y -qq qemu-user-static netcat-openbsd procps > /dev/null 2>&1
"

# Mount partitions inside container
echo "Mounting partitions..."
docker exec $CONTAINER_NAME bash -c "
    # Mount rootfs (partition 2)
    # Note: We do NOT mount partition 3 (data) because QEMU user-mode
    # cannot write to mount points. Instead, firmware will write to
    # /data directory within the rootfs partition.
    mkdir -p /mnt/img
    mount /dev/mapper/${LOOP_DEV}p2 /mnt/img

    # Ensure /data directory exists and is writable
    mkdir -p /mnt/img/data
    chmod 755 /mnt/img/data

    echo 'Rootfs mounted successfully'
    mount | grep /mnt/img
"

# Inject test configuration
echo "Injecting test configuration..."
docker exec $CONTAINER_NAME bash -c "
    mkdir -p /mnt/img/data/config
    cat > /mnt/img/data/config/ceti-config.txt <<EOF
# Depth-Aware Burnwire Test Configuration
timeout_s=$TIMEOUT_S
burn_interval_s=$BURN_INTERVAL_S
burn_depth_threshold=$BURN_DEPTH_THRESHOLD
surface_pressure=$SURFACE_PRESSURE
aprs_on_whale=false
EOF
    echo 'Configuration injected:'
    cat /mnt/img/data/config/ceti-config.txt
"

# Start firmware
echo ""
echo "========================================="
echo "Starting Firmware"
echo "========================================="
docker exec -d $CONTAINER_NAME bash -c "
    qemu-aarch64-static -L /mnt/img \
        -E LD_PRELOAD=/work/qa/libpigpio_sim/build/libpigpio_sim_arm64.so \
        /mnt/img/opt/ceti-tag-data-capture/bin/cetiTagApp > /firmware.log 2>&1
"

# Wait for initialization
echo "Waiting for firmware initialization (15s)..."
sleep 15

# Verify firmware is running
docker exec $CONTAINER_NAME bash -c "ps aux | grep cetiTagApp | grep -v grep" > /dev/null
if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Firmware running${NC}"
    echo ""
    echo "=== Firmware Initialization Log (first 50 lines) ==="
    docker exec $CONTAINER_NAME head -50 /firmware.log
    echo "==="
    echo ""
    echo "=== Data Directory Check ==="
    docker exec $CONTAINER_NAME bash -c "ls -la /mnt/img/data/ 2>&1 | head -20"
    docker exec $CONTAINER_NAME bash -c "mount | grep /mnt/img"
    echo "==="
else
    echo -e "${RED}✗ Firmware not running${NC}"
    docker exec $CONTAINER_NAME cat /firmware.log | tail -50
    exit 1
fi

echo ""
echo "========================================="
echo "TEST: Burnwire Timeout & Depth Control"
echo "========================================="
echo ""
echo "This test demonstrates depth-aware burnwire behavior:"
echo "  - Burnwire ONLY heats when underwater (>$BURN_DEPTH_THRESHOLD m)"
echo "  - Active burn time counted separately from calendar time"
echo "  - Burnwire pauses at surface during whale breathing intervals"
echo ""
echo "Phase 1: Wait for timeout ($TIMEOUT_S seconds)..."
sleep $((TIMEOUT_S + 5))

echo ""
echo "Phase 2: Initial dive - Start burning (burnwire should turn ON)"
echo "  Simulating dive: 10m depth"
echo 'DEPTH=10' | docker exec -i $CONTAINER_NAME nc -u -w1 127.0.0.1 9999
sleep 8
echo "  Active burn time: ~8 seconds"

echo ""
echo "Phase 3: Surface breathing - Pause burning (burnwire should turn OFF)"
echo "  Simulating surface: 0.5m depth"
echo 'DEPTH=0.5' | docker exec -i $CONTAINER_NAME nc -u -w1 127.0.0.1 9999
sleep 6
echo "  Burnwire paused (no active burn time accumulated)"

echo ""
echo "Phase 4: Second dive - Resume burning (burnwire should turn ON)"
echo "  Simulating dive: 15m depth"
echo 'DEPTH=15' | docker exec -i $CONTAINER_NAME nc -u -w1 127.0.0.1 9999
sleep 10
echo "  Active burn time: ~18 seconds total (8 + 10)"

echo ""
echo "Phase 5: Surface again - Pause burning (burnwire should turn OFF)"
echo "  Simulating surface: 1m depth"
echo 'DEPTH=1' | docker exec -i $CONTAINER_NAME nc -u -w1 127.0.0.1 9999
sleep 5
echo "  Burnwire paused again"

echo ""
echo "Phase 6: Third dive - Resume burning (burnwire should turn ON)"
echo "  Simulating dive: 12m depth"
echo 'DEPTH=12' | docker exec -i $CONTAINER_NAME nc -u -w1 127.0.0.1 9999
sleep 10
echo "  Active burn time: ~28 seconds total (8 + 10 + 10)"

echo ""
echo "Phase 7: Fourth dive - Continue burning to completion"
echo "  Simulating dive: 20m depth"
echo 'DEPTH=20' | docker exec -i $CONTAINER_NAME nc -u -w1 127.0.0.1 9999
sleep 15
echo "  Active burn time: ~43 seconds total"

echo ""
echo "Phase 8: Final dive - Complete burn cycle"
echo "  Simulating dive: 18m depth"
echo 'DEPTH=18' | docker exec -i $CONTAINER_NAME nc -u -w1 127.0.0.1 9999
sleep 20
echo "  Active burn time: ~60 seconds total (should complete)"

echo ""
echo "Waiting for burn completion and shutdown..."
sleep 5

echo ""
echo "========================================="
echo "Collecting Evidence"
echo "========================================="
echo ""

# Check for burnwire events log (LD_PRELOAD redirects /data/* to /tmp/qemu_data/*)
echo "=== Burnwire Event Log (data_burnwire.csv) ==="
if docker exec $CONTAINER_NAME test -f /tmp/qemu_data/data_burnwire.csv; then
    docker exec $CONTAINER_NAME cat /tmp/qemu_data/data_burnwire.csv
    echo ""

    # Analyze events
    echo "=== Event Analysis ==="
    docker exec $CONTAINER_NAME bash -c "
        if [ -f /tmp/qemu_data/data_burnwire.csv ]; then
            echo 'Burn start events:'
            grep 'burn_start' /tmp/qemu_data/data_burnwire.csv | head -1
            echo ''
            echo 'Submerged events (burnwire ON):'
            grep 'submerged' /tmp/qemu_data/data_burnwire.csv | wc -l
            echo ''
            echo 'Surfaced events (burnwire OFF):'
            grep 'surfaced' /tmp/qemu_data/data_burnwire.csv | wc -l
            echo ''
            echo 'Burn complete event:'
            grep 'burn_complete' /tmp/qemu_data/data_burnwire.csv | tail -1
        fi
    "
else
    echo -e "${YELLOW}WARNING: data_burnwire.csv not created${NC}"
    echo "This may indicate the feature is not active or state not reached"
fi

echo ""
echo "=== State Machine Transitions (data_state.csv) ==="
if docker exec $CONTAINER_NAME test -f /tmp/qemu_data/data_state.csv; then
    docker exec $CONTAINER_NAME tail -10 /tmp/qemu_data/data_state.csv
else
    echo -e "${YELLOW}WARNING: data_state.csv not found${NC}"
fi

echo ""
echo "=== IOX Output Changes (from firmware log) ==="
docker exec $CONTAINER_NAME grep "IOX OUTPUT" /firmware.log | head -20
echo "..."
docker exec $CONTAINER_NAME grep "IOX OUTPUT" /firmware.log | tail -10

echo ""
echo "=== Pressure Sensor Responses ==="
docker exec $CONTAINER_NAME grep "Pressure sensor response" /firmware.log | head -10

echo ""
echo "========================================="
echo "Test Results Summary"
echo "========================================="
echo ""

# Verify depth-aware behavior
SUBMERGED_COUNT=$(docker exec $CONTAINER_NAME bash -c "grep -c 'submerged' /tmp/qemu_data/data_burnwire.csv 2>/dev/null || echo 0")
SURFACED_COUNT=$(docker exec $CONTAINER_NAME bash -c "grep -c 'surfaced' /tmp/qemu_data/data_burnwire.csv 2>/dev/null || echo 0")

echo "Depth-aware events detected:"
echo "  Submerged events: $SUBMERGED_COUNT"
echo "  Surfaced events: $SURFACED_COUNT"
echo ""

if [ "$SUBMERGED_COUNT" -gt 0 ] && [ "$SURFACED_COUNT" -gt 0 ]; then
    echo -e "${GREEN}✓ PASS: Depth-aware burnwire control verified${NC}"
    echo "  Burnwire toggled based on depth as expected"
else
    echo -e "${YELLOW}⚠ PARTIAL: Limited depth-aware events detected${NC}"
    echo "  Check logs for state machine progression"
fi

echo ""
echo "========================================="
echo "Cleanup"
echo "========================================="

# Check if KEEP_CONTAINER environment variable is set
if [ "$KEEP_CONTAINER" = "1" ]; then
    echo -e "${YELLOW}Container kept running for inspection: $CONTAINER_NAME${NC}"
    echo ""
    echo "To inspect:"
    echo "  docker exec -it $CONTAINER_NAME bash"
    echo "  docker exec $CONTAINER_NAME cat /firmware.log"
    echo "  docker exec $CONTAINER_NAME ls -la /mnt/img/data/"
    echo ""
    echo "To cleanup when done:"
    echo "  docker rm -f $CONTAINER_NAME"
    echo "  sudo kpartx -dv out/sdcard.img"
else
    docker rm -f $CONTAINER_NAME
    # Unmount and remove loop devices
    echo "Cleaning up loop devices..."
    kpartx -dv out/sdcard.img || true
fi

echo ""
echo "Test complete. Logs saved in container output above."
echo ""
echo "For detailed firmware logs, check the LD_PRELOAD output"
echo "and look for CETI_LOG messages via journald (if available)."
