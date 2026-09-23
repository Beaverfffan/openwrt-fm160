# Provenance and licensing

本仓库包含**三类**代码，许可证各不相同，复用前请读这一段。

## 1. 项目原创代码 — GPL-2.0-or-later

| 目录 | 内容 |
|---|---|
| `fm160d/` | 模组管理守护进程（策略层） |
| `luci-app-fm160/` | LuCI 应用 |
| `luci-proto-ecm-fm160/` `luci-proto-qmi-fm160/` `luci-proto-mbim-fm160/` | 三个拨号协议插件 |
| `fm160-qmi/` | 全开源 QMI 拨号替代实现 |
| `fibocom-dial/files/` | FM160 控制脚本集（dial-ctl / keepalive / oplog / mbim / qmap） |

## 2. `at-daemon/` — vendored from QModem（MPL-2.0 + 禁止商用附加条款）

| | |
|---|---|
| Upstream | `https://github.com/FUjr/QModem` → `application/ubus_at_daemon` |
| Commit | `86102c2a6f62`（2026-09-11） |
| License | MPL-2.0，**附加条款：禁止商业使用**（上游条款，随代码传播） |
| 一致性 | 全部文件与上游逐字节一致，哈希表见 `at-daemon/NOTICE.md` |

## 3. `fibocom-dial/src/` 与 `qmi-wwan-f/` — vendored 厂商代码

| 目录 | 来源 | 许可证 |
|---|---|---|
| `fibocom-dial/src/` | coolsnowwolf/lede → `package/lean/fibocom-dial`（广和通原厂 QMI 拨号源码包） | 以源文件头为准（GPL-2.0 系） |
| `qmi-wwan-f/` | 广和通 QMI WWAN 驱动（QModem feed 同名包），仓内 LICENSE 为 MPL-2.0 + 非商用限制，见该目录 `NOTICE.md`；驱动源文件头为 GPL-2.0 | 两种并存，细节见 `qmi-wwan-f/NOTICE.md` |

仓库根 `LICENSE`（GPL-2.0）仅适用于第 1 类；第 2、3 类以其各自目录内的
LICENSE/NOTICE 为准。
