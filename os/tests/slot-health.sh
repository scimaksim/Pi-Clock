#!/bin/sh
# Exercise the OpenRC start function with a temporary boot record.
set -eu
. ./os/overlay/etc/init.d/pi-clock-health-check
test_dir=$(mktemp -d)
trap 'rm -f "$test_dir/slot.conf" "$test_dir/slot.conf.tmp"; rmdir "$test_dir"' EXIT
SLOT_CONF="$test_dir/slot.conf"
ebegin() { :; }
einfo() { :; }
ewarn() { :; }
eend() { return "$1"; }
sleep() { :; }
sync() { :; }
pgrep() { [ "$renderer_up" = yes ]; }
ip() { [ "$network_up" = yes ] && echo '    inet 192.0.2.1/24'; }

cat > "$SLOT_CONF" <<'EOF'
ACTIVE_SLOT=B
PREVIOUS_SLOT=A
PENDING=true
BOOT_TRIES=1
SLOT_A_VERSION="old"
SLOT_B_VERSION="new"
EOF

renderer_up=no
network_up=yes
if start; then echo "Accepted slot without renderer" >&2; exit 1; fi
grep -qx PENDING=true "$SLOT_CONF"
grep -qx BOOT_TRIES=1 "$SLOT_CONF"
renderer_up=yes
network_up=no
start
grep -qx PENDING=false "$SLOT_CONF"
grep -qx BOOT_TRIES=0 "$SLOT_CONF"
