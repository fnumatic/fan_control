#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Install this working tree as a DKMS module and load it.
# Run from the repository root with sudo.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
V=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$ROOT/module/dkms.conf")
[ -n "$V" ] || { echo "no PACKAGE_VERSION in module/dkms.conf" >&2; exit 1; }

DST=/usr/src/minisforum-ec-$V
KDIR=/lib/modules/$(uname -r)/build
mkdir -p "$DST"
cp -f "$ROOT"/module/Kbuild "$ROOT"/module/Makefile "$ROOT"/module/dkms.conf \
      "$ROOT"/module/minisforum_ec.c "$DST"/

# Compile once outside DKMS first. DKMS unbuilds the previous build before it
# compiles, so a broken tree must fail while that build is still in place.
make -C "$KDIR" M="$DST" modules >/dev/null
make -C "$KDIR" M="$DST" clean >/dev/null

if ! dkms status -m minisforum-ec -v "$V" | grep -q .; then
	dkms add -m minisforum-ec -v "$V"
fi
dkms build --force -m minisforum-ec -v "$V"
dkms install --force -m minisforum-ec -v "$V"

printf 'minisforum_ec\n' > /etc/modules-load.d/minisforum_ec.conf
depmod -a

# The running module must go, or the new one would not be the code in charge.
if lsmod | grep -q '^minisforum_ec '; then
	modprobe -r minisforum_ec || {
		echo "minisforum_ec is in use; the running module keeps the old code" >&2
		exit 1
	}
fi
modprobe minisforum_ec

# Check the running module, not the file on disk.
got_ver=$(cat /sys/module/minisforum_ec/version)
got_src=$(cat /sys/module/minisforum_ec/srcversion)
want_src=$(modinfo -F srcversion "$(modinfo -n minisforum_ec)")
[ "$got_ver" = "$V" ] || { echo "loaded version $got_ver, expected $V" >&2; exit 1; }
[ "$got_src" = "$want_src" ] || {
	echo "running srcversion $got_src, installed $want_src" >&2
	exit 1
}

sleep 3
sensors 'minisforum_ec-*'
