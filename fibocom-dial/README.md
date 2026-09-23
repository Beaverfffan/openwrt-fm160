# fibocom-dial — 广和通 QMI 拨号套件 + FM160 控制脚本集

本目录是**两部分**的合体，许可证不同，见下。

## 1. `src/` — 厂商 QMI 拨号套件（vendored，来自 coolsnowwolf/lede）

| 项 | 值 |
|---|---|
| 来源 | `https://github.com/coolsnowwolf/lede` → `package/lean/fibocom-dial`（广和通原厂 QMI 拨号源码包） |
| 组成 | `fibocom-dial` 守护进程（SET_DATA_FORMAT / BIND_MUX / QMAP 流程、双栈、内置 udhcpc）、`fibo_qmimsg_server`（多 PDN 的 QMI 通道代理）、`multi-pdn-manager`（最多 8 路并发 PDN）、`libmnl/` 内联副本 |
| 配合 | 厂商驱动 `qmi_wwan_f`（见 `../qmi-wwan-f/`），仅 QMI 模式（GTUSBMODE 32）有意义 |
| 许可证 | 以源文件头为准（GPL-2.0 系） |

> 该源码长期无人维护，本仓库只修阻塞性 bug、不演进功能。FMT 决定论等实测结论记录在 `docs/AT-FACTS.md`。

## 2. `files/` — FM160 项目的自有控制脚本（GPL-2.0-or-later）

由本仓库维护，是 LuCI 前端和 keepalive 的实际执行体：

| 文件 | 作用 |
|---|---|
| `usr/sbin/fm160-dial-ctl` | 三模式拨号统一入口：`status / set <key> <val> / up / down`。校验并持久化 `/etc/config/fm160`，选好后端（ECM/QMAP/MBIM），创建 `network.fm160` 接口并 ifup/ifdown |
| `usr/sbin/fm160-keepalive` | 链路看门狗：按 `ka_interval` ping 模组 DNS（v4/v6），失败重拨，`ka_reload_rounds` 连失败后重启 USB 配置，`ka_stop_rounds`（24h 滑窗）后放弃并上报 |
| `usr/sbin/fm160-oplog` | 语义化操作日志环形缓冲（300 行），`add / tail / clear`；页面、脚本、看门狗共用 |
| `usr/sbin/mbim-up.sh` / `mbim-down.sh` / `mbim-set-ip.sh` | MBIM 模式：广和通 vendor CLI 拨号（`GTI` 命令族），**不用**原生 umbim/libmbim——广和通 MBIM 实现与原生栈差异过大 |
| `usr/sbin/qmap-up.sh` / `qmap-down.sh` | QMAP 模式：fibocom-dial 启动/停止 + LAN 侧运营商前缀处理 |
| `etc/init.d/fm160-dial` | procd 包装：`dial_autostart=1` 时开机拉起，含 oplog 记录 |
| `etc/init.d/fibocom-dial` / `etc/config/fibocom-dial` | 原厂守护进程的服务壳与配置 |

## 依赖

`+libpthread`；运行时配合 `kmod-qmi-wwan-f`（QMAP 模式）或 `fm160d`（ECM 模式 AT 拨号）。

`luci-app-fm160` 依赖本包（拨号页直接 exec `fm160-dial-ctl` / `fm160-oplog`）。
