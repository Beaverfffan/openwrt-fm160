# openwrt-fm160

为 **广和通 FM160（单模块）** 做的 OpenWrt 管理套件：模组管理、三模式拨号、
短信、基站锁定、GNSS、mwan3 多线调度。目标是「一台路由器上一个 FM160，
稳定第一」，不做多厂商通用框架。

> 事实依据：三份广和通官方文档的检索整理见 `docs/AT-FACTS.md`；
> 设计取舍见 `docs/DESIGN.md`；构建指南见 `docs/BUILD-ISTOREOS-H69K.md`。

## 组成

| 目录 | 包 | 作用 |
|---|---|---|
| `at-daemon/` | `ubus-at-daemon` | AT 传输层（vendored QModem，串口独占/队列/URC 事件） |
| `fm160d/` | `fm160d` | 模组管理守护进程：轮询调度、状态解析、ECM 拨号、USB 配置切换、短信、GNSS、LAN IPv6、ubus 后端 |
| `fibocom-dial/` | `fibocom-dial` | 厂商 QMI 拨号套件（vendored coolsnowwolf/lede）+ FM160 控制脚本集（dial-ctl/keepalive/oplog/mbim/qmap） |
| `qmi-wwan-f/` | `kmod-qmi-wwan-f` | 厂商 QMI WWAN 驱动（QMAP 必需） |
| `fm160-qmi/` | `fm160-qmi` | **全开源** QMI 拨号替代（stock `qmi_wwan` + `uqmi`，零厂商二进制） |
| `luci-proto-ecm-fm160/` | — | ECM 拨号协议插件 |
| `luci-proto-qmi-fm160/` | — | QMAP 拨号协议插件（走 fibocom-dial） |
| `luci-proto-mbim-fm160/` | — | MBIM 拨号协议插件（vendor CLI，非原生 umbim） |
| `luci-app-fm160/` | `luci-app-fm160` + zh-cn 翻译 | LuCI 应用：概览/信号/小区锁定/拨号/USB 模式/GNSS/MWAN3/短信/日志/AT 台 |

## 快速开始

把本仓库（或其中需要的包目录）放进 OpenWrt 源码树的 `package/` 下：

```sh
git clone https://github.com/Beaverfffan/openwrt-fm160 /tmp/openwrt-fm160
cp -r /tmp/openwrt-fm160/{at-daemon,fm160d,fibocom-dial,qmi-wwan-f,luci-app-fm160,luci-proto-*} package/
# 按需选择（make menuconfig）：
#   Utilities → fm160d, fibocom-dial
#   Kernel modules → WWAN Support → kmod-qmi-wwan-f
#   LuCI → Applications → luci-app-fm160
make package/fm160d/compile package/luci-app-fm160/compile -j4
```

依赖关系已通过各包 `DEPENDS` 声明全量进 Makefile，menuconfig 选中
`luci-app-fm160` 会自动带上 `fm160d`、`fibocom-dial`、`luci-base`。

## 拨号模式怎么选

| 模式 | USB 配置 | 数据面 | 特点 |
|---|---|---|---|
| ECM（默认） | GTUSBMODE 33 | 模组 DHCP | 最成熟，双栈 + LAN 前缀由 fm160d 完整支持 |
| MBIM | GTUSBMODE 30 | wwan0 vendor CLI | 实测单流速度最佳（15-20 MB/s） |
| QMAP | GTUSBMODE 32 | wwan0 + qmi_wwan_f | 厂商驱动 QMAP 聚合，多通道能力 |
| 开源 QMI | GTUSBMODE 32 | stock qmi_wwan + uqmi | `fm160-qmi` 包，零厂商二进制，无 QMAP |

三模式共用 `fm160-dial-ctl` 与拨号页；切换模式会自动重启 USB 配置（30-90 s）。

## mwan3 集成

LuCI 的 MWAN3 页把有线和 FM160 接口自动注册进 mwan3（metric 分层=故障
转移，同层权重=分流），hotplug 自动重注册，只管理带 `fm160_managed` 标记
的配置段。mwan3 为可选依赖。

## 验证

- CI（`.github/workflows/check.yml`）跑 `tools/check.sh`：前端语法、
  CRLF、i18n 提取基/碰撞/po2lmo 往返、C 编译期检查、诊断导出测试。
- 真机验收记录：`docs/HARDWARE-PROBE.md`（H69K 探针）、`docs/AT-FACTS.md`
  （手册 AT × 模组实测交叉验证）。

## 许可证

混合许可证仓库——原创代码 GPL-2.0，vendored 组件各自带 LICENSE/NOTICE。
**复用前必读 [`NOTICE.md`](NOTICE.md)**（`at-daemon` 与 `qmi-wwan-f` 带
非商业限制条款）。
