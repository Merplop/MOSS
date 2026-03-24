#!/bin/sh
set -e
. ./iso.sh

# Create a persistent disk image for ext2 filesystem if it doesn't exist
DISK_IMG="moss_disk.img"
if [ ! -f "$DISK_IMG" ]; then
    echo "Creating 64 MiB disk image for ext2 filesystem..."
    dd if=/dev/zero of="$DISK_IMG" bs=1M count=64 2>/dev/null
fi

qemu-system-$(./target-triplet-to-arch.sh $HOST) -audiodev pa,id=speaker -machine pcspk-audiodev=speaker -cdrom moss.iso -drive file=$DISK_IMG,format=raw,if=ide,index=0
