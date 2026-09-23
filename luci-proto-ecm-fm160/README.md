# luci-proto-ecm-fm160 — ECM 拨号协议（LuCI proto 插件）

给 `network.fm160` 提供 `proto ecm_fm160`。ECM 是 FM160 最成熟的形态：
`fm160d` 通过 AT 激活 PDP 上下文，模组在 `usb0` 上直接提供 DHCP，
netifd 只负责地址侧（v4 DHCP + v6 RA/无 PD）。

- 拨号参数（APN/PDP 类型）读 `/etc/config/fm160`，由拨号页统一管理
- v6 的 LAN 侧前缀由 `fm160d` 的 prefix 功能负责，不在本 proto 内

依赖：`+luci-base +fibocom-dial`（ECM 由 fm160d 拨，fibocom-dial 提供控制脚本集）。
