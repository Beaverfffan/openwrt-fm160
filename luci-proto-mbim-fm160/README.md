# luci-proto-mbim-fm160 — MBIM 拨号协议（LuCI proto 插件）

给 `network.fm160` 提供 `proto mbim_fm160`。实测本模组单流速度最佳的
模式（15-20 MB/s），但**不用** OpenWrt 原生 umbim/libmbim——广和通 MBIM
实现与原生差异过大，拨号全部走原厂 vendor CLI（`mbim-up.sh` 内 `GTI` 命令族）。

- 必须 GTUSBMODE 30；`mbimcli` 看门狗用 setsid 后台 + 定时杀（busybox 无 timeout）
- 断线回收与 LAN 前缀处理在 `mbim-down.sh` / `mbim-set-ip.sh`

依赖：`+luci-base +fibocom-dial`。
