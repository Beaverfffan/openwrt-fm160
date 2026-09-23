# fm160-qmi —— Fibocom FM160 的**开源** QMI 拨号实现

> 用户指令：「fibocom 官方集成和拨号如上，**我希望用开源实现**」。
> 官方给的是**闭源二进制**（`fibocom-dial` / `QConnectManager` / `quectel-CM-M`）；
> 本目录是**同等能力、全开源**的替代实现。

## 组成

| 目标路径 | 作用 |
|---|---|
| `/usr/sbin/fm160-qmi` | 拨号器本体（`up` / `down` / `status` / `watch`） |
| `/usr/sbin/fm160-qmi-at` | 零依赖 AT 助手（`routes` / `get` / `off` / `at` / `reset`），用于写 `AT+GTAUTOCONNECT=0` |
| `/etc/init.d/fm160-qmi` | procd 服务（开机自启 + 崩溃拉起 + 健康检查重拨） |
| `/etc/config/fm160-qmi` | 配置（`enabled` / `profile` / `apn` / `netif` / `ipfam` / `metric`） |
| `dist/fm160-qmi_<ver>_all.ipk` | 打包产物，`opkg install --force-reinstall` 直接装 |

**依赖**：`uqmi`（OpenWrt 自带，开源）、`netifd`（`network.2_1`：`proto=dhcp device=wwan0 metric=11`）。
**不需要**：任何厂商二进制、`libqmi/qmicli`、内核或驱动改动。

## 安装

### 方式 A：装 ipk（推荐）

```sh
# 包的 Architecture 是 all，依赖 uqmi + netifd
scp -O dist/fm160-qmi_1.0.2-1_all.ipk root@<device>:/tmp/
ssh root@<device> 'sha256sum /tmp/fm160-qmi_*.ipk'
ssh root@<device> 'opkg install /tmp/fm160-qmi_1.0.2-1_all.ipk'
# 装完 postinst 会打印首次启用步骤（见下「关键前提」）
```

> `scp` 必加 `-O`：OpenSSH 9+ 默认走 SFTP 子系统，而 dropbear 通常不带
> `sftp-server`，会报 `subsystem request failed on channel 0`。
> 装不上就退回管道：`ssh root@<device> 'cat > /tmp/x.ipk' < dist/xxx.ipk`。

### 方式 B：直接铺文件

```sh
# 0) ★ 前置（只做一次）：关掉模组自建的自动连接，否则任何拨号器都拿不到数据
#    见下节「关键前提」。这一步是必需的，不是可选优化。
scp -r usr etc root@<device>:/
ssh root@<device> 'chmod +x /usr/sbin/fm160-qmi* /etc/init.d/fm160-qmi'
ssh root@<device> 'fm160-qmi-at off'          # 自动发现 AT 口 → 读现值 → 写 0 → 回读
ssh root@<device> 'fm160-qmi-at reset'        # ★ 必须复位一次才生效

# 1) 装服务并启动（模组 QMI 服务需 150–200 s 就绪，脚本会自己等）
ssh root@<device> '
  /etc/init.d/fm160-qmi enable
  /etc/init.d/fm160-qmi start
  sleep 5; fm160-qmi status
'
```

> **关于 AT 助手（★ 有坑）**：在 iStoreOS/H69K 上 **`ubus-at-daemon` 会独占全部 `/dev/ttyUSB*`**，
> 直接往 tty 写 AT 会**得不到应答**（不是口错，是被占）。正确通路是
> `ubus call fm160 at '{"cmd":"AT+…"}'`（`fm160d` 暴露的方法，它持有端口 lease）；
> `fm160-qmi-at` 会按 **① `fm160` ubus → ③ 直接写 tty** 依次尝试，并可用 `routes` 看实际走通哪条：
>
> ```sh
> fm160-qmi-at routes      # 谁持有端口 + 哪条通路可用（会往候选口发一条 AT 探测，无副作用）
> fm160-qmi-at get         # 读 AT+GTAUTOCONNECT?
> ```
>
> 目标机上通常**没有** `picocom`/`microcom`/`sendat`（实测 iStoreOS 一个都没有），
> 所以助手只用 `stty` + 重定向 + `ubus`，并**自动探测**哪个 `ttyUSBn` 答 AT
> ——AT 口会随 USB 重枚举位移，不能写死。**不需要**先停 `fm160d`（正是它在提供 ubus 通路）。

可选配置 `/etc/config/fm160-qmi`：

```
config fm160-qmi 'main'
	option enabled  '1'
	option profile  '1'            # 用模组自带 profile（默认）
	option apn      ''             # 留空 = 用 profile；填了则 --apn <值>
	option netif    '2_1'          # netifd 接口名
	option ipfam    'unspecified'  # ipv4|ipv6|unspecified，见下
	option metric   '11'           # 默认路由 metric，别抢 eth0
```

## 与厂商 `quectel-CM` 的逐条对应

| `quectel-CM` 的 QMI 请求 | 本实现的 `uqmi` 等价物 |
|---|---|
| `QMICTL_SYNC` / `GET_VERSION` / `GET_CLIENT_ID` | uqmi 内部自动完成 |
| `QMIWDS_ADMIN_SET_DATA_FORMAT` | `--wda-set-data-format raw-ip --ul-aggregation-protocol qmap --dl-aggregation-protocol qmap --ul-datagram-max-count 11 --ul-datagram-max-size 8192 --dl-datagram-max-count 32 --dl-datagram-max-size 16384` |
| `QMIWDS_BIND_MUX_DATA_PORT {ep{2,4}, mux 0x81}` | `--bind-mux 129 --endpoint-type hsusb --endpoint-iface 4` |
| `QMIWDS_GET_PROFILE_SETTINGS (pdp:1 index:1)` | `--start-network --profile 1` |
| `QMIWDS_START_NETWORK_INTERFACE` | `--start-network --ip-family ipv4` |
| `QMIWDS_GET_RUNTIME_SETTINGS` | 不直接调用 —— 地址交给 netifd（见下） |
| 驱动发送门 | `echo 1 > /sys/class/net/wwan0/link_state` |
| CM 自己跑 `udhcpc` | `ifup 2_1`（netifd 的 `udhcpc -s /lib/netifd/dhcp.script`） |

### 为什么地址交给 netifd 而不是自己跑 DHCP

busybox 的 `/usr/share/udhcpc/default.script` 里有一句**破坏性删除**：

```sh
eval $(route -n | awk '
    /^0.0.0.0\W{9}('$valid_gw')\W/ {next}
    /^0.0.0.0/ {print "route del -net "$1" gw "$2";"}
')
```

⇒ **凡「网关不是本次租约给的」默认路由一律删掉**。厂商 CM 直接调 busybox udhcpc，
会顺手删掉 `eth0` 的默认路由、把整机出口切到蜂窝（见 `notes/2026-09-21.md` §2.3）。
netifd 的 `/lib/netifd/dhcp.script` 是 metric 感知的，不碰别人的路由。

## ★ 关键前提：`AT+GTAUTOCONNECT=0`

出厂默认是 `1`。此时模组会在开机后**自己**拉起一个 RMNET/ECM 数据呼叫
（`AT+GTWWAN?` 回 `1,<cid>`），该呼叫占住 USB 数据面，宿主驱动按 QMAP 解析会读到畸形帧
（`drop skb_len=... larger than 1500`），**任何**拨号器都拿不到数据。

真机 A/B（同机同驱动，同一个厂商 CM 二进制）：

| `GTAUTOCONNECT` | `GTWWAN?` | 起 CM 的结果 |
|---|---|---|
| `1`（出厂） | `1,1` | ping 8/8 loss、Δrx=0 ❌ |
| `0` | `0` | ping 10/10、rx=750 ✅ |

设置（模组 NVRAM，持久；设一次即可）：

```sh
AT+GTAUTOCONNECT=0
AT+CFUN=1,1          # 或整机重启，让它生效
```

> 注：`GTAUTOCONNECT` 不在官方 V2.3/V2.5 文档里 —— 属 Fibocom 私有扩展 AT。
> 同族的 `AT+GTWWAN=?` 回 `(0,1),(1-23)`，`AT+GTSTATIS?` 是只读统计。

## ★ 关键前提 2：`SET_DATA_FORMAT` 不可省（冷启动必需）

模组复位/冷启动后：

```
data-aggregation-protocol = "unknown"      ul/dl max datagrams = 0
```

而驱动 `qmi_wwan_f` 在 bind 时对 `idProduct=0x0104` **强制** `qmap_mode=1`、
`qmap_size=16384`，给每个 TX 包加 QMAP 头（`FIBOCOM_QMAP_MUX_ID=0x81=129`），RX 也按 QMAP 解析。
⇒ 模组「不分帧」而驱动「按 QMAP 收」⇒ 宿主 rx 恒 0。

显式设置后：

```
data-aggregation-protocol = "qmap"         ul 11/8192   dl 32/16384
```

⇒ 数据面立即成立。

**曾经误判**：以为 uqmi 没有这个能力、需要打补丁。实际设备上的 uqmi
`--wda-set-data-format` **原生带聚合选项**（`--ul/dl-aggregation-protocol`、
`--ul/dl-datagram-max-count/size`、`--flow-control`），之前是被 `grep` 过滤 help 时筛掉了。
唯一确实没有的是 `endpoint_info` TLV —— 真机实测不影响可用。

## 与 `fm160d` 的关系

`fm160d` 是**纯 AT 侧**守护进程（模块识别、band lock、cell lock、GNSS 配置），
不持有 `/dev/cdc-wdm0`，**可以与本服务并存**。

不过 `/dev/cdc-wdm0` 是**单 reader** 设备：任何要动它的工具（`uqmi` / `qmicli` / `quectel-CM`）
都必须独占。本实现会在 `do_up` 开始时 `killall quectel-CM*`，并在 `status` 里报告厂商工具进程数。
**不要同时跑厂商 CM 与本服务。**

## 验收证据（真机，hot 态）

```
SET_DATA_FORMAT ok (raw-ip + ul/dl=qmap, 11/8192, 32/16384)
start-network ok, pdh=1264268368
data status: connected
地址已就绪: 10.198.173.20/29
ping -c 10 -I wwan0 223.5.5.5  → 10/10, 0% loss, min/avg/max = 30.379/35.903/40.497 ms
数据格式（回读）                → raw-ip + qmap, ul 11/8192, dl 32/16384
AT+GTSTATIS?                   → 0,38,10327,8221   （模组侧第三方佐证，非零）
udhcpc 数                      → 0（没有额外 DHCP 客户端）
```

## ★★ QMI「偶发超时」的真机理（三轮独立开机取证，2026-09-21）

取证脚本在设备 `/root/`：`128-qmi-contention.sh`、`132-coldstart-probe.sh`、
`135-qmi-latency.sh`、`136-boot-rate.sh`、`138-mode-split.sh`。

### 一句话结论

> 这不是「超时」，也不是「服务没就绪」，而是**单次 QMI 请求级别的偶发失败**：
> 任何一条 uqmi 调用都有小概率拿到 `"Failed to connect to service"`（CTL 分配该服务
> 的 client id 失败）或 `"Unknown error"`（模组回了 uqmi 不认识的错误码）。
> **下一次同样的调用立刻就好。** 它在**开机后约 0–200 s 内显著更频繁（~1.8%）**，
> 稳态下低一个数量级（<0.17%）。

### 决定性证据

**① 同一命令、相隔 1.5 s、一错一对**（138 模式 Z，最直接的证据）：

```
t=63.20  nas=240ms/UNKNOWN   nas2=80ms/ok     ← 前一次错、后一次立刻成功
t=68.62  ctl=ok nas=ok wds=ok                 ← 自此进入全绿
t=89.42  nas=50ms/NOCONN     wds=50ms/ok      ← 孤立单点失败；同帧 WDS 正常
t=151.72 nas=110ms/ok        wds=20ms/UNKNOWN ← 这次换成 WDS 失败、NAS 正常
t=156.89 → 全绿直到 421s+（45+ 帧零失败）
```
⇒ 失败**不聚集在任何忙窗口**，且**落在哪个服务是随机的**。

**② 与 `--get-versions` 无关**（138 三模式轮转，各占 1/3 帧）：

| 模式 | 形状 | 是否失败 |
|---|---|---|
| X | `--get-versions` → `--get-serving-system` | 失败 2 次 |
| Y | `--get-serving-system` 单独（**无 ctl 前置**） | 失败 3 次 |
| Z | `--get-serving-system` → `--get-serving-system` | 失败 1 次 |

⇒ 曾被怀疑的「`--get-versions` 把 NAS 打进忙态」**被否定**。

**③ 不是慢响应**（135，稳态，240 次 NAS 调用）：

```
nas-4k-0  (TO=4000)  : min=80ms avg=87ms max=100ms slow(>=500ms)=0 fail=0/60
nas-4k-1  (TO=4000)  : min=90ms avg=109ms max=110ms slow(>=500ms)=0 fail=0/60
nas-20k-0 (TO=20000) : min=80ms avg=87ms max=110ms slow(>=500ms)=0 fail=0/60
wds-4k-0  (TO=4000)  : min=20ms avg=25ms max=50ms  slow(>=500ms)=0 fail=0/60
wds-20k-0 (TO=20000) : min=20ms avg=24ms max=50ms  slow(>=500ms)=0 fail=0/60
```
⇒ 正常就是 87 ms，**没有长尾**。所以早期那些「吃满 4 s / 6 s 超时」的帧，
其响应是**根本没回来**，而不是回来了但很晚。

**④ 不是多 reader 争用**（128）：单进程串行 / 双进程并发 / 双进程并发+`flock`
各 40~80 次，`/dev/cdc-wdm0` **全部 0 失败**。

**⑤ 不是「服务未就绪」**：138 里 uptime **68 s 起就持续全绿**，
但失败仍在其后的 89.42 s、151.72 s 复发 ⇒ 失败窗口比就绪点晚得多。

### 失败率量化

| 区间 | 调用数 | 失败 | 失败率 |
|---|---|---|---|
| 开机后 0–200 s（132/136/138 合并） | ~330 | 6 | **≈1.8%** |
| 稳态（135 的 240 次 + 136/138 后期 ~350 次） | ~590 | **0** | **<0.17%** |

若稳态真有 1.8%，590 次全绿的概率仅 `0.982^590 ≈ 2×10⁻⁵` ⇒ 稳态确实显著更低。

### 对实现的直接要求（已落地）

1. **就绪门不许用 `--get-versions`**：它一次要问多个服务的版本，命中概率被放大数倍；
   而它在 NAS 单点失败时会把整条命令判成失败。已改为**只问 WDS**
   （拿合法 client id + `--get-data-status` 有应答），见 `wait_qmi`。
2. **`wait_qmi` 里不许循环 `--get-client-id`**：每成功一次就永久占掉一个 client id
   （实测 1→35 单调泄漏、不复用；`uqmi --sync` 只能部分回收）。现改为最多分配一次并缓存。
3. **每条命令都要能重试**：`qretry` 保留，冷启动期的重试预算要覆盖 ~200 s 窗口。
4. **稳态不需要额外加固**：QMI 在稳态是可靠的。

## ★★★★★ 5G SA 实网验收 + 生产路径（2026-09-21）

模组驻留 **5G NR / SA** 的一轮完整验收（此前一直是 LTE，见"已知边界"里那条纠正）：

```
AT+C5GREG?                  -> +C5GREG: 0,1                 # 5GS 已注册
AT+COPS?                    -> 0,0,"????",11               # act=11 = NR connected to 5GCN
uqmi --get-serving-system   -> radio_interface:["5gnr"]    plmn 460/11
uqmi --get-system-info      -> 5gnr.service_status:"available"

fm160-qmi up                -> IPv4 由 netifd(udhcpc) 落地   10.65.8.127/24
status                      -> link_state=0x1 carrier=1 data-status="connected"
                               current-settings: "pdp-type":"ipv4v6" "ip-family":"ipv4"
路由                        -> default via 10.65.8.128 dev wwan0 proto static src 10.65.8.127 metric 11
DNS                         -> /tmp/resolv.conf.d/resolv.conf.auto: 218.2.2.2 / 218.4.4.4
ping 223.5.5.5              -> 6/6，0% loss，36.3/176.5/535.9 ms
wget（40 MB，走 DNS+TCP）    -> rc=0；GTSTATIS 第3字段 40038798 → 101537512（+61.5 MB）；tx_errors=0
```

`ipfam=ipv6` 单栈也端到端通（同一轮）：`240e:479:1650:2118:8ce2:7502:b2c3:e8dc/64`、
`ping6 240e:5a::6666` = **26.7 ms**、`ping6 240c::6666` = **202 ms（跨 AS）**。
⚠️ **ping 网关不回是正常的**：raw-ip 链路上"网关"只是模组内部下一跳，不回 ICMP。

### ★★ 数据面的真正前提是 **power cycle**，不是任何 AT/配置项

反复拨号/折腾之后会出现这个**全绿但零收包**的形态：

```
data-status="connected"、聚合="qmap"、地址+DNS 都对、link_state=0x1、carrier=1
但  ping 100% loss、rx_bytes=0、模块侧 AT+GTSTATIS? 恒 "0,0,0,0"
    wwan0 tx_packets=0 而 tx_errors 随发包数同步 +N   ← USB TX URB 全部失败
```

**只有整机 `reboot` 能清掉。** 更狠的一条：**`rmmod/insmod qmi_wwan_f` 本身就会把它打死**
—— 所以**换 `qmap_mode` 的正确姿势是 reboot，不是 reload**。

### ★ `Depends` 与安装

`depends = "uqmi netifd"`，但 opkg 只会保留真正存在的包名：
```
opkg list-installed | grep fm160   ->  fm160-qmi - 1.0.2-1
```
`.ipk` 外层是 **gzip+tar**（不是 ar！），详见 `notes/2026-09-21.md` §7.4；打包器 `tools/140-build-ipk.py`
默认输出该形态，`tools/141-verify-ipk.py` 会校验成员顺序。

## 已知边界

- **`qmap_mode` 必须是 1**（本脚本已自愈，仍要写在这里）：
  `qmi_wwan_f.c:2445-2447` 里 `qmap_mode == 1` 时 `mpQmapNetDev[0] = dev->net`，
  ⇒ **基网卡 `wwan0` 才是数据口**；`mode>1` 时 RX 只投到 `wwan0.N`，
  而地址/发送门/健康检查都挂在 `wwan0` 上 ⇒ **静默失败（全绿但零收包）**。
  `ensure_qmap_mode()` 启动时会检测并自愈。
- **netifd 处于 `pending` 时会冲掉手工落的地址** ⇒ QMI 兜底前**必须先 `ifdown`**，
  否则出现"apply 报成功、下一句地址就没了"。等待用 `dhcpwait`（默认 90 s），
  判据用 `ubus call network.interface.2_1 status` 的 `ipv4-address`。
  本模组**确实应答 DHCP**，只是要 20–60 s。
- **冷启动等待**：host reboot 后 `/dev/cdc-wdm0` 约 60 s 出现，
  **WDS 服务可应答约在 65–110 s**；此后仍有约 2 分钟的单点偶发失败窗口。
  脚本 `wait_qmi` 预算 240 s，守护模式会持续重试。
- **多路 PDN：未走通**（2026-09-21 结论，撤回早先"已打通"的说法）。
  两道独立的坎：① 第二路 `start-network` 直接 `"Call failed"`（换 APN 也一样）；
  ② **`qmap_mode>1` 的数据面本身不工作** —— `wwan0.1` `tx_packets` 在涨但
  `rx_packets` 恒 0、base `tx_errors` 同步涨、**模块侧 `GTSTATIS` 恒 0,0,0,0**
  ⇒ 包根本没到模组。正确形态是**每路一个 WDS client**（`BIND_MUX_DATA_PORT` 按 client 生效，
  用同一个 client 连做两次会被顶掉、两次回同一 PDH）。细节与后续方向见 `notes/2026-09-21.md` §7.7。
- **QMI 偶发失败**：见上一章，稳态 <0.17%，无需额外加固。
- **UIM/短信** 未涉及。
