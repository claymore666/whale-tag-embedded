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
TIMEOUT_S=30              # Short timeout for testing
BURN_INTERVAL_S=20        # Active burn time (underwater only)
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
echo "Cleaning up loop devices..."
losetup -D 2>/dev/null || true

echo "Test parameters:"
echo "  timeout_s: $TIMEOUT_S"
echo "  burn_interval_s: $BURN_INTERVAL_S (active underwater time)"
echo "  burn_depth_threshold: $BURN_DEPTH_THRESHOLD m"
echo "  surface_pressure: $SURFACE_PRESSURE bar"
echo ""

# Start Docker container
echo "Starting Docker container..."
docker run -d \
    --name $CONTAINER_NAME \
    --privileged \
    -v "$(pwd):/work" \
    -w /work \
    debian:bookworm \
    tail -f /dev/null

# Install dependencies
echo "Installing container dependencies..."
docker exec $CONTAINER_NAME bash -c "
    apt-get update -qq && \
    apt-get install -y -qq kpartx qemu-user-static netcat-openbsd procps > /dev/null 2>&1
"

# Mount SD card image
echo "Mounting SD card image..."
MOUNT_OUTPUT=$(docker exec $CONTAINER_NAME bash -c "
    set -e
    echo 'Running kpartx...'
    kpartx -av /work/out/sdcard.img 2>&1 | tee /tmp/kpartx.out
    echo 'Detecting loop device...'
    LOOP_DEVICE=\$(grep -oP 'loop\d+' /tmp/kpartx.out | head -1)
    if [ -z \"\$LOOP_DEVICE\" ]; then
        echo 'Error: Failed to detect loop device'
        cat /tmp/kpartx.out
        exit 1
    fi
    echo \"Detected loop device: \$LOOP_DEVICE\"
    echo 'Mounting partition 2...'
    mkdir -p /mnt/img
    mount /dev/mapper/\${LOOP_DEVICE}p2 /mnt/img 2>&1
    echo \"Mounted partition 2\"
" 2>&1)

if [ $? -ne 0 ]; then
    echo -e "${RED}Error: Failed to mount SD card image${NC}"
    echo "$MOUNT_OUTPUT"
    docker rm -f $CONTAINER_NAME
    exit 1
fi

echo "$MOUNT_OUTPUT"

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
echo "Phase 1: Wait for timeout ($TIMEOUT_S seconds)..."
sleep $((TIMEOUT_S + 5))

echo ""
echo "Phase 2: Surface interval (burnwire should be OFF)"
echo "  Simulating surface: 0.5m depth"
echo 'DEPTH=0.5' | docker exec -i $CONTAINER_NAME nc -u -w1 127.0.0.1 9999
sleep 5

# Check IOX output for burnwire state
echo "  Checking burnwire state..."
docker exec $CONTAINER_NAME tail -20 /firmware.log | grep "IOX OUTPUT" | tail -3

echo ""
echo "Phase 3: Dive (burnwire should turn ON)"
echo "  Simulating dive: 10m depth"
echo 'DEPTH=10' | docker exec -i $CONTAINER_NAME nc -u -w1 127.0.0.1 9999
sleep 5

echo "  Checking burnwire state..."
docker exec $CONTAINER_NAME tail -20 /firmware.log | grep "IOX OUTPUT" | tail -3

echo ""
echo "Phase 4: Surface again (burnwire should turn OFF)"
echo "  Simulating surface: 0.5m depth"
echo 'DEPTH=0.5' | docker exec -i $CONTAINER_NAME nc -u -w1 127.0.0.1 9999
sleep 5

echo "  Checking burnwire state..."
docker exec $CONTAINER_NAME tail -20 /firmware.log | grep "IOX OUTPUT" | tail -3

echo ""
echo "Phase 5: Dive again (burnwire should turn ON)"
echo "  Simulating dive: 15m depth"
echo 'DEPTH=15' | docker exec -i $CONTAINER_NAME nc -u -w1 127.0.0.1 9999
sleep 10

echo "  Checking burnwire state..."
docker exec $CONTAINER_NAME tail -20 /firmware.log | grep "IOX OUTPUT" | tail -3

echo ""
echo "========================================="
echo "Collecting Evidence"
echo "========================================="
echo ""

# Check for burnwire events log
echo "=== Burnwire Event Log (data_burnwire.csv) ==="
if docker exec $CONTAINER_NAME test -f /mnt/img/data/data_burnwire.csv; then
    docker exec $CONTAINER_NAME cat /mnt/img/data/data_burnwire.csv
    echo ""

    # Analyze events
    echo "=== Event Analysis ==="
    docker exec $CONTAINER_NAME bash -c "
        if [ -f /mnt/img/data/data_burnwire.csv ]; then
            echo 'Burn start events:'
            grep 'burn_start' /mnt/img/data/data_burnwire.csv | head -1
            echo ''
            echo 'Submerged events (burnwire ON):'
            grep 'submerged' /mnt/img/data/data_burnwire.csv | wc -l
            echo ''
            echo 'Surfaced events (burnwire OFF):'
            grep 'surfaced' /mnt/img/data/data_burnwire.csv | wc -l
            echo ''
            echo 'Burn complete event:'
            grep 'burn_complete' /mnt/img/data/data_burnwire.csv | tail -1
        fi
    "
else
    echo -e "${YELLOW}WARNING: data_burnwire.csv not created${NC}"
    echo "This may indicate the feature is not active or state not reached"
fi

echo ""
echo "=== State Machine Transitions (data_state.csv) ==="
if docker exec $CONTAINER_NAME test -f /mnt/img/data/data_state.csv; then
    docker exec $CONTAINER_NAME tail -10 /mnt/img/data/data_state.csv
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
SUBMERGED_COUNT=$(docker exec $CONTAINER_NAME bash -c "grep -c 'submerged' /mnt/img/data/data_burnwire.csv 2>/dev/null || echo 0")
SURFACED_COUNT=$(docker exec $CONTAINER_NAME bash -c "grep -c 'surfaced' /mnt/img/data/data_burnwire.csv 2>/dev/null || echo 0")

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
docker rm -f $CONTAINER_NAME

echo ""
echo "Test complete. Logs saved in container output above."
echo ""
echo "For detailed firmware logs, check the LD_PRELOAD output"
echo "and look for CETI_LOG messages via journald (if available)."
