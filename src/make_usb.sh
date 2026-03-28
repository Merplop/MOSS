#!/bin/bash
#
# MOSS USB Drive Preparation Script
#
# Creates a bootable USB drive with GRUB + MOSS kernel + ext2 ramdisk.
# The ext2 filesystem is loaded as a GRUB module into RAM, so it works
# on any hardware (no ATA/USB driver required for disk access).
#
# Usage: sudo ./make_usb.sh /dev/sdX
#
# WARNING: This will DESTROY ALL DATA on the target device!
#

set -e

RAMDISK_SIZE_MB=8   # Size of the ext2 ramdisk image (MiB)

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: This script must be run as root (sudo)."
    exit 1
fi

if [ -z "$1" ]; then
    echo "Usage: sudo $0 /dev/sdX"
    echo ""
    echo "Available removable block devices:"
    lsblk -d -o NAME,SIZE,TRAN,RM | grep -E '^\s*sd|^\s*NAME' | head -20
    exit 1
fi

DEVICE="$1"

# Safety checks
if [ ! -b "$DEVICE" ]; then
    echo "ERROR: $DEVICE is not a block device."
    exit 1
fi

# Check it's a whole disk, not a partition
if echo "$DEVICE" | grep -qE '[0-9]$'; then
    echo "ERROR: Specify the whole disk (e.g., /dev/sdb), not a partition."
    exit 1
fi

DEVICE_SIZE=$(blockdev --getsize64 "$DEVICE" 2>/dev/null || echo 0)
DEVICE_MB=$((DEVICE_SIZE / 1048576))

if [ "$DEVICE_MB" -lt 64 ]; then
    echo "ERROR: Device is too small ($DEVICE_MB MiB). Need at least 64 MiB."
    exit 1
fi

echo "============================================"
echo "  MOSS USB Drive Preparation"
echo "============================================"
echo ""
echo "Target device: $DEVICE ($DEVICE_MB MiB)"
echo ""
echo "This will create:"
echo "  Single partition with GRUB + MOSS kernel + ${RAMDISK_SIZE_MB} MiB ext2 ramdisk"
echo ""
echo "ALL DATA ON $DEVICE WILL BE DESTROYED!"
echo ""
read -p "Type 'yes' to continue: " CONFIRM
if [ "$CONFIRM" != "yes" ]; then
    echo "Aborted."
    exit 0
fi

# Build MOSS first
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
echo ""
echo "--- Building MOSS kernel ---"
cd "$SCRIPT_DIR"
. ./build.sh

KERNEL="$SCRIPT_DIR/sysroot/boot/moss.kernel"
if [ ! -f "$KERNEL" ]; then
    echo "ERROR: Kernel not found at $KERNEL"
    exit 1
fi

echo ""
echo "--- Creating ext2 ramdisk image ---"
RAMDISK_IMG="$SCRIPT_DIR/moss_ramdisk.img"
dd if=/dev/zero of="$RAMDISK_IMG" bs=1M count=$RAMDISK_SIZE_MB 2>/dev/null
# Format with MOSS-compatible settings: 1024-byte blocks, MOSS volume label
mkfs.ext2 -b 1024 -L MOSS -q "$RAMDISK_IMG"
echo "Created ${RAMDISK_SIZE_MB} MiB ext2 ramdisk image"

echo ""
echo "--- Unmounting any existing partitions ---"
umount "${DEVICE}"* 2>/dev/null || true

echo ""
echo "--- Wiping device signatures ---"
wipefs --all --force "$DEVICE" 2>/dev/null || true
dd if=/dev/zero of="$DEVICE" bs=1M count=1 conv=notrunc 2>/dev/null
DEVICE_SECTORS=$(blockdev --getsz "$DEVICE")
if [ "$DEVICE_SECTORS" -gt 34 ]; then
    dd if=/dev/zero of="$DEVICE" bs=512 count=34 \
       seek=$((DEVICE_SECTORS - 34)) conv=notrunc 2>/dev/null
fi

echo ""
echo "--- Creating partition table ---"
sfdisk "$DEVICE" <<EOF
label: dos
unit: sectors

${DEVICE}1 : start=2048, type=83, bootable
EOF

partprobe "$DEVICE" 2>/dev/null || true
sleep 2

PART1="${DEVICE}1"
if [ ! -b "$PART1" ]; then
    PART1="${DEVICE}p1"
fi

if [ ! -b "$PART1" ]; then
    echo "ERROR: Partition not found ($PART1)."
    exit 1
fi

echo ""
echo "--- Formatting boot partition ---"
mkfs.ext2 -L MOSSBOOT -b 4096 -q "$PART1"

echo ""
echo "--- Installing GRUB, kernel, and ramdisk ---"
MOUNT_DIR=$(mktemp -d)

mount "$PART1" "$MOUNT_DIR"
mkdir -p "$MOUNT_DIR/boot/grub"

cp "$KERNEL" "$MOUNT_DIR/boot/moss.kernel"
cp "$RAMDISK_IMG" "$MOUNT_DIR/boot/moss_disk.img"

cat > "$MOUNT_DIR/boot/grub/grub.cfg" <<GRUBEOF
set timeout=3
set default=0

menuentry "MOSS" {
    set gfxpayload=1920x1080x32
    multiboot /boot/moss.kernel
    module /boot/moss_disk.img
}
GRUBEOF

grub-install --target=i386-pc --boot-directory="$MOUNT_DIR/boot" --force "$DEVICE"

umount "$MOUNT_DIR"
rmdir "$MOUNT_DIR"

# Clean up temp ramdisk image
rm -f "$RAMDISK_IMG"

echo ""
echo "============================================"
echo "  USB drive ready!"
echo "============================================"
echo ""
echo "GRUB will load the ${RAMDISK_SIZE_MB} MiB ext2 image into RAM"
echo "as a multiboot module. The MOSS kernel uses it as a ramdisk."
echo ""
echo "Note: Changes to the filesystem live in RAM only and"
echo "will be lost on reboot, unless you also have an ATA disk."
echo ""
echo "BIOS must be set to Legacy/CSM boot mode (not UEFI)."
echo ""

sync
echo "Safe to remove the USB drive."
