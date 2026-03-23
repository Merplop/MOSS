#!/bin/sh
set -e
. ./iso.sh

qemu-system-$(./target-triplet-to-arch.sh $HOST) -audiodev pa,id=speaker -machine pcspk-audiodev=speaker -cdrom moss.iso
