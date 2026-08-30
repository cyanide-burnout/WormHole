#!/bin/bash
#
# Takes back everything install.sh put in place.

set -euo pipefail

PREFIX=${PREFIX:-/usr/local}
UNITDIR=/etc/systemd/system
CONFDIR=/etc/ivshmem-server
UNIT=ivshmem-server
WORK=${WORK:-/var/cache/ivshmem-server}
MARKER=.ivshmem-server-build

PURGE=no

usage()
{
  cat <<EOF
Usage: uninstall.sh [--prefix DIR] [--purge]

  --purge   also drop $CONFDIR and the build tree in $WORK
EOF
  exit "${1:-0}"
}

while [ $# -gt 0 ]; do
  case $1 in
    --prefix) PREFIX=$2; shift 2 ;;
    --purge)  PURGE=yes; shift ;;
    -h|--help) usage ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

[ "$(id -u)" = 0 ] || { echo "run it under sudo" >&2; exit 1; }

for file in "$CONFDIR"/*.conf; do
  [ -e "$file" ] || continue
  instance=$(basename "$file" .conf)
  echo "stopping $UNIT@$instance"
  systemctl disable --now "$UNIT@$instance" 2>/dev/null || true
done

rm -f "$UNITDIR/ivshmem-server@.service"
systemctl daemon-reload

rm -f "$PREFIX/bin/ivshmem-server" "$PREFIX/bin/ivshmem-client" \
      "$PREFIX/sbin/ivshmem-server-ctl"

# only a hook that is ours
for hook in /etc/libvirt/hooks/qemu /etc/libvirt/hooks/qemu.ivshmem; do
  if [ -e "$hook" ] && grep -q 'ivshmem-server hook' "$hook"; then
    rm -f "$hook"
    echo "removed $hook"
  fi
done

if [ "$PURGE" = yes ]; then
  rm -rf "$CONFDIR"
  echo "removed $CONFDIR"

  # WORK can be said from the environment, and rm -rf of whatever it happens to
  # hold is not something to do on trust: only a tree install.sh marked as its
  # own, and install.sh marks nothing it did not make or find empty
  case $WORK in
    /) echo "the root is no place to remove anything from; left alone"; exit 0 ;;
    /*) ;;
    *) echo "$WORK is not an absolute path; left alone"; exit 0 ;;
  esac

  if [ -f "$WORK/$MARKER" ]; then
    rm -rf "$WORK"
    echo "removed $WORK"
  elif [ -e "$WORK" ]; then
    echo "$WORK bears no mark of install.sh having built there; left alone"
  fi
else
  echo "$CONFDIR kept, --purge to drop it"
fi
