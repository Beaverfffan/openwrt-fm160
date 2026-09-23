# at-daemon — vendored QModem component

This directory is the **unmodified** `ubus_at_daemon` application from
[FUjr/QModem](https://github.com/FUjr/QModem). Do not edit `src/` here: keeping
it byte-identical to upstream is what lets us verify it by hash instead of by
reading a diff.

| | |
|---|---|
| Upstream repository | `https://github.com/FUjr/QModem` |
| Upstream path | `application/ubus_at_daemon` |
| Upstream commit | `86102c2a6f62` — *"qmodem: integrate independent SIP SMS and VoIP services"*, 2026-09-11 |
| Branch | `main` |
| License | MPL-2.0 + non-commercial restriction — see `LICENSE` in this directory |

## Verifying the vendored copy

Every file under `src/` and `files/` is byte-identical to upstream. The check is
a git blob hash, which is `sha1("blob " + len + "\0" + content)`:

```sh
cd at-daemon
for f in src/* files/etc/init.d/* files/etc/config/*; do
	printf '%s  %s\n' "$(git hash-object "$f")" "$f"
done
```

Expected values, as served by the GitHub contents API for the commit above:

| File | git blob hash |
|---|---|
| `src/Makefile` | `749e2f1bdcf61c24b782085e17d55e3513c0cb66` |
| `src/at_handler.c` | `0e56622e787ab04321951a7a33a7dbe226c957ea` |
| `src/config_loader.c` | `a6855b65e8fe2d0ce1e035155d37214c38b548f0` |
| `src/const.h` | `7bcfa04f6d50c08070fde152829785898f0f5e32` |
| `src/control.c` | `1261153c5af4919c05be46b0864e02d21b354320` |
| `src/event_callback.c` | `b66ad1cfbdd9ff959a407512c48d5489199dc526` |
| `src/line_events.c` | `16727ff14abaa078901e3254571b73f13072adbf` |
| `src/main.c` | `fc4d78acf03fdc0318aec496d4660d2b25fd2b8b` |
| `src/port_manager.c` | `2fc56e09bbf788890d4c0af5d85cdb7866213b06` |
| `src/ubus_at_daemon.h` | `1d34a2b818f61ea308913acaa072a8583f160a8e` |
| `files/etc/init.d/ubus-at-daemon` | `eba9562011b014d668bb6c7c34339cba3826c47f` |
| `files/etc/config/ubus-at-daemon` | `28e69bf6dc6041d115ec870bffad4ea9bfcf1013` |

A mismatch means someone edited the transport. That is not automatically wrong,
but it must be a conscious, written-down decision — an AT transport that quietly
diverges from the version everyone else runs is exactly the kind of bug that
wastes a week.

**Worked example.** `files/` was checked rather than assumed, and both files
turned out to be CRLF — 411 and 282 bytes instead of 386 and 271, one extra `\r`
per line. That is the dangerous kind of difference: it compiles, packages and
installs cleanly, and then the init script never starts on the device, because
the kernel reads its interpreter as `"/bin/sh /etc/rc.common\r"`. Normalising to
LF fixed the bug and restored byte-identity with upstream in the same step.
`tools/cccheck/eolcheck.py` now guards against a repeat.

## What is modified, and what is not

Only the packaging around the vendored files differs from upstream, and it
differs because upstream builds this component as part of the QModem feed:

| Path | Status |
|---|---|
| `src/**` | upstream, byte-identical |
| `files/**` | upstream, byte-identical (LF) |
| `LICENSE` | upstream, byte-identical |
| `Makefile` | ours — upstream's pulls version info from `../../version.mk`, which only exists inside the QModem feed; also adds `PKG_LICENSE` and a `conffiles` declaration for `/etc/config/ubus-at-daemon` |
| `version.mk` | ours — carries `QMODEM_VERSION` / `QMODEM_RELEASE` locally so the directory builds standalone |
| `README.md` | ours |

Upstream's package name `ubus-at-daemon` is kept, so a router that already has
QModem installed will have its copy upgraded rather than ending up with two
daemons fighting over the same AT port.

The one build warning this component produces comes from `src/main.c:3`, which
defines `ARRAY_SIZE` unconditionally while `libubox/utils.h` guards its own with
`#ifndef`. The two definitions are character-for-character identical. It is left
alone on purpose: patching it would end the byte-for-byte guarantee this file
exists to provide.

## Version pin

`version.mk` mirrors upstream's values. If you bump it, bump the commit hash in
this file too, and re-run the hash table above.
