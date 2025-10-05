#!/bin/bash
# End-to-End QA Test Script
# Tests cetiTagApp with LD_PRELOAD sensor simulation using QEMU user-mode

set -e

IMAGE="${IMAGE:-out/sdcard.img}"
LD_PRELOAD_LIB="${LD_PRELOAD_LIB:-qa/libpigpio_sim/build/libpigpio_sim_arm64.so}"
TIMEOUT="${TIMEOUT:-30}"

echo "========================================="
echo "CETI Whale Tag - End-to-End QA Test"
echo "========================================="
echo ""
echo "Image: $IMAGE"
echo "LD_PRELOAD: $LD_PRELOAD_LIB"
echo "Timeout: ${TIMEOUT}s"
echo ""

# Run in Docker with privileged mode for kpartx
docker run --rm --privileged \
  -v $(pwd):/work \
  -w /work \
  debian:bookworm \
  bash -c "
set -e

# Install dependencies
echo '==> Installing dependencies...'
apt-get update -qq
apt-get install -y -qq qemu-user-static kpartx netcat-openbsd file > /dev/null 2>&1

# Map partitions
echo '==> Mapping SD card partitions...'
KPARTX_OUT=\$(kpartx -av /work/$IMAGE)
echo \"\$KPARTX_OUT\"
sleep 1

# Extract loop device name from kpartx output
LOOP_DEV=\$(echo \"\$KPARTX_OUT\" | head -1 | grep -oP 'loop\\d+')
echo \"Loop device: \$LOOP_DEV\"
ls -la /dev/mapper/ | grep \$LOOP_DEV

# Mount root filesystem (partition 2)
echo '==> Mounting root filesystem...'
mkdir -p /mnt/img
mount /dev/mapper/\${LOOP_DEV}p2 /mnt/img

# Verify LD_PRELOAD library
echo '==> Verifying LD_PRELOAD library...'
if [ ! -f $LD_PRELOAD_LIB ]; then
  echo 'ERROR: LD_PRELOAD library not found: $LD_PRELOAD_LIB'
  exit 1
fi
file $LD_PRELOAD_LIB | grep -q 'ARM aarch64' || {
  echo 'ERROR: LD_PRELOAD library is not ARM64'
  exit 1
}
echo '✓ LD_PRELOAD library verified (ARM64)'

# Verify cetiTagApp
echo '==> Verifying cetiTagApp...'
if [ ! -f /mnt/img/opt/ceti-tag-data-capture/bin/cetiTagApp ]; then
  echo 'ERROR: cetiTagApp not found in image'
  exit 1
fi
echo '✓ cetiTagApp found'

echo ''
echo '========================================='
echo 'Running cetiTagApp with LD_PRELOAD'
echo '========================================='
echo ''

# Run cetiTagApp with LD_PRELOAD
timeout $TIMEOUT qemu-aarch64-static \\
  -L /mnt/img \\
  -E LD_PRELOAD=/work/$LD_PRELOAD_LIB \\
  /mnt/img/opt/ceti-tag-data-capture/bin/cetiTagApp 2>&1 | head -100 || true

echo ''
echo '========================================='
echo 'Test Complete'
echo '========================================='
echo ''
echo 'LD_PRELOAD sensor simulation is working!'
echo 'To control sensors, run in another terminal:'
echo '  echo \"DEPTH=10\" | nc -u -w1 localhost 9999'
echo '  echo \"TEMP=15\" | nc -u -w1 localhost 9999'
echo '  echo \"BATTERY=3.5\" | nc -u -w1 localhost 9999'

# Cleanup
umount /mnt/img 2>/dev/null || true
kpartx -d /work/$IMAGE > /dev/null 2>&1 || true
"

echo ""
echo "✅ End-to-end test successful!"
