# qmi_wwan_f (vendored Fibocom QMI WWAN driver)

Upstream: `<https://github.com/FUjr/QModem.git>`, path `driver/fibocom_QMI_WWAN/`,
pinned at `964d8dd`. Full provenance, licence warning and a list of what this
driver does that mainline does not: **see [NOTICE.md](NOTICE.md)**.

## Why it is in the tree

The FM160 in USB mode 32 exposes its data function as interface 4 of pid `0x0104`
with `bInterfaceProtocol 0x50` -- Fibocom's own RMNET code, not QMI's `0xff`.
mainline `qmi_wwan` binds it (by vid/pid/interface number, not by class code) and
frames it as Ethernet. Two facts about that function are now established on real
hardware:

* **The framing was wrong and is fixable from the host.** Patch
  `782-usb-net-qmi-wwan-Fibocom_RMNET_rawip.patch` makes mainline bind it raw-IP;
  measured result: `qmi/raw_ip=Y`, `type=65534`, `addr_len=0`,
  `POINTOPOINT,MULTICAST,NOARP`, and `tx_errors` stops climbing.
* **Framing was not the whole story.** With correct framing the module still
  returns nothing: `rx_packets` stayed 0 across four independent sessions, and the
  module's own byte counter (`AT+GTSTATIS?`) never moved, while the control plane
  stayed healthy.

What remains untested is the one path mainline cannot produce: this driver forces
`qmap_mode=1` for pid `0x0104`, i.e. **MAP framing on the primary netdev**, where
mainline only ever puts MAP on a separate `qmimux` netdev. That is the reason to
have the driver in the tree instead of re-deriving its table row.

## Building

```sh
# on the build machine, from the tree root
make package/kernel/qmi-wwan-f/compile V=s
```

Two prerequisites that are easy to trip over:

1. **The package must be selected.** `include/kernel.mk` emits, for a kmod that
   is not enabled, `compile: <name>-disabled` with a
   *"not available in the kernel config"* warning instead of a real build. Set
   `CONFIG_PACKAGE_kmod-qmi_wwan_f=y` (or `=m`) first, or use
   `_tools/istoreos-h69k/50-import-qmiwwanf.sh`, which builds the `.ko` directly
   against the tree's kernel directory and needs no config change.
2. **`pahole` must be on `PATH`.** This kernel sets `CONFIG_DEBUG_INFO_BTF=y` and
   `CONFIG_DEBUG_INFO_BTF_MODULES=y`, so every module link shells out to it; it
   lives in `staging_dir/host/bin` and is not on `PATH` for a bare
   `make -C <linux_dir>`. Missing it fails the link **and deletes the `.ko` it
   was relinking.**

## Verifying the artefact

```sh
modinfo build_dir/target-*/linux-*/qmi_wwan_f-1.0/qmi_wwan_f.ko
```

Expect `vermagic` to end in the running kernel's version. On the H69K that is
`6.6.144 SMP mod_unload aarch64`, matching `uname -r` exactly -- which is what
makes the driver loadable without flashing an image.

## Loading it on the device

The driver competes with mainline for the same interface, and it wins the race
because `82-qmi_wwan_f` sorts before the unprefixed `qmi_wwan` (see NOTICE.md).
To swap by hand, mainline must release the interface first:

```sh
ifdown 2_1                       # the netifd interface holding wwan0
ip link set wwan0 down
rmmod qmi_wwan
insmod /tmp/qmi_wwan_f.ko
# then re-check framing, /dev/cdc-wdm0, and the QMI control plane
```

Both drivers register a cdc-wdm device, so the `uqmi` path survives the swap.
Nothing here changes USB mode, and `rmmod` + a reboot restores the stock module.
