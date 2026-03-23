#!/bin/sh
set -e
. ./build.sh

mkdir -p isodir
mkdir -p isodir/boot
mkdir -p isodir/boot/grub

cp sysroot/boot/moss.kernel isodir/boot/moss.kernel
cat > isodir/boot/grub/grub.cfg << EOF
set gfxpayload=1920x1080x32

menuentry "moss" {
        multiboot /boot/moss.kernel
}
EOF
grub-mkrescue -o moss.iso isodir
