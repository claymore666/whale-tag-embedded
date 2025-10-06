#!/bin/bash
# Resize filesystem on already-expanded partition 3

set -e

IMAGE="${1:-out/sdcard.img}"

if [ ! -f "$IMAGE" ]; then
    echo "Error: Image file not found: $IMAGE"
    exit 1
fi

echo "========================================="
echo "Resizing Filesystem on Partition 3"
echo "========================================="
echo "Image: $IMAGE"
echo ""

docker run --rm --privileged -v $(pwd):/work -w /work debian:bookworm bash -c '
set -e

IMAGE="'$IMAGE'"

# Install required tools
apt-get update -qq && apt-get install -y -qq kpartx e2fsprogs > /dev/null 2>&1

# Map partitions
KPARTX_OUT=$(kpartx -av "$IMAGE")
LOOP=$(echo "$KPARTX_OUT" | head -1 | grep -oP "loop\d+")
echo "Mapped to $LOOP"

# Check and resize filesystem
echo "Checking filesystem on /dev/mapper/${LOOP}p3..."
e2fsck -f -y /dev/mapper/${LOOP}p3

echo ""
echo "Resizing filesystem to use all available space..."
resize2fs /dev/mapper/${LOOP}p3

# Show new size
echo ""
echo "New filesystem size:"
df -h /dev/mapper/${LOOP}p3 2>/dev/null || tune2fs -l /dev/mapper/${LOOP}p3 | grep "Block count"

# Cleanup
kpartx -d "$IMAGE"

echo ""
echo "Done!"
'

echo ""
echo "========================================="
echo "Filesystem resize complete!"
echo "========================================="
