# luci-proto-qmi-fm160 — QMAP 拨号协议（LuCI proto 插件）

给 `network.fm160` 提供 `proto qmi_fm160`，跑广和通原厂 `fibocom-dial`
（QMAP 聚合，多通道能力）。单流速率视模组固件而定。

- 数据面走厂商驱动 `qmi_wwan_f`（见 `../qmi-wwan-f/`），**必须** GTUSBMODE 32
- 拨号前后处理（含 `SET_DATA_FORMAT` 冷启动序列）在 `qmap-up.sh`
- 与 `uqmi` / `modemmanager` **互斥**（fibocom-dial 声明 `CONFLICTS`）：二者会抢占
  同一 cdc-wdm QMI 控制通道，导致拨号静默失败。开源 uqmi 替代（fm160-qmi）已于
  2026-09-22 移除——当晚实测数据面未打通（拿到地址但 RX=0）。

依赖：`+luci-base +fibocom-dial`。
