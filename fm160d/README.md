# fm160d — FM160 模组管理守护进程（策略层）

单 FM160 场景下的模组管理者。AT 传输**不自实现**，而是通过 ubus 复用
`ubus-at-daemon`（见 `../at-daemon/`，QModem 项目，逐字节 vendored）——
串口独占、请求队列、行事件、URC 订阅这些容易出错的部分全部交给它。

许可证：GPL-2.0-or-later。

## 职责

| 层 | 内容 |
|---|---|
| 轮询调度 | 分级节奏（注册 5s / 小区 10s / 字节计数走 sysfs 零 AT 成本），前台页面打开时加速（`profile` 调用）；对高通 AT 解析器的脆弱性做了静默窗与熔断 |
| 状态解析 | 身份（IMEI/ICCID/IMSN…）、注网（CEREG/C5GREG）、信号（CSQ/RSRP/RSRQ）、服务小区与邻区、载波聚合、速率（GTSTATIS） |
| 拨号（ECM 模式） | 通过 AT 激活 PDP；模组在 usb0 上发 DHCP，netifd 只做地址侧 |
| USB 配置切换 | GTUSBMODE 白名单 + 回滚状态机；共享 USB ID 的配置对（17/32、18/33）必须读回确认，切换后模块重枚举 30-90s |
| 短信 | PDU 编解码、收发、存储管理、长短信；`+CMTI` URC 事件驱动 |
| GNSS | NMEA 采集与解析（多星座、SNR、fix 状态），引擎开关持久化 |
| LAN IPv6 | ECM 链路只有 /64 无 PD：`fm160-prefix` 把运营商前缀挂上 br-lan 并给 odhcpd 配 ndp relay，让 LAN 拿到全球地址（细节见配置文件内注释） |
| ubus | 单一对象 `fm160`，LuCI 的唯一后端；`status / at / sms_* / dial_* / setusbmode / setbands / setcelllock / setgnss* / profiles / rescan / ident / enabled / profile / diagnostics` |

## 配置

`/etc/config/fm160`（`config fm160 'main'`）：`enabled`、`tier_scale`、`dial_apn`、
`dial_pdp`（IP/IPV6/IPV4V6）、`dial_cid`、`dial_autostart`、`ka_*` 看门狗、
`lan_ipv6` 及端口/netdev 钉扎项。配置文件内每个选项都有设计理由注释。

## 依赖

`+libubus +libubox +libblobmsg-json +ubus-at-daemon`

## 构建

```sh
make package/fm160d/clean package/fm160d/compile -j4
```
