#!/bin/bash
# Expand SD card image for QA testing
# Adds 1GB to the image and resizes the /data partition

set -e

IMAGE="${1:-out/sdcard.img}"
EXPAND_SIZE="1G"

if [ ! -f "$IMAGE" ]; then
    echo "Error: Image file not found: $IMAGE"
    exit 1
fi

echo "========================================="
echo "Expanding SD Card Image for QA Testing"
echo "========================================="
echo "Image: $IMAGE"
echo "Expansion: $EXPAND_SIZE"
echo ""

# Create backup
BACKUP="${IMAGE}.backup"
if [ -f "$BACKUP" ]; then
    echo "Backup already exists: $BACKUP"
    echo "Skipping backup creation (remove $BACKUP to force new backup)"
else
    echo "Creating backup: $BACKUP"
    cp "$IMAGE" "$BACKUP"
    echo "Backup created successfully"
fi
echo ""

# Check original size
ORIGINAL_SIZE=$(stat -c%s "$IMAGE")
echo "Original size: $(numfmt --to=iec-i --suffix=B $ORIGINAL_SIZE)"

# Add 1GB to the image
echo "Adding $EXPAND_SIZE to image..."
truncate -s +$EXPAND_SIZE "$IMAGE"

NEW_SIZE=$(stat -c%s "$IMAGE")
echo "New size: $(numfmt --to=iec-i --suffix=B $NEW_SIZE)"
echo ""

# Use Docker container for partition operations
echo "Resizing partition and filesystem..."
docker run --rm --privileged -v $(pwd):/work -w /work debian:bookworm bash -c '
set -e

IMAGE="'$IMAGE'"

# Install required tools
apt-get update -qq && apt-get install -y -qq kpartx e2fsprogs parted cloud-guest-utils > /dev/null 2>&1

# Map partitions
KPARTX_OUT=$(kpartx -av "$IMAGE")
LOOP=$(echo "$KPARTX_OUT" | head -1 | grep -oP "loop\d+")
echo "Mapped to $LOOP"

# Get partition info
echo "Current partition table:"
parted /dev/$LOOP unit s print

# Identify which partition is the data partition (partition 3)
echo ""
echo "Checking partition labels..."
e2label /dev/mapper/${LOOP}p3 2>/dev/null && echo "Partition 3 is the data partition" || true

# Use growpart to resize partition 3 to fill available space
echo ""
echo "Growing partition 3 (data partition) to use all available space..."
growpart /dev/$LOOP 3

# Inform kernel of partition changes
partprobe /dev/$LOOP 2>/dev/null || true

# Reload partition table
kpartx -d "$IMAGE" 2>/dev/null || true
sleep 1
KPARTX_OUT=$(kpartx -av "$IMAGE")
LOOP=$(echo "$KPARTX_OUT" | head -1 | grep -oP "loop\d+")
echo "Remapped to $LOOP"

# Resize filesystem on partition 3
echo "Resizing filesystem on /dev/mapper/${LOOP}p3..."
e2fsck -f -y /dev/mapper/${LOOP}p3
resize2fs /dev/mapper/${LOOP}p3

# Show new size
echo ""
echo "New filesystem size:"
tune2fs -l /dev/mapper/${LOOP}p3 | grep "Block count"

# Cleanup
kpartx -d "$IMAGE"

echo "Done!"
'

echo ""
echo "========================================="
echo "Image expansion complete!"
echo "========================================="
echo "The /data partition has been expanded by $EXPAND_SIZE"
