# luci-proto-qmi-fm160 — QMAP 拨号协议（LuCI proto 插件）

给 `network.fm160` 提供 `proto qmi_fm160`，跑广和通原厂 `fibocom-dial`
（QMAP 聚合，多通道能力）。单流速率视模组固件而定。

- 数据面走厂商驱动 `qmi_wwan_f`（见 `../qmi-wwan-f/`），**必须** GTUSBMODE 32
- 拨号前后处理（含 `SET_DATA_FORMAT` 冷启动序列）在 `qmap-up.sh`
- 想要全开源 QMI 栈（stock `qmi_wwan` + `uqmi`）请用 `../fm160-qmi/` 替代

依赖：`+luci-base +fibocom-dial`。
