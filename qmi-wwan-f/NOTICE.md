# NOTICE -- vendored third-party kernel driver

This directory is **not** written by this project. It carries the Fibocom QMI WWAN
driver from the QModem project, vendored so it builds inside this tree without
adding the whole QModem feed.

## Provenance

| | |
|---|---|
| Upstream repository | <https://github.com/FUjr/QModem.git> |
| Upstream path | `driver/fibocom_QMI_WWAN/` |
| Pinned commit | `964d8dd325f230377bb58e373d3b1892d73cc969` (2026-09-17, `main`) |
| Also identical at | `c49654efc870f53712ee8e25bf181722eb1466d9` (upstream `main` tip at time of import, 2026-09-20) |
| Driver self-version | `V1.0.5`, from `#define VERSION_NUMBER "V1.0.5"` (qmi_wwan_f.c:95) |
| Upstream package name | `kmod-qmi_wwan_f` (KernelPackage `qmi_wwan_f`, `PKG_VERSION:=1.0`, `PKG_RELEASE:=5`) |

The two `src/` files are **byte-identical** to upstream. Both were checked with
`cmp` against the local clone and with `sha256sum`; the `qmi_wwan_f.c` blob id was
additionally confirmed to be the same at `964d8dd` and at the `main` tip.

| File | Bytes | sha256 | git blob at `964d8dd` |
|---|---|---|---|
| `src/qmi_wwan_f.c` | 82620 | `5c001613ae786a5bc907f9e0d7cd1606d459a8374e6947fa991b3655ab10915d` | `0b4c08cdf98c74a43be21949fcc20389e566749e` |
| `src/Makefile` | 963 | `38879542e8659391cf9c3c5ecbcde87fb8be863b8b4950426a37f4ad99b10a66` | `00250a1c1ae704c1a87a66680646b77f57c9238c` |
| `LICENSE` | 17582 | `a8cb74e6e80dcaaac4f18ecf2012d1ed8e6a3843ea2c4ac4d8842f5e8c713d6f` | `f94c79a8906625857873b4cbb422849976de3da7` |

For reference, the upstream package Makefile this directory's `Makefile` derives
from is `4d466a817368810133b96b1ddfd05b6737eb0c33` (`driver/fibocom_QMI_WWAN/Makefile`,
1028 B) -- it is the one file we modify, so its blob is recorded here rather than
in the table above.

The `.c` blob id being equal at `964d8dd` and at the `main` tip is why we pinned
the older commit: the driver did not change across the ~1137 commits between
them, so pinning costs nothing and gives a stable, checkable reference.
`50-import-qmiwwanf.sh` re-checks the two sha256 values before installing, and
refuses to continue if either drifts.

## What we changed

Nothing in `src/`.

| Path | Status |
|---|---|
| `src/qmi_wwan_f.c`, `src/Makefile` | upstream, byte-identical |
| `LICENSE` | upstream repo LICENSE, byte-identical |
| `Makefile` | upstream body kept verbatim; added a header comment and the `PKG_LICENSE` / `PKG_LICENSE_FILES` / `PKG_MAINTAINER` fields (marked `LOCAL`), which upstream omits |
| `NOTICE.md`, `README.md` | ours |

## Licence -- read before shipping this in a product

Two different licences are in play and they are not the same document:

* **The driver source is GPL-2.0.** `src/qmi_wwan_f.c` carries Bjørn Mork's
  GPL-2.0 header and ends with `MODULE_LICENSE("GPL")`. That is what
  `PKG_LICENSE:=GPL-2.0` records.
* **The QModem repository `LICENSE` is MPL-2.0 plus an additional restriction
  prohibiting commercial use.** Upstream's own README states this explicitly:
  *"It is therefore not an unmodified standard MPL 2.0 grant. Read the complete
  license before using, distributing, or integrating the project."*

The `LICENSE` file is carried here verbatim so the restriction travels with the
copy. **The driver file is descended from mainline `qmi_wwan.c` (GPL-2.0), so the
GPL reading is the defensible one for `src/qmi_wwan_f.c` itself** -- but the
non-commercial clause is a repository-level condition on the QModem project and
nobody has litigated the boundary. If this firmware is ever distributed, that is
a decision to take deliberately, not by omission.

## What this driver claims on our hardware

Its USB device table contains, at `qmi_wwan_f.c:2394`:

```c
{ QMI_FIXED_RAWIP_INTF(0x2cb7, 0x0104, 4) },  /* Fibocom FG150/FM150/NL952/FG101 */
```

That is **the same interface** our tree-local patch `782-usb-net-qmi-wwan-Fibocom_RMNET_rawip.patch`
targets in mainline `qmi_wwan.c` (interface 4 of pid `0x0104`), and the same one
the FM160 exposes when it is in USB mode 32.

## What it does that mainline does not

These are the reasons it cannot be reduced to a `driver_info` table edit -- our
patch `782` only reproduces the first of them:

1. **Forces raw-IP at bind time.** For any product whose `driver_info` carries
   `FLAG_NOARP` (`qmi_wwan_f.c:1929`) it sets `IFF_NOARP`, clears
   `IFF_BROADCAST | IFF_MULTICAST`, and sends
   `USB_CDC_REQ_SET_CONTROL_LINE_STATE` with DTR asserted *during bind*.
2. **Sets its own RX URB size**: `dev->rx_urb_size = ETH_DATA_LEN + ETH_HLEN + 6`
   (`:1958`), i.e. 1558, rather than mainline's value.
3. **Puts MAP (QMAP) framing on the primary netdev.** `qmap_mode` is forced to 1
   for `idProduct 0x0104` (`:1962-1970`, the `lte_a` branch), with
   `qmap_version = 5`. In mainline, QMAP lives on a separate `qmimux` netdev that
   userspace creates via `/sys/class/net/<if>/qmi/add_mux`; the primary netdev
   there always carries plain frames. **This is the only framing configuration
   this project has not yet measured on the FM160.**
4. Replaces `netdev_ops` and `ethtool_ops`, adds `NETIF_F_VLAN_CHALLENGED`, and
   carries its own `ndo_get_stats64` fix for kernel 6.10+.

Note that `qmap_mode` cannot be set to 0 for this pid: the `lte_a` branch
re-forces 1 whenever the parsed value is 0, so passing `qmap_mode=0` still ends
up at 1.

## Load-order interaction (important)

`AUTOLOAD:=$(call AutoLoad,82,qmi_wwan_f)` installs `/etc/modules.d/82-qmi_wwan_f`.
Mainline's `kmod-usb-net-qmi-wwan` uses `$(call AutoProbe,qmi_wwan)`, which
resolves to priority 0 and installs a plain `/etc/modules.d/qmi_wwan` with **no
numeric prefix**. `/etc/init.d/modules` walks that directory in `ls` order, and
`8` sorts before `q`, so **the Fibocom driver loads first and wins the
interface**. Installing this package therefore *takes over* `(0x2cb7, 0x0104, 4)`
from mainline -- including over our patched module. That is the intent, but it is
a swap, not an addition.

There is no `EXPORT_SYMBOL` anywhere in the driver, so it cannot clash with
mainline `qmi_wwan` at the symbol level; the two can coexist in the kernel, and
whichever probes first owns the interface.

## Re-vendoring

```sh
# 1. on a machine that can reach github.com
git clone https://github.com/FUjr/QModem.git /tmp/QModem
git -C /tmp/QModem log -1 --format=%H -- driver/fibocom_QMI_WWAN
git -C /tmp/QModem rev-parse HEAD:driver/fibocom_QMI_WWAN/src/qmi_wwan_f.c   # compare blob id

# 2. update the pinned sha256 values in _tools/istoreos-h69k/50-import-qmiwwanf.sh
# 3. refresh _stage/istoreos-h69k/kmod-qmi-wwan-f/src/, update this table
# 4. re-run 50-import-qmiwwanf.sh
```

If upstream ever fixes the data-plane behaviour on this module, the change will
show up in the blob id above -- which is the cheapest place to look first.
