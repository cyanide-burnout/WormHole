#!/bin/bash
#
# Fetches the QEMU sources, builds ivshmem-server out of contrib/ and installs it
# as a systemd template unit with one instance per shared region.
#
# QEMU builds the server but installs nothing: contrib/ivshmem-server is marked
# install: false in its meson.build, which is why no distribution carries it and
# why the binary has to be taken out of the build tree by hand.

set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

QEMU_VERSION=${QEMU_VERSION:-v11.1.1}
SOURCE=${SOURCE:-}
WORK=${WORK:-/var/cache/ivshmem-server}
PREFIX=${PREFIX:-/usr/local}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}
UNITDIR=/etc/systemd/system
MARKER=.ivshmem-server-build

DEPS=auto
HOOK=no
KEEP=no
BUILD_ONLY=no
INSTANCES=()
NO_INSTANCE=no
INSTANCE_ARGS=()

fail() { echo "install.sh: $*" >&2; exit 1; }
step() { echo; echo "==> $*"; }

# The build tree is a cache root reads back: a tarball found already downloaded
# is not downloaded again and a build.ninja found in place is not configured
# again. Under /var/tmp, which is 1777, that is an invitation to hand root a
# source tree of one's own -- so the directory has to belong to whoever is
# building and be closed to everybody else.
#
# It is also marked, and the mark is what uninstall.sh --purge takes as leave to
# rm -rf the place. That makes leaving a mark the graver of the two things done
# here: --work /etc would pass every test above when root is the one building.
# So a mark is only ever put on a directory this made, or on one that was empty
# when it was taken over -- never on a directory that already holds anything.
secure_work()
{
  local path=$1 owner mode

  case $path in
    /) fail "--work wants a directory, not the root" ;;
    /*) ;;
    *) fail "--work wants an absolute path: $path" ;;
  esac

  [ -L "$path" ] && fail "$path is a symlink"

  if [ ! -d "$path" ]; then
    mkdir -p -m 0700 "$path" 2>/dev/null || \
      fail "cannot make $path -- pass --work with somewhere you can write"
  elif [ ! -f "$path/$MARKER" ] && [ -n "$(ls -A "$path" 2>/dev/null)" ]; then
    fail "$path holds something already and carries no mark of ours;" \
         "pass --work with a directory that is new or empty"
  fi

  owner=$(stat -c %u "$path")
  mode=$(stat -c %a "$path")
  mode=${mode: -3}

  [ "$owner" = "$(id -u)" ] || \
    fail "$path belongs to uid $owner and not to uid $(id -u), which is building"
  case ${mode:1:1}${mode:2:1} in
    *[2367]*) fail "$path is writable by group or other (mode $mode)" ;;
  esac

  : > "$path/$MARKER"
}

usage()
{
  cat <<EOF
Usage: install.sh [options]

  --version TAG      QEMU tag to build from (default $QEMU_VERSION)
  --source URL|DIR   tarball to fetch, or an unpacked QEMU tree to build in place
  --work DIR         where to download and build (default $WORK)
  --prefix DIR       install prefix (default $PREFIX)
  --jobs N           parallel build jobs (default $JOBS)
  --deps|--no-deps   install build dependencies through the package manager
  --build-only       build the binary and stop, installing nothing
  --keep             keep the build tree
  --hook             install the libvirt hook that starts an instance on demand

  --instance NAME    instance to create, may be given more than once
                     (default: one instance named "default")
  --no-instance      install the unit and create no instance

  Passed on to every instance created, see ivshmem-server-ctl add:
  --size S  --vectors N  --socket P  --shm NAME  --directory D
  --owner U  --group G  --mode M  --verbose  --force  --no-start
EOF
  exit "${1:-0}"
}

while [ $# -gt 0 ]; do
  case $1 in
    --version)  QEMU_VERSION=$2; shift 2 ;;
    --source)   SOURCE=$2;       shift 2 ;;
    --work)     WORK=$2;         shift 2 ;;
    --prefix)   PREFIX=$2;       shift 2 ;;
    --jobs|-j)  JOBS=$2;         shift 2 ;;
    --deps)     DEPS=yes;        shift ;;
    --no-deps)  DEPS=no;         shift ;;
    --build-only) BUILD_ONLY=yes; shift ;;
    --keep)     KEEP=yes;        shift ;;
    --hook)     HOOK=yes;        shift ;;
    --instance) INSTANCES+=("$2"); shift 2 ;;
    --no-instance) NO_INSTANCE=yes; shift ;;
    --size|--vectors|--socket|--shm|--directory|--owner|--group|--mode)
                INSTANCE_ARGS+=("$1" "$2"); shift 2 ;;
    --verbose|--force|--no-start)
                INSTANCE_ARGS+=("$1"); shift ;;
    -h|--help)  usage ;;
    *)          fail "unknown option: $1 (try --help)" ;;
  esac
done

[ "$BUILD_ONLY" = yes ] || [ "$(id -u)" = 0 ] || fail "installing needs root, run it under sudo"
[ ${#INSTANCES[@]} -gt 0 ] || [ "$NO_INSTANCE" = yes ] || INSTANCES=(default)

# --- build dependencies -----------------------------------------------------

# iproute2 is not for the build but for ss, which is how an instance tells a
# stale socket from one somebody is serving
APT_PACKAGES="build-essential ninja-build meson python3 python3-venv pkg-config
              libglib2.0-dev zlib1g-dev curl ca-certificates xz-utils iproute2"
DNF_PACKAGES="gcc make ninja-build meson python3 pkgconf-pkg-config
              glib2-devel zlib-devel curl xz iproute"

install_dependencies()
{
  if command -v apt-get >/dev/null 2>&1; then
    step "installing build dependencies"
    DEBIAN_FRONTEND=noninteractive apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y $APT_PACKAGES
  elif command -v dnf >/dev/null 2>&1; then
    step "installing build dependencies"
    dnf install -y $DNF_PACKAGES
  else
    echo "no apt-get and no dnf here; make sure a compiler, meson, ninja," >&2
    echo "pkg-config and the glib and zlib headers are installed" >&2
  fi
}

if [ "$DEPS" = yes ] || { [ "$DEPS" = auto ] && [ "$(id -u)" = 0 ]; }; then
  install_dependencies
fi

# --- sources ----------------------------------------------------------------

secure_work "$WORK"

if [ -d "${SOURCE:-/nowhere}" ]; then
  SOURCEDIR=$(cd "$SOURCE" && pwd)
  step "building in $SOURCEDIR"
else
  URL=${SOURCE:-https://github.com/qemu/qemu/archive/refs/tags/$QEMU_VERSION.tar.gz}
  ARCHIVE=$WORK/$(basename "$URL")

  mkdir -p "$WORK/source"

  if [ -s "$ARCHIVE" ]; then
    step "already downloaded: $ARCHIVE"
  else
    step "downloading $URL"
    if command -v curl >/dev/null 2>&1
      then curl -fL --retry 3 --progress-bar -o "$ARCHIVE.part" "$URL"
      else wget -O "$ARCHIVE.part" "$URL"
    fi
    mv "$ARCHIVE.part" "$ARCHIVE"
  fi

  # unpacked under the name of the archive rather than all into one place, so
  # that two versions asked for in turn cannot be mistaken for one another
  UNPACKED=$WORK/source/$(basename "${ARCHIVE%.tar.*}" .tgz)

  if [ -d "$UNPACKED" ]; then
    step "already unpacked: $UNPACKED"
  else
    step "unpacking"
    rm -rf "$UNPACKED.part"
    mkdir -p "$UNPACKED.part"
    tar -xf "$ARCHIVE" -C "$UNPACKED.part"
    mv "$UNPACKED.part" "$UNPACKED"
  fi

  # the tarball holds one directory, whatever the tag was called
  SOURCEDIR=$(find "$UNPACKED" -maxdepth 1 -mindepth 1 -type d | head -1)
  [ -n "$SOURCEDIR" ] || fail "no directory inside $ARCHIVE"
fi

[ -f "$SOURCEDIR/contrib/ivshmem-server/main.c" ] || \
  fail "$SOURCEDIR does not look like a QEMU tree"

# --- build ------------------------------------------------------------------

BUILDDIR=$WORK/build
STAMP=$BUILDDIR/.configured-from

# A build tree is only a saving as long as it was made from what is being asked
# for now. Left to itself it would answer --version with whatever it was
# configured from the last time and say nothing about it.
WANTED="$SOURCEDIR $PREFIX"

if [ -f "$BUILDDIR/build.ninja" ] && [ "$(cat "$STAMP" 2>/dev/null)" != "$WANTED" ]; then
  step "the tree in $BUILDDIR was configured from something else; starting over"
  rm -rf "$BUILDDIR"
fi

mkdir -p "$BUILDDIR"

if [ ! -f "$BUILDDIR/build.ninja" ]; then
  step "configuring"
  # Everything but the tools is off: the server links against libqemuutil and
  # wants nothing else, and a full QEMU would take an hour to say the same.
  ( cd "$BUILDDIR" && "$SOURCEDIR/configure" \
      --prefix="$PREFIX" \
      --without-default-features \
      --enable-tools \
      --disable-system \
      --disable-user \
      --disable-docs \
      --disable-guest-agent \
      --disable-werror )
  echo "$WANTED" > "$STAMP"
else
  step "already configured in $BUILDDIR from $SOURCEDIR"
fi

step "building"
ninja -C "$BUILDDIR" -j "$JOBS" \
  contrib/ivshmem-server/ivshmem-server \
  contrib/ivshmem-client/ivshmem-client

BINARY=$BUILDDIR/contrib/ivshmem-server/ivshmem-server
CLIENT=$BUILDDIR/contrib/ivshmem-client/ivshmem-client
[ -x "$BINARY" ] || fail "the build left no $BINARY"

if [ "$BUILD_ONLY" = yes ]; then
  echo
  echo "built: $BINARY"
  exit 0
fi

# --- install ----------------------------------------------------------------

step "installing into $PREFIX"

install -D -m 0755 "$BINARY" "$PREFIX/bin/ivshmem-server"
if [ -x "$CLIENT" ]; then
  install -D -m 0755 "$CLIENT" "$PREFIX/bin/ivshmem-client"
fi

# both the unit and the control script carry the prefix they were installed under
TEMPORARY=$(mktemp)
sed "s|^PREFIX=.*|PREFIX=$PREFIX|" "$HERE/ivshmem-server-ctl" > "$TEMPORARY"
install -D -m 0755 "$TEMPORARY" "$PREFIX/sbin/ivshmem-server-ctl"
rm -f "$TEMPORARY"

TEMPORARY=$(mktemp)
sed "s|/usr/local/sbin/|$PREFIX/sbin/|g" "$HERE/ivshmem-server@.service" > "$TEMPORARY"
install -D -m 0644 "$TEMPORARY" "$UNITDIR/ivshmem-server@.service"
rm -f "$TEMPORARY"

install -d -m 0755 /etc/ivshmem-server
systemctl daemon-reload

echo "$PREFIX/bin/ivshmem-server"
echo "$PREFIX/sbin/ivshmem-server-ctl"
echo "$UNITDIR/ivshmem-server@.service"

# --- libvirt hook -----------------------------------------------------------

if [ "$HOOK" = yes ]; then
  step "installing the libvirt hook"
  HOOKMARK='ivshmem-server hook'
  if [ -e /etc/libvirt/hooks/qemu ] && ! grep -q "$HOOKMARK" /etc/libvirt/hooks/qemu; then
    install -D -m 0755 "$HERE/libvirt-hook" /etc/libvirt/hooks/qemu.ivshmem
    echo "a hook of your own is already at /etc/libvirt/hooks/qemu;"
    echo "ours went to /etc/libvirt/hooks/qemu.ivshmem -- call it from yours,"
    echo "passing the arguments and the domain XML on stdin through"
  else
    install -D -m 0755 "$HERE/libvirt-hook" /etc/libvirt/hooks/qemu
    echo "/etc/libvirt/hooks/qemu"
    echo "hooks are read when the daemon starts: systemctl restart virtqemud libvirtd"
    echo "(restarting them leaves running domains alone)"
  fi
fi

# --- instances --------------------------------------------------------------

for instance in "${INSTANCES[@]+"${INSTANCES[@]}"}"; do
  step "instance $instance"
  "$PREFIX/sbin/ivshmem-server-ctl" add "$instance" "${INSTANCE_ARGS[@]+"${INSTANCE_ARGS[@]}"}"
done

[ "$KEEP" = yes ] || rm -rf "$WORK/source" "$WORK/build"

step "done"
"$PREFIX/sbin/ivshmem-server-ctl" list
