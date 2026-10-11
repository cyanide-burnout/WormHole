# ivshmem-server

This document describes the server side of host mode: how `Server/` builds
*ivshmem-server* out of the QEMU sources and puts it under systemd, how its instances
are managed, and what has to be done for libvirt. The library itself is described in
the [README](../README.md).

- [Building and Installing](#building-and-installing)
- [Instances](#instances)
- [What libvirt will not do for you](#what-libvirt-will-not-do-for-you)

## Building and Installing

*ivshmem-server* is the other side of host mode: it hands every peer the region
and the eventfds of all the others. QEMU builds it out of `contrib/` and then
installs nothing — `install: false` in its `meson.build` is the whole reason no
distribution carries it, and the disclaimer it prints on startup is the reason
nobody has asked them to. `Server/` takes it from there: it fetches the sources,
builds that one binary out of them and puts it under systemd as a template unit
with an instance per region.

```
cd Server
sudo ./install.sh
```

QEMU is configured with everything but the tools switched off, since the server
links against `libqemuutil` and wants nothing else, so the build is minutes
rather than an hour. What comes out is `/usr/local/bin/ivshmem-server`,
`/usr/local/sbin/ivshmem-server-ctl` and one instance named `default`, running on
`/run/ivshmem-server/default.sock` with 16M of memory in `/dev/shm/ivshmem-default`.
`ivshmem-client` is installed along with it, being useful for looking at a region
without writing anything. `--version`, `--prefix`, `--source`, `--instance` and
the rest are in `./install.sh --help`, and `make install FLAGS="..."` is the same
thing said through the Makefile.

The tarball stays in `/var/cache/ivshmem-server` so that a second run has nothing
to download; the unpacked sources and the build tree are swept once the binary is
installed, unless `--keep` says otherwise or `--build-only` stopped short of
installing anything. A tree that is kept is picked up again by the next run, but
only if it was configured from the same sources for the same prefix — otherwise
it is thrown away and configured afresh, rather than answering `--version` with
whatever it was built from the last time. Since all of that is read back, the
directory has to belong to whoever is building and be closed to everybody else.
Somewhere world-writable — `/var/tmp`, where a build tree would otherwise
naturally go — is somewhere a tarball or a `build.ninja` can be left waiting for
root to find, and this refuses to be that. `--work` moves it under the same rule
and one more: the directory is marked as ours, and since that mark is what
`uninstall.sh --purge` takes as leave to `rm -rf` the place, it is only ever put
on a directory that was made for the build or was empty when it was taken over.
`--work /etc` would pass every other test there is when root is the one
building.

## Instances

One unit, one instance per region, one file per instance:

```
ivshmem-server-ctl add render --size 64M --group kvm
ivshmem-server-ctl list
ivshmem-server-ctl remove render --purge
```

`add` writes `/etc/ivshmem-server/render.conf`, then enables and starts
`ivshmem-server@render`; over an instance that is already running, `--force`
restarts it, since a file the running server never read is not a configuration.
The file is where the socket path, the size, the number of vectors, the name of
the shared object and the ownership of the socket live; editing it and restarting
the instance is the whole of reconfiguring one. Two instances are not allowed
onto one socket or one region.

That file looks like shell and is deliberately not read as shell. Sourcing it
would run whatever it held before anything could be said about it, and a check
that comes after the execution is a check of what is left rather than of what was
done — so it is parsed instead: `KEY=value` of a name the instance knows, quoted
or not, and a line that is anything else stops the instance where it stands. The
values are looked at on both sides of the file, going in and coming back out — a
path is an absolute path, a size is digits and a suffix, a mode is three octal
digits, the number of vectors is one to sixty-four — so a file edited into
something strange is refused with a word about which value it was, rather than by
the server failing oddly later on. The libvirt hook reads the socket path out the
same way, being root inside libvirtd and no place to execute a file for the sake
of one line of it.

The socket is bound owned by root and by default reachable by nobody else, so the
instance is given a group once it is up — `kvm` on Debian, which is what libvirt
runs QEMU under, and `qemu` where that is the name instead. A host client of your
own has to be in that group or be root.

A socket left over from a server that was killed rather than stopped is taken
away at the next start — but only after `ss` has been asked and has answered that
nothing is serving it. `ss` missing, `ss` failing and `ss` answering nothing at
all are three ways of not having been told, and none of them is an answer: an
instance that finds a socket it cannot account for stops and says so instead of
unlinking one somebody may be serving. That is what `iproute2` is doing among the
dependencies.

Restarting an instance hands out a *new* region: the server unlinks the shared
object as it goes down, and neither a guest nor a host client reconnects on its
own. That is why the unit does not restart itself on failure — coming back up
quietly would leave everything attached talking to nothing.

## What libvirt will not do for you

It will not wait. A `<shmem>` device turns into a client `-chardev socket`, and
QEMU connects to it while the device is being realized, blocking there for the
protocol handshake that carries the file descriptor of the region. There is no
retry and no timeout to wait out: if the socket is not there, the domain does not
start, and libvirt reports what QEMU said about the connection.

So the server has to be up first. At boot that is what the ordering in the unit
is for — instances are `Before=libvirtd.service virtqemud.service
libvirt-guests.service`, which covers the domains libvirt autostarts. For a
domain started by hand later, or on a host where the instance is not enabled,
there is a hook:

```
sudo ./install.sh --hook
sudo systemctl restart virtqemud libvirtd
```

It goes to `/etc/libvirt/hooks/qemu`, runs at `prepare/begin` — before QEMU is
spawned, with libvirt waiting on it — reads the domain XML, and starts the
instance every `<shmem>` of that domain points at. A socket that belongs to no
instance of ours it leaves alone. If one already lives at that path, the hook is
installed next to it as `qemu.ivshmem` for you to call yourself. libvirt reads
its hooks when the daemon starts, which is what the restart is for; running
domains are not disturbed by it.

The domain side, the `<shmem>` element with its one vector and what AppArmor has to
say about the socket, is described in [GUEST.md](GUEST.md#libvirt).
