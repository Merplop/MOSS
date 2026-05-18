#!/bin/bash
#
# populate_disk.sh — Copy TCC and support files into the MOSS ext2 disk image.
#
# Creates the directory structure that TCC expects:
#   /usr/lib/tcc/          — libtcc1.a, include/
#   /usr/lib/tcc/include/  — TCC's own headers (stdarg.h, stddef.h, etc.)
#   /usr/include/          — MOSS userlibc headers
#   /usr/lib/              — crt0.o, libc.a, crti.o, crtn.o
#   /bin/tcc               — the TCC binary
#
# Usage: sudo ./populate_disk.sh [disk_image]
#        Default disk_image: moss_disk.img
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DISK_IMG="${1:-$SCRIPT_DIR/moss_disk.img}"

if [ ! -f "$DISK_IMG" ]; then
    echo "Disk image not found: $DISK_IMG"
    echo "Run qemu.sh first to create it, or specify path."
    exit 1
fi

MOUNT_DIR=$(mktemp -d)
trap "umount '$MOUNT_DIR' 2>/dev/null; rmdir '$MOUNT_DIR' 2>/dev/null" EXIT

echo "Mounting $DISK_IMG ..."
mount -o loop "$DISK_IMG" "$MOUNT_DIR"

echo "Creating directory structure ..."
mkdir -p "$MOUNT_DIR/bin"
mkdir -p "$MOUNT_DIR/usr/lib/tcc/include"
mkdir -p "$MOUNT_DIR/usr/include/sys"
mkdir -p "$MOUNT_DIR/tmp"
mkdir -p "$MOUNT_DIR/dev"
mkdir -p "$MOUNT_DIR/root"
mkdir -p "$MOUNT_DIR/etc"
chmod 1777 "$MOUNT_DIR/tmp"

# --- TCC binary ---
echo "Copying TCC binary ..."
TCC_BIN="$SCRIPT_DIR/tinycc/tcc"
if [ ! -f "$TCC_BIN" ]; then
    echo "ERROR: TCC binary not found. Run: cd tinycc && make -f Makefile.moss"
    exit 1
fi
# Strip debug symbols to save space
i686-elf-strip -o "$MOUNT_DIR/bin/tcc" "$TCC_BIN"

# --- libtcc1.a ---
echo "Copying libtcc1.a ..."
cp "$SCRIPT_DIR/tinycc/libtcc1.a" "$MOUNT_DIR/usr/lib/tcc/"

# --- TCC's own include headers (stdarg.h, stddef.h, etc.) ---
echo "Copying TCC include headers ..."
cp "$SCRIPT_DIR/tinycc/include/"*.h "$MOUNT_DIR/usr/lib/tcc/include/"

# --- MOSS userlibc headers ---
echo "Copying MOSS userlibc headers ..."
cp "$SCRIPT_DIR/userlibc/include/"*.h "$MOUNT_DIR/usr/include/"
cp "$SCRIPT_DIR/userlibc/include/sys/"*.h "$MOUNT_DIR/usr/include/sys/"

# --- MOSS userlibc libraries + CRT ---
echo "Copying libc.a and CRT objects ..."
cp "$SCRIPT_DIR/userlibc/libc.a" "$MOUNT_DIR/usr/lib/"
cp "$SCRIPT_DIR/userlibc/crt0.o" "$MOUNT_DIR/usr/lib/"

# Create dummy crti.o and crtn.o (TCC's default Linux linker expects them)
# They're empty — MOSS doesn't need constructor/destructor sections
echo "Creating dummy crti.o and crtn.o ..."
cat > /tmp/moss_empty.S << 'EOF'
.section .text
EOF
i686-elf-gcc -c /tmp/moss_empty.S -o "$MOUNT_DIR/usr/lib/crti.o"
i686-elf-gcc -c /tmp/moss_empty.S -o "$MOUNT_DIR/usr/lib/crtn.o"
# Also create a dummy crt1.o that just jumps to crt0's _start
# (TCC uses crt1.o on Linux, not crt0.o)
cp "$SCRIPT_DIR/userlibc/crt0.o" "$MOUNT_DIR/usr/lib/crt1.o"
rm -f /tmp/moss_empty.S

# --- Copy the user linker script ---
echo "Copying linker script ..."
cp "$SCRIPT_DIR/userlibc/user.ld" "$MOUNT_DIR/usr/lib/tcc/moss.ld"

# --- GNU Coreutils (cross-compiled against musl) ---
echo "Copying GNU coreutils ..."
COREUTILS_SRC="$SCRIPT_DIR/coreutils/src"
if [ -d "$COREUTILS_SRC" ]; then
    CORE_UTILS="ls cat echo cp mv mkdir rm rmdir ln pwd wc head tail
        touch chmod chown date env id whoami basename dirname
        true false yes sleep test printf seq tr cut sort uniq
        tee readlink realpath mktemp uname expr
        comm join paste fold fmt nl od tac shuf"
    for util in $CORE_UTILS; do
        if [ -f "$COREUTILS_SRC/$util" ]; then
            i686-elf-strip -o "$MOUNT_DIR/bin/$util" "$COREUTILS_SRC/$util"
        fi
    done
    # Also install [ as a link to test
    if [ -f "$MOUNT_DIR/bin/test" ]; then
        cp "$MOUNT_DIR/bin/test" "$MOUNT_DIR/bin/["
    fi
    echo "  $(ls "$MOUNT_DIR/bin" | wc -l) utilities installed"
else
    echo "  WARNING: coreutils not found at $COREUTILS_SRC"
fi

# --- Bash ---
echo "Copying bash ..."
BASH_BIN="$SCRIPT_DIR/bash/bash"
if [ -f "$BASH_BIN" ]; then
    i686-elf-strip -o "$MOUNT_DIR/bin/bash" "$BASH_BIN"
    # Also provide /bin/sh as a symlink/copy for scripts
    cp "$MOUNT_DIR/bin/bash" "$MOUNT_DIR/bin/sh"
    echo "  bash installed ($(du -h "$MOUNT_DIR/bin/bash" | cut -f1))"
else
    echo "  WARNING: bash binary not found at $BASH_BIN"
fi

# --- Root home directory ---
echo "Creating /root/.bashrc ..."
cat > "$MOUNT_DIR/root/.bashrc" << 'BASHRC'
export PS1='\u@\h:\w\$ '
export PATH=/bin:/usr/bin
BASHRC

cat > "$MOUNT_DIR/root/.profile" << 'PROFILE'
[ -f ~/.bashrc ] && . ~/.bashrc
PROFILE

# --- /etc files ---
cat > "$MOUNT_DIR/etc/passwd" << 'PASSWD'
root:x:0:0:root:/root:/bin/bash
PASSWD

cat > "$MOUNT_DIR/etc/group" << 'GROUP'
root:x:0:root
GROUP

cat > "$MOUNT_DIR/etc/shells" << 'SHELLS'
/bin/bash
/bin/sh
SHELLS

echo "Syncing ..."
sync

echo ""
echo "Disk image populated successfully!"
echo "Files on disk:"
find "$MOUNT_DIR" -not -path '*/lost+found*' -not -path "$MOUNT_DIR" | \
    sed "s|$MOUNT_DIR||" | sort
echo ""
echo "Usage on MOSS:"
echo "  exec bin/tcc -nostdlib -o hello /usr/lib/crt0.o hello.c /usr/lib/libc.a"
