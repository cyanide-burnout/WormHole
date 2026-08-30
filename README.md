# WormHole

Linux inter-VM shared memory client library

* Fresh UIO driver compatible with Linux kernels 6.x
* Library could work
  * in host mode using direct connection to *QEMU ivshmem-server*
  * as well as in guest mode using UIO driver
* Allows to run multiple instances of application in both modes
* Server side is here as well: *ivshmem-server* is built out of QEMU sources, which
  compile it and install it nowhere, and put under systemd with an instance per
  shared region
* In addition here is integration with io_uring-based main loop (see repository **FastRing**)

## Driver

The UIO driver lives in `Driver/`. Building it needs the kernel headers for the
running kernel (`linux-headers-$(uname -r)` or equivalent).

Install through DKMS so the module is rebuilt automatically on kernel upgrades:

```
cd Driver
sudo make dkms
sudo make reload
```

To remove it:

```
sudo make dkms-remove   # this version
sudo make dkms-purge    # every installed version
```

A plain one-off build without DKMS is also available, but it does not survive
a kernel upgrade:

```
cd Driver
make
sudo make install
```

## Server

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

### Instances

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

### What libvirt will not do for you

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

The domain side is the ordinary thing:

```xml
<shmem name='ivshmem-default'>
  <model type='ivshmem-doorbell'/>
  <size unit='M'>16</size>
  <server path='/run/ivshmem-server/default.sock'/>
  <msi vectors='1'/>
</shmem>
```

One vector, because that is all the UIO driver can make use of. On a host with
AppArmor, `/run/...` is outside what the generated profile grants a domain; if
the domain is denied the socket, either put it where the profile reaches or add
the path to `/etc/apparmor.d/local/abstractions/libvirt-qemu`.

## Library

### WormHole

```c
struct WormHole* CreateWormHole(const char* path, HandleWormHoleBellFunction function, void* closure);
void ReleaseWormHole(struct WormHole* hole);

int KickWormHole(struct WormHole* hole, uint32_t identifier);

int HandleWormHoleHostServerEvent(struct WormHole* hole);
void HandleWormHoleInterruptEvent(struct WormHole* hole, int64_t value);
```

`CreateWormHole` picks the mode by what `path` turns out to be: a character device
is the UIO driver and gives a guest client, a socket is *ivshmem-server* and gives a
host one. Everything below that difference is the same for both.

What the caller gets to use is `memory` and `length` — the shared region itself —
and `identifier`, the ID of this peer among the others. `KickWormHole` rings the
doorbell of a peer by its ID, and the `function` given at creation is called when
somebody rings ours.

Waiting is left to the caller: `handle` is the event source to poll and `size` is
how much to read from it. Having read a value, hand it to
`HandleWormHoleInterruptEvent`; hand it `HOLE_CLIENT_VALUE_READ` instead to have the
source read for you. In host mode the connection to the server is a second source,
`host.socket`, and everything that arrives on it is for
`HandleWormHoleHostServerEvent` — that is how the peers coming and going become
known.

A peer may ring itself, and the mode makes no difference to that: on a host several
clients of one server see each other the same way as the machines do.

### WormLoop

```c
struct WormLoop* CreateWormLoop(struct FastRing* ring, struct WormHole* hole);
void ReleaseWormLoop(struct WormLoop* loop);
```

Ties a hole to an io_uring loop of **FastRing**, and after that neither source has to
be polled by hand: the doorbell arrives as the bell of the hole, and in host mode the
socket of the server is watched as well. The kick is replaced by an io_uring one, so
ringing a neighbour costs no syscall of its own.

The rule of a loop applies: the bell is called from inside the loop and has nothing
to wait for there.

## Arena

On top of the library there is **WormArena** — a store of named objects, pools of
blocks and lock-free queues laid out inside the shared memory, meant to be used by
several processes on several machines at once. See [Arena.md](Arena.md).

## Testing

`Test/Arena` needs neither the driver nor a server: an anonymous shared mapping
stands in for ivshmem and `fork` for the peers. It covers the layout and the leases,
the pools, the queues and the descriptors, and four ways of a peer dying in the
middle of something — 72 checks in all.

```
cd Test/Arena
make && ./arena
```

`Test/Hole` is the one that wants the real thing. `test.sh 1` runs host clients
against *ivshmem-server*, `test.sh 2` builds a guest image and boots it with
*ivshmem-doorbell* so that the guest talks to the host through the driver.
*ivshmem-server* is not packaged by Debian; `Server/` builds it out of the
`contrib/` directory of QEMU and puts it under systemd.

`Test/Smoke` puts the arena over that same real region. Every peer is given the path
of the socket or of the UIO device, a name of its own, a number of seconds and how
many peers take part in the run, itself included:

```
cd Test/Smoke
make && ./smoke /tmp/ivshmem_socket first 30 6
```

Whoever gets there first lays the arena out and the rest attach to it by name. Each
keeps a lease, takes the write lock of a shared region once a second, and hears from
the others two ways at once. It has a queue of its own, named after it, that everybody
else puts a message into every second; and there is one queue shared by all of them
that each posts to and takes from as well.

The verdict stands on the first of the two. Nobody competes for an inbox, so hearing
from every other peer is a matter of the arena working rather than of winning a race,
and a peer left alone is not mistaken for one that has reached somebody. The shared
queue is reported and not judged, because competing for one queue is where the
behaviour worth looking at shows up — and it is not fair. Consumers wake at very
different speeds: a peer of the host, woken by a write to an eventfd inside its own
kernel, was measured taking two thirds of everything, while a peer of a guest, which
waits on a doorbell through a virtual machine exit, took none of it at all. The
report also counts how often taking failed on a slot a producer had claimed and not
yet filled in, which is the other way a queue can look empty when it is not.

### What has been run

On 2026-08-30, Debian 13 and kernel 6.12.107 on both sides of the boundary:

* Host mode, several clients on one *ivshmem-server*: doorbells travel both ways,
  and when the server is killed the loss is reported once and the client falls
  silent instead of spinning.
* Guest mode through the driver: the module loads, `/dev/uio0` appears with a
  region of the expected size, and the bells cross the boundary of the virtual
  machine in both directions.
* Twelve clients at once — four on the host and four in each of two virtual
  machines — and every one of them sees all twelve sources. A machine is a single
  peer however many processes it runs: the four in a guest share one ID, while the
  four on the host have one each.
* All four processes holding one `/dev/uio0` are woken by an interrupt, not merely
  the first of them.
* No io-wq worker is left parked in a blocking read. Measured against the previous
  revision, which had one per hole: the UIO device supports no NOWAIT reads, so an
  asynchronous read of it is served by a kernel worker, whereas watching the source
  and reading it once ready is not.
* The arena over that region, peers of `Test/Smoke` on the host and in both virtual
  machines at once, some of them joining half a minute after the arena had been laid
  out and had been busy since. One laid it out and the rest attached to it by name,
  every one of them held a lease and took the write lock of the region without missing
  it once, and every one of them heard from every other through its own queue. Nothing
  was ever lost in either direction: 1070 messages sent between the inboxes and 1070
  taken out of them, 345 posted to the shared queue and 345 taken from it, and both
  left empty at the end.
* Taking from a shared queue is not fair, and it was never once held up by a slot
  a producer had claimed and not filled in. Of the 345 messages a peer of the host
  took 220 and a peer of a guest took none, which is the difference between waking
  inside a kernel and waking through a virtual machine exit.

What has not been run: the ways a peer can die in the middle of something — the
sweeps that give back its blocks, its locks and the queue slots it took and never
filled — are covered only by `Test/Arena`, where the memory is ordinary and cacheable
and the peers are forks of one process.

## Limits

* Only one IRQ vector is supported because of UIO limits
* Due to byte-order depended implementation of ivshmem in QEMU a guest system should use the same byte-order as a host

## Links

* https://www.qemu.org/docs/master/system/devices/ivshmem.html
* https://www.qemu.org/docs/master/specs/ivshmem-spec.html
* https://github.com/henning-schild-work/ivshmem-guest-code/tree/master
* https://github.com/qemu/qemu/tree/master/contrib/ivshmem-client
* https://github.com/projectacrn/acrn-hypervisor/tree/master/hypervisor/dm/vpci/ivshmem.c
* https://github.com/projectacrn/acrn-hypervisor/tree/master/misc/sample_application/rtvm
* https://github.com/torvalds/linux/blob/master/drivers/uio/uio_netx.c
