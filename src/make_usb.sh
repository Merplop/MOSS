#!/bin/bash
#
# MOSS USB Drive Preparation Script
#
# Creates a bootable USB drive with GRUB + MOSS kernel + populated ext2 ramdisk.
# The ext2 filesystem is loaded as a GRUB multiboot module into RAM, so it works
# on any hardware (no ATA/USB driver required for disk access after boot).
#
# The ramdisk is pre-populated with: TCC compiler, GNU coreutils, bash, userlibc,
# headers, and support files — everything needed for a working MOSS environment.
#
# Usage: sudo ./make_usb.sh /dev/sdX
#
# WARNING: This will DESTROY ALL DATA on the target device!
#

set -e

RAMDISK_SIZE_MB=64  # Size of the ext2 ramdisk image (MiB)

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

if [ "$DEVICE_MB" -lt 128 ]; then
    echo "ERROR: Device is too small ($DEVICE_MB MiB). Need at least 128 MiB."
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOOT_SIZE_MB=256

echo "============================================"
echo "  MOSS USB Drive Preparation"
echo "============================================"
echo ""
echo "Target device: $DEVICE ($DEVICE_MB MiB)"
echo ""
echo "This will create:"
echo "  ${BOOT_SIZE_MB} MiB boot partition with GRUB + MOSS kernel + ${RAMDISK_SIZE_MB} MiB ext2 ramdisk"
echo "  Ramdisk pre-populated with TCC, coreutils, bash, and userlibc"
echo ""
echo "ALL DATA ON $DEVICE WILL BE DESTROYED!"
echo ""
read -p "Type 'yes' to continue: " CONFIRM
if [ "$CONFIRM" != "yes" ]; then
    echo "Aborted."
    exit 0
fi

# Build MOSS first
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
echo "--- Populating ramdisk with userland ---"
RAMDISK_MNT=$(mktemp -d)
mount -o loop "$RAMDISK_IMG" "$RAMDISK_MNT"

# Create directory structure
mkdir -p "$RAMDISK_MNT/bin"
mkdir -p "$RAMDISK_MNT/usr/lib/tcc/include"
mkdir -p "$RAMDISK_MNT/usr/include/sys"
mkdir -p "$RAMDISK_MNT/tmp"
mkdir -p "$RAMDISK_MNT/dev"
mkdir -p "$RAMDISK_MNT/root"
mkdir -p "$RAMDISK_MNT/etc"
chmod 1777 "$RAMDISK_MNT/tmp"

# --- TCC binary ---
TCC_BIN="$SCRIPT_DIR/tinycc/tcc"
if [ -f "$TCC_BIN" ]; then
    echo "  Installing TCC compiler..."
    i686-elf-strip -o "$RAMDISK_MNT/bin/tcc" "$TCC_BIN"
else
    echo "  WARNING: TCC binary not found at $TCC_BIN (skipping)"
fi

# --- libtcc1.a ---
if [ -f "$SCRIPT_DIR/tinycc/libtcc1.a" ]; then
    cp "$SCRIPT_DIR/tinycc/libtcc1.a" "$RAMDISK_MNT/usr/lib/tcc/"
fi

# --- TCC's own include headers (stdarg.h, stddef.h, etc.) ---
if [ -d "$SCRIPT_DIR/tinycc/include" ]; then
    echo "  Installing TCC headers..."
    cp "$SCRIPT_DIR/tinycc/include/"*.h "$RAMDISK_MNT/usr/lib/tcc/include/"
fi

# --- MOSS userlibc headers ---
if [ -d "$SCRIPT_DIR/userlibc/include" ]; then
    echo "  Installing userlibc headers..."
    cp "$SCRIPT_DIR/userlibc/include/"*.h "$RAMDISK_MNT/usr/include/"
    if [ -d "$SCRIPT_DIR/userlibc/include/sys" ]; then
        cp "$SCRIPT_DIR/userlibc/include/sys/"*.h "$RAMDISK_MNT/usr/include/sys/"
    fi
fi

# --- MOSS userlibc libraries + CRT ---
if [ -f "$SCRIPT_DIR/userlibc/libc.a" ]; then
    echo "  Installing libc.a and CRT objects..."
    cp "$SCRIPT_DIR/userlibc/libc.a" "$RAMDISK_MNT/usr/lib/"
    cp "$SCRIPT_DIR/userlibc/crt0.o" "$RAMDISK_MNT/usr/lib/"

    # Create dummy crti.o and crtn.o (TCC expects them)
    cat > /tmp/moss_empty.S << 'EOF'
.section .text
EOF
    i686-elf-gcc -c /tmp/moss_empty.S -o "$RAMDISK_MNT/usr/lib/crti.o"
    i686-elf-gcc -c /tmp/moss_empty.S -o "$RAMDISK_MNT/usr/lib/crtn.o"
    # TCC uses crt1.o on Linux, not crt0.o
    cp "$SCRIPT_DIR/userlibc/crt0.o" "$RAMDISK_MNT/usr/lib/crt1.o"
    rm -f /tmp/moss_empty.S
fi

# --- Linker script ---
if [ -f "$SCRIPT_DIR/userlibc/user.ld" ]; then
    cp "$SCRIPT_DIR/userlibc/user.ld" "$RAMDISK_MNT/usr/lib/tcc/moss.ld"
fi

# --- GNU Coreutils (cross-compiled against musl) ---
COREUTILS_SRC="$SCRIPT_DIR/coreutils/src"
if [ -d "$COREUTILS_SRC" ]; then
    echo "  Installing GNU coreutils..."
    CORE_UTILS="ls cat echo cp mv mkdir rm rmdir ln pwd wc head tail
        touch chmod chown date env id whoami basename dirname
        true false yes sleep test printf seq tr cut sort uniq
        tee readlink realpath mktemp uname expr
        comm join paste fold fmt nl od tac shuf"
    for util in $CORE_UTILS; do
        if [ -f "$COREUTILS_SRC/$util" ]; then
            i686-elf-strip -o "$RAMDISK_MNT/bin/$util" "$COREUTILS_SRC/$util"
        fi
    done
    # Also install [ as a link to test
    if [ -f "$RAMDISK_MNT/bin/test" ]; then
        cp "$RAMDISK_MNT/bin/test" "$RAMDISK_MNT/bin/["
    fi
    echo "  $(ls "$RAMDISK_MNT/bin" | wc -l) utilities installed"
else
    echo "  WARNING: coreutils not found at $COREUTILS_SRC"
fi

# --- mpad (text editor) ---
MPAD_BIN="$SCRIPT_DIR/mpad"
if [ -f "$MPAD_BIN" ]; then
    echo "  Installing mpad..."
    i686-elf-strip -o "$RAMDISK_MNT/bin/mpad" "$MPAD_BIN"
else
    echo "  WARNING: mpad binary not found at $MPAD_BIN"
fi

# --- Bash ---
BASH_BIN="$SCRIPT_DIR/bash/bash"
if [ -f "$BASH_BIN" ]; then
    echo "  Installing bash..."
    i686-elf-strip -o "$RAMDISK_MNT/bin/bash" "$BASH_BIN"
    cp "$RAMDISK_MNT/bin/bash" "$RAMDISK_MNT/bin/sh"
else
    echo "  WARNING: bash binary not found at $BASH_BIN"
fi

# --- GUI ---
GUI_BIN="$SCRIPT_DIR/gui"
if [ -f "$GUI_BIN" ]; then
    echo "  Installing gui..."
    i686-elf-strip -o "$RAMDISK_MNT/bin/gui" "$GUI_BIN"
else
    echo "  WARNING: gui binary not found at $GUI_BIN"
fi

# --- Neofetch ---
NEOFETCH_BIN="$SCRIPT_DIR/neofetch"
if [ -f "$NEOFETCH_BIN" ]; then
    echo "  Installing neofetch..."
    cp "$NEOFETCH_BIN" "$RAMDISK_MNT/bin/neofetch"
    chmod 755 "$RAMDISK_MNT/bin/neofetch"
else
    echo "  WARNING: neofetch not found at $NEOFETCH_BIN"
fi

# --- Simple test script (verifies shebang execution) ---
cat > "$RAMDISK_MNT/bin/test_sh" << 'TESTSH'
#!/bin/bash
echo "shebang works!"
TESTSH
chmod 755 "$RAMDISK_MNT/bin/test_sh"

# --- Root home directory ---
cat > "$RAMDISK_MNT/root/.bashrc" << 'BASHRC'
export PS1='\[\033[01;32m\]\u@\h\[\033[00m\]:\[\033[01;34m\]\w\[\033[00m\]\$ '
export PATH=/bin:/usr/bin

# Enable color support
export LS_COLORS='rs=0:di=01;34:ln=01;36:mh=00:pi=40;33:so=01;35:do=01;35:bd=40;33;01:cd=40;33;01:or=40;31;01:mi=00:su=37;41:sg=30;43:ca=00:tw=30;42:ow=34;42:st=37;44:ex=01;32:*.tar=01;31:*.gz=01;31:*.bz2=01;31:*.xz=01;31:*.zip=01;31:*.jpg=01;35:*.png=01;35:*.gif=01;35:*.bmp=01;35:*.c=00;33:*.h=00;33:*.o=01;30'
alias ls='ls --color=auto'
alias dir='dir --color=auto'
BASHRC

cat > "$RAMDISK_MNT/root/.profile" << 'PROFILE'
[ -f ~/.bashrc ] && . ~/.bashrc
PROFILE

# --- /etc files ---
cat > "$RAMDISK_MNT/etc/passwd" << 'PASSWD'
root:x:0:0:root:/root:/bin/bash
PASSWD

cat > "$RAMDISK_MNT/etc/group" << 'GROUP'
root:x:0:root
GROUP

cat > "$RAMDISK_MNT/etc/shells" << 'SHELLS'
/bin/bash
/bin/sh
SHELLS

sync
umount "$RAMDISK_MNT"
rmdir "$RAMDISK_MNT"
echo "  Ramdisk populated successfully"

echo ""
echo "--- Unmounting any existing partitions on target ---"
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

BOOT_SECTORS=$((BOOT_SIZE_MB * 2048))

echo ""
echo "--- Creating partition table ---"
sfdisk "$DEVICE" <<EOF
label: dos
unit: sectors

${DEVICE}1 : start=2048, size=${BOOT_SECTORS}, type=83, bootable
EOF

partprobe "$DEVICE" 2>/dev/null || true
sleep 2

PART1="${DEVICE}1"
if [ ! -b "$PART1" ]; then
    PART1="${DEVICE}p1"
fi

if [ ! -b "$PART1" ]; then
    echo "ERROR: Partition not found ($PART1)."
    rm -f "$RAMDISK_IMG"
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
echo "as a multiboot module. The MOSS kernel mounts it as a ramdisk."
echo ""
echo "Installed userland:"
echo "  - TCC compiler (/bin/tcc)"
echo "  - GNU coreutils (/bin/ls, cat, cp, ...)"
echo "  - Bash shell (/bin/bash, /bin/sh)"
echo "  - userlibc headers + libraries (/usr/include, /usr/lib)"
echo ""
echo "Note: The ramdisk lives in RAM — changes are lost on reboot."
echo "      If the machine has an ATA/SATA disk, use 'mkfs' + 'mount'"
echo "      in the MOSS shell for persistent storage."
echo ""
echo "BIOS must be set to Legacy/CSM boot mode (not UEFI)."
echo ""

sync
echo "Safe to remove the USB drive."
