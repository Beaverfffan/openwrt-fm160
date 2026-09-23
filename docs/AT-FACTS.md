# FM160/FG160 官方文档事实摘录（实现依据）

> 来源（用户提供，均已抽取为可检索文本，见 `_ref/fm160/docs/`）：
> - `AT-Commands-FM160-FG160.txt` — FM160&FG160 AT Commands User Manual **V1.0.2**（348 页，109 条命令）
> - `Dialup-ECM-NCM-RNDIS-MBIM.txt` — ECM&NCM&RNDIS&MBIM 拨号集成指导_Linux **V1.0**（51 页）
> - `GNSS-Application-Guide.txt` — FM160 Application Guide_GNSS **V1.0**（22 页）
>
> 凡本文标注 ⚠️ 的都是「踩了就难回头」的点，实现里必须有对应守卫。

---

## 1. USB Profile：`AT+GTUSBMODE`（拨号模式的物理前提）

### 1.1 手册给出的候选值（11.1.2）

> 「supported mode depends on the target device and they may be as below」

| mode | 接口组成 | 含 AT 口 |
|---|---|---|
| 17 | DIAG + MODEM + AT + PIPE + RMNET + ADB | ✔ |
| 18 | DIAG + MODEM + AT + PIPE + ECM + ADB | ✔ |
| 20 | MODEM | ✘ |
| 21 | MODEM + AT | ✔（无数据口） |
| 24 | RNDIS + MODEM + DIAG + ADB | ✘ |
| 29 | MBIM + AT + DIAG | ✔ |
| 30 | MBIM + MODEM + DIAG + AT | ✔ |
| 31 | DIAG + MODEM + RMNET + DPL + QDSS + ADB | ✘ |
| 32 | DIAG + MODEM + AT + PIPE + RMNET | ✔ |
| 33 | DIAG + MODEM + AT + PIPE + ECM | ✔ |

- 持久化：Persistent = Yes；**改动后需 reset / power cycle 才生效**。
- 响应快（< 1s）。

### 1.2 拨号文档的 4 张端口表（**平台不同，mode 号含义不同**）

拨号文档 `2.1 USB 端口信息` 里有 **4 张表**，必须按平台对号入座：

| 表 | 章节 | 平台 | VID | PID 段 | mode |
|---|---|---|---|---|---|
| **表 1** | 2.1.1 | **高通**（FM150/FG150/FM100/FM101/FG101/FM130/**FM160**/NL95X） | `0x2CB7` | `010x` | 17,18,19,20,21,22,23,24,28,29,30,32,33 |
| 表 2 | 2.1.1 | 另一平台 | `0x1508` / `0x05C6` | 0x1000/0x1001/0x9025/0x90B6 | 17,18,19,22,24,25 |
| 表 3 | 2.1.2 | 展锐（UNISOC） | `0x2CB7` | `0x0A05/0x0A06/0x0A07` | 36,37,38,39,40,41 |
| 表 4 | 2.1.3 | MTK（FM350/FG360） | `0x0e8d` | `0x7126/0x7127` | 40,41 |

⚠️ **只有「表 1 且 VID:PID = 2CB7:010x」适用于 FM160。**
⚠️ **同一个 mode 号在不同表里含义不同**：mode 19 在表 1 有 AT 口（ECM+AT），在表 2 的 `05C6:9025` 里**没有 AT 口**（RmNet+Mystorage）。⇒ **不能用 mode 号本身判断安全性，必须先核对平台。**

**表 1 全表（本项目唯一依据）**

| mode | PID | 接口号 → 接口名 | 含 AT 口 |
|---|---|---|---|
| 17 | **0104** | 0 DIAG / 1 Modem / 2 **AT** / 3 Pipe / 4 **RmNet** / 5 ADB | ✔ |
| 18 | **0105** | 0 DIAG / 1 Modem / 2 **AT** / 3 Pipe / 4 ECM / 5 ECM / 6 ADB | ✔ |
| 19 | 0106 | 0 DIAG / 1 Modem / 2 **AT** / 3 ECM / 4 ECM | ✔ |
| 20 | 0107 | 0 Modem | **✘** |
| 21 | 0108 | 0 Modem / 1 **AT**（无数据口） | ✔ |
| 22 | 0109 | 0 Modem / 1 **AT** / 2 **RmNet** | ✔ |
| 23 | 010A | 0 Modem / 1 **AT** / 2 ECM / 3 ECM | ✔ |
| 24 | 010B | 0 RNDIS / 1 RNDIS / 2 Modem / 3 DIAG / 4 ADB | **✘** |
| 28 | 010F | 0 MBIM / 1 MBIM | **✘** |
| 29 | 0110 | 0 MBIM / 1 MBIM / 2 **AT** / 3 DIAG | ✔ |
| 30 | 0111 | 0 MBIM / 1 MBIM / 2 Modem / 3 DIAG / 4 **AT** | ✔ |
| 32 | **0104** | 0 DIAG / 1 Modem / 2 **AT** / 3 Pipe / 4 **RmNet** | ✔ |
| 33 | **0105** | 0 DIAG / 1 Modem / 2 **AT** / 3 Pipe / 4 ECM / 5 ECM | ✔ |

⚠️ **17 与 32 共用 PID 0104；18 与 33 共用 PID 0105。**
⇒ **禁止用 VID:PID 推断当前 mode**，必须 `AT+GTUSBMODE?` 回读。PID 只能用来「模块在不在」+「是不是 010x 段（即表 1 适用）」。

⚠️ **无 AT 口 = 单向门**（`20`、`24`、`28`，加上 AT 手册 11.1.2.4 独有列出的 `31` = DIAG+MODEM+RMNET+DPL+QDSS+ADB）。切进去之后路由器侧再没有任何 AT 通道可以切回来。
⇒ **硬黑名单 `{20, 24, 28, 31}`，无旁路。** 其余「表 1 有、但 AT 手册 11.1.2.4 未列出」的 mode（19/22/23）属 `device dependent`，必须用户显式确认。

> 供查阅的机器可读版本：`_ref/fm160/docs/USB-MODES.txt`（由 `_tools/` 脚本从 PDF 文本直接抽取，含四张表的原始行）。

### 1.3 官方建议的上电/断电时序（拨号文档 3.4）

- 断电 → 再上电：**> 12 s**（等电容放电）
- 相邻两次**开机**：**> 90 s**
- 相邻两次**断电**：**> 300 s**
- 开机后等 **90 s** 仍枚举不出端口 ⇒ 判本次开机失败，重新开机（或延时 5 s 起连续发 AT，最长待 90 s）

---

## 2. 拨号：数据面怎么建

### 2.1 ECM / RNDIS / NCM（**不用** QMI/MBIM 协议栈，靠模块内部拨号）

官方流程（3.5/3.6）：

```
AT+CPIN?                      → +CPIN: READY
AT+CREG?                      → +CREG: 0,1 或 0,5     （CS 域）
AT+CGREG? / AT+CEREG?         → 0,1 或 0,5             （PS 域 / 4G）
AT+CGDCONT=1,"IP","<APN>"     → OK
AT+GTWWAN=1,1   (ECM/RMNET)   → OK        ← 或 AT+GTRNDIS=1,1（RNDIS）
AT+GTWWAN?                    → +GTWWAN: 1,1,"IP","pdns","sdns"   ← 拿到 IP 才算成功
```

- **结束判定**（官方明确）：收到 `OK` / `ERROR` / `+GTWWAN: 0` / **210 s 超时**，四者之一即认为本次指令结束，**然后**才用 `AT+GTWWAN?` 确认。
- **失败策略**（官方）：连续 5 次 `AT+GTWWAN?` 拿不到 IP ⇒ 回到 `AT+CPIN?`；连续 5 次拨号失败 ⇒ **复位模块**。
- **断开**：`AT+GTWWAN=0,<cid>`；连续 5 次仍失败 ⇒ 复位。
  ⚠️ 官方原话：**不能直接拔 USB 线断开数据业务，也不能直接断电或重启模块**。
- ⚠️ **文档自相矛盾**：AT 手册 11.1.15 说 ECM/RMNET 用 `+GTWWAN`、RNDIS 用 `+GTRNDIS`；但拨号文档 V1.0 的 **ECM 章节示例里用的是 `AT+GTRNDIS=1,1`**（含 FM160 示例）。
  ⇒ **实现必须在运行期探测**：`AT+GTWWAN=?` 与 `AT+GTRNDIS=?` 哪个返回 OK 用哪个，不硬编码。

### 2.2 QMI（mode 17/22/25/32）

- 主机驱动 `qmi_wwan` → `/dev/cdc-wdm0` + `wwan0`
- OpenWrt 侧：`uqmi` + netifd `proto qmi`

### 2.3 MBIM（mode 29/30）

- 主机驱动 `cdc_mbim` → `/dev/cdc-wdm0` + `wwan0`
- 拨号：`mbimcli --connect=session-id=0,apn=<apn>`；查：`--query-connection-state` / `--query-ip-configuration`
- 断开：`mbimcli -d /dev/cdc-wdm0 --disconnect="0"`
- OpenWrt 侧：`umbim` + netifd `proto mbim`
- ⚠️ 官方提醒：mbimcli 版本决定功能可用性（1.14 起），与模块无关。

### 2.4 多路 PDN（VLAN，本期可选）

```
AT+GTWWAN=1,1        # CID1
AT+GTWWAN=1,3        # CID3 → 模块内部自动建 VLAN 1
# 主机侧
ip link add link usb0 name usb0.1 type vlan id 1
dhclient usb0.1
echo 0 > /proc/sys/net/ipv4/conf/all/rp_filter     # 必须关反向路由检查
```

相关专有命令：`+GTMPDN`（VLAN 多 PDN 开关）、`+GTMAPVLAN`（VLAN ID 映射）、`+GTIPPASS`（IP passthrough）、`+GTRMNETMAP`（RMNET NIC 映射顺序）。

---

## 3. 内核侧改动要求（拨号文档 §2）

### 3.1 必需驱动

| 用途 | 内核配置 |
|---|---|
| AT 串口 | `CONFIG_USB_SERIAL_OPTION`（USB driver for GSM and CDMA modems） |
| ECM | `CONFIG_USB_NET_CDCETHER`（CDC Ethernet support） |
| NCM | `CONFIG_USB_NET_CDC_NCM` |
| MBIM | `CONFIG_USB_NET_CDC_MBIM` |
| RNDIS | `CONFIG_USB_USBNET` + `CONFIG_USB_NET_RNDIS_HOST` |
| QMI | `CONFIG_USB_NET_QMI_WWAN` |

### 3.2 option 驱动需要认识 FM160 —— 结论：本项目**不打**这个补丁，但必须避开 mode 21 / 29

厂商文档给了两种写法（全局 `option_blacklist_info` 结构 或 就地在 `usb_device_id` 用 `RSVD(n)`），
并按 `USBMODE = n` 逐条分配保留接口位。

**但真正决定 AT 口能不能出现的不是这张表，而是内核 `drivers/usb/serial/option.c` 里有没有对应 PID 的条目。**
option 先按 VID:PID 匹配，匹配不上就根本不会去 probe 任何接口 —— USB 描述符再健康也没用。

#### (1) 厂商给的保留位表（原文 §2.3 `option.c` 节选，VID 0x2CB7）

| PID | mode | 厂商 reserved bits | 表 1 端口表里有吗 |
|---|---|---|---|
| 0104 | 17 / 32 | `RSVD(4)\|RSVD(5)` | ✓ |
| 0105 | 18 | `RSVD(4)\|RSVD(5)\|RSVD(6)` | ✓ |
| 0105 | 33 | `RSVD(4)` | ✓ |
| 0106 | 19 | `RSVD(3)\|RSVD(4)` | ✓ |
| 0109 | 22 | `RSVD(2)` | ✓ |
| 010A | 23 | `RSVD(2)\|RSVD(3)` | ✓ |
| 010B | 24 | `RSVD(0)\|RSVD(1)\|RSVD(4)` | ✓ |
| 010C / 010D / 010E | 25 / 26 / 27 | `RSVD(4)\|RSVD(5)\|RSVD(6)` | ✗ 只出现在 option.c 列表 |
| 010F | 28 | `RSVD(0)\|RSVD(1)` | ✓ |
| 0110 | 29 | `RSVD(0)\|RSVD(1)` | ✓ |
| 0111 | 30 | `RSVD(0)\|RSVD(1)` | ✓ |
| — | 20 / 21 / 31 | **厂商两张列表里都没有** | 20/21 在表 1，31 不在 |

⚠️ 20（0x0107）和 21（0x0108）在厂商自己的两张 `option.c` 列表里**都没出现**，尽管表 1 给了完整布局
（20 = 只有 Modem；21 = Modem + AT）。

#### (2) 内核实际带了哪几条（数据来自构建机已解出的 6.6.127 源码）

| PID | mode | 内核条目 | 与厂商建议的差别 |
|---|---|---|---|
| 0104 | 17 / 32 | `USB_DEVICE` + `RSVD(4)\|RSVD(5)` | **逐位一致** ✓ |
| 0105 | 18 / 33 | `USB_DEVICE_INTERFACE_CLASS(…,0xff)` + `RSVD(6)` | 少 4/5；但 4/5 是 ECM(class 0x02)，靠 class 已挡住 ⇒ 等效 |
| 0106 | 19 | 有，无 driver_info | — |
| 010A | 23 | 有 | — |
| 010B | 24 | 两条 `AND_INTERFACE_INFO`（Diag / AT） | — |
| 0111 | 30 | 有，无 driver_info | 0/1 是 MBIM 类 ⇒ 等效 |
| 0107 / 0108 / 0109 / 010C / 010D / 010E / 010F / 0110 | 20/21/22/25/26/27/28/**29** | **无条目** | — |

清零验证（在编译机上跑过，结果为 `0x0104=1 0x0105=1 0x0106=1 0x0107=0 0x0108=0 0x0109=0
0x010a=1 0x010b=2 0x010c=0 0x010d=0 0x010e=0 0x010f=0 0x0110=0 0x0111=1`）：

```sh
F=<build_dir>/…/linux-6.6.127/drivers/usb/serial/option.c
for p in 0104 0105 0106 0107 0108 0109 010a 010b 010c 010d 010e 010f 0110 0111; do
    printf '0x%s %s\n' "$p" "$(grep -c "0x2cb7, 0x$p" $F)"
done
grep -rn '0x0110\|0x010f' target/linux/     # 树内没有任何补丁补上它们
```

#### (3) 结论

* **本项目允许切换的 mode（17 / 18 / 32 / 33 / 30）全部有内核条目**，AT 口照常落在 `ttyUSB2`。
  厂商补丁对本项目**不必要** —— 这也正是 32 模式（0x0104）在 H69K 真机上 AT 正常的原因。
* ⚠️ **mode 29（0x0110）必须按「禁止」处理**：模块侧有 AT 口，主机侧没有 `ttyUSB*`。
  切过去 = 永久失去管理通道，且无法从系统侧复位。已写入 `api.js` 的 `USB_MODE_NO_KERNEL_DRIVER`，
  并从 MBIM 候选表里删除（MBIM 只剩 30）。
* mode 21（0x0108）同理不可切；它的 AT 口只会出现在 `ttyUSB1` 而非 `ttyUSB2`，
  属于"靠手工还能救回来"，但仍然不提供。
* 若将来确实要 21 / 29，每个 PID 一行补丁即可：
  `{ USB_DEVICE_INTERFACE_CLASS(0x2cb7, 0x0110, 0xff), .driver_info = RSVD(0) | RSVD(1) },`
  但那时必须重新真机验证，不能只凭这一行。

⚠️ 内核数据来自 **6.6.127**。本树目标已升到 **6.6.144**；option.c 对这几个 PID 的覆盖在两者之间没有变化，
但刷机后仍应复核一次（看 `ls /dev/ttyUSB*` 的数量即可）。

### 3.3 ⚠️ 零包（ZLP）机制 — 长 AT 命令的潜在杀手

文档 §2.4 要求在 `drivers/usb/serial/usb_wwan.c` 的 `usb_wwan_setup_urb()` 里：

```c
if (dir == USB_DIR_OUT) {
    struct usb_device_descriptor *desc = &serial->dev->descriptor;
    if (desc->idVendor == cpu_to_le16(0x2cb7))
        urb->transfer_flags |= URB_ZERO_PACKET;
}
```

**为什么重要**：AT 写入时若长度恰好是端点包长整数倍而没补零包，模块侧会一直等后续数据 ⇒ 表现为「AT 挂住、超时」。长 APN、`AT+CGDCONT`、以及**短信 PDU（`AT+CMGS`/`AT+CMGW` 通常 > 64 B）**最容易被命中。
⇒ 真机验证清单第一条：发一条 > 70 字节的 PDU 看是否稳定。若不稳 ⇒ 上 ZLP 补丁。

---

## 4. 命令地图（109 条中与本项目相关的）

### 4.1 模块身份 / 状态
`AT+CGMI` `AT+GMI` `AT+CGMM` `AT+GMM` `AT+CGMR` `AT+GMR` `AT+CGSN`(IMEI) `AT+GSN` `AT+CFSN`(出厂 SN) `AT+CNUM`(本机号) `AT+CCID`(ICCID) `AT+GTUSIM`

### 4.2 控制 / 温度 / 电源
`AT+CFUN`(0/1/4；返回 `+CFUN: 1` 才算正常) `AT+GTFMODE`(硬件飞行模式) `AT+CPWROFF` `AT+MTSM`(温度传感器) `AT+MMAD`(ADC 电压) `AT+GTDUALSIM` `AT+GTWAKE` `AT+SLPMODE`

### 4.3 SIM
`AT+CPIN` `AT+TPIN` `AT+CPINR`(剩余重试次数) `AT+CPWD` `AT+CLCK` `AT+CRSM` `AT+CSIM`

### 4.4 网络 / 信号 / 频段 / 小区
`AT+CSQ`(rssi,ber；rssi>0 且 ≠99 才有效) `AT+CESQ`(扩展：rsrp/rsrq) `AT+CREG` `AT+CGREG` `AT+CEREG` `AT+C5GREG` `AT+COPS` `AT+CPLS` `AT+CPOL` `AT+COPN` `AT+CEMODE` `AT+GTRAT` **`AT+GTACT`**(RAT+频段) **`AT+GTCCINFO`**(服务+邻区，含 RSRP/RSRQ/SINR) `AT+GTCAINFO`(CA) **`AT+GTCELLLOCK`**(锁小区)

### 4.5 数据
`AT+CGDCONT` `AT+CGATT` `AT+CGACT` `AT+CGPADDR` `AT+CGCONTRDP` `AT+CGEREP` `AT+CGAUTH`/`AT+MGAUTH` `AT+CSCON` `AT+GTSTATIS`(收发包计数)

### 4.6 专有
`AT+GTUSBMODE` `AT+GTWWAN` `AT+GTRNDIS` `AT+GTMPDN` `AT+GTMAPVLAN` `AT+GTIPPASS` `AT+GTAUTOCONNECT` `AT+GTAUTODHCP` `AT+GTDNS` `AT+GTPREDNSCFG` **`AT+GTURCMODE`** `AT+GTROAMCFG` `AT+GTECMDOWNEN` `AT+GTRMNETMAP`

### 4.7 短信
`AT+CSCS` `AT+CSMS` `AT+CPMS` `AT+CMGF` `AT+CSCA` `AT+CSMP` `AT+CSDH` `AT+CNMI` `AT+CNMA` `AT+CMGL` `AT+CMGR` `AT+CMGS` `AT+CMGW` `AT+CMSS` `AT+CMGD` `AT+CSCB` `AT+SMMFULL`

### 4.8 GNSS
`AT+GTGPSPOWER` `AT+GTGPS` `AT+GTGPSEPO` `AT+GTAGPSSERV` `AT+GTGPSCFG` `AT+GTGPSCERT`

---

## 5. 信号与频段编码（**仅用于显示映射**）

### 5.1 `AT+GTACT` 的 RAT 值

| rat | 含义 |
|---|---|
| 1 | UMTS |
| 2 | LTE |
| 4 | LTE/UMTS |
| 10 | Auto（注意：**写入 10 后回读会变成 20**） |
| 14 | NR-RAN |
| 16 | NR-RAN/WCDMA |
| 17 | NR-RAN/LTE |
| 20 | NR-RAN/WCDMA/LTE |

`<PreferredAct1/2>`：2 = WCDMA 优先，3 = LTE 优先，6 = NR 优先。

### 5.2 频段编码

| RAT | 编码 |
|---|---|
| UMTS | = 频段号本身（1..25） |
| LTE | = **100 + 频段号**（B3 → 103，B41 → 141，B64 → 164） |
| NR | = **500 + 频段号**（N1 → 501，N78 → 578，N79 → 579） |

写频段的写法示例（官方）：
- 只锁频段、不改 RAT：`AT+GTACT=,,,160,155`（前 3 个参数留空）
- LTE B3 + NR N78：`AT+GTACT=,,,103,5078`（手册原文如此，疑为 `578` 的排版错误）

⚠️ **一律以 `AT+GTACT=?` 返回的设备支持列表为准**。手册的 `5010`/`50512` 是 PDF 抽文本时的粘连，不要照抄。

### 5.3 `AT+GTCCINFO?` 输出结构（信号页的主数据源）

一条命令同时给出**服务小区 + 最多 10 个邻区**，分 4 种形态：

```
UMTS 服务小区:  <IsServiceCell>,<rat>,<mcc>,<mnc>,<lac>,<cellid>,<uarfcn>,<psc>,<band>,<ecno>,<rscp>,<rac>,<rxlev>,<reserved>,<Ec/Io_lev>
LTE 服务小区:   <IsServiceCell>,<rat>,<mcc>,<mnc>,<tac>,<cellid>,<earfcn>,<physicalcellId>,<band>,<bandwidth>,<rssnr_value>,<rxlev>,<rsrp>,<rsrq>
NR  服务小区:   <IsServiceCell>,<rat>,<mcc>,<mnc>,<tac>,<cellid>,<narfcn>,<physicalcellId>,<band>,<bandwidth>,<ss-sinr>,<rxlev>,<ss-rsrp>,<ss-rsrq>
EN-DC:          LTE 行 + NR 行 各一条（rat 4 与 9）
```

`<IsServiceCell>`：1 = 服务小区，2 = 邻区。`<rat>`：0 无效 / 2 WCDMA / 4 LTE / 9 NR-RAN。
响应时间 < 3 s，Persistent = Yes。

### 5.4 `AT+GTCELLLOCK` 语法

```
AT+GTCELLLOCK=<mode>[,<rat>,<type>,<earfcn>[,<PCI>][,<scs>[,<nrband>]]]
```

| 参数 | 取值 |
|---|---|
| mode | 0 关闭 / 1 开启 |
| rat | 0 LTE / 1 NR / 2 UMTS |
| type | 0 锁 PCI/PSC / 1 锁频点 |
| earfcn | 0 – 4294967295 |
| PCI | LTE 0–503 / NR 0–1007 |
| scs | 0 = 15 kHz / 1 = 30 kHz |
| nrband | 500 + N |

⚠️ 三条官方硬约束：
1. 只想锁「上次关机前注册的 LTE/SA PCI」→ 下发 `AT+GTCELLLOCK=1`
2. **下发后必须重启 UE**（配置写在 EFS，重启才生效）
3. **换 SIM 卡前必须先关闭本功能**

### 5.5 `AT+GTURCMODE` — 主动减少 URC 干扰

`AT+GTURCMODE=<report_flag>,[URC]`：`report_flag` 0 = 不上报、1 = 上报；`URC` 是匹配子串（最长 10 字符），**最多可屏蔽 10 条 URC**。
⇒ 对本项目极其有用：把用不到的 URC 关掉，AT 流更干净、解析碰撞更少。但 **`+CMTI` 绝不能关**。

---

## 6. GNSS 事实（GNSS 手册全文 22 页）

> ⚠️ 本节是**手册派生**的事实。真机实测见 **§10** —— 含 `GTGPSPOWER` 不影响
> 连接性的实测判据、NMEA 的实际出口、本机（有天线但室内）看不到卫星、以及各调用的成本。
> **两者冲突时以 §10 为准**（例：本节说 `GTGPS` 的 `<item>` 形如 `"RMC"`，
> §10.4 记下**必须带引号**，不带引号是 `ERROR`）。

| 命令 | 作用 | 关键取值 |
|---|---|---|
| `AT+GTGPSPOWER` | GNSS 开关 | 0 关（默认）/ 1 开 |
| `AT+GTGPS[=<item>]` | 读 NMEA | item ∈ `"RMC"` `"GGA"` `"GSA"` `"GSV"`；不带参数 = 全部 |
| `AT+GTGPSEPO` | AGPS 开关 | 0 关（默认）/ 1 MSB / 2 MSA |
| `AT+GTAGPSSERV` | AGPS 服务器 | `AT+GTAGPSSERV="supl.qxwz.com",7276`（端口 1–65535） |
| `AT+GTGPSCFG=<x>,<value>` | 卫星/SUPL 配置 | x: 0 SUPL 版本（0=1.0,1=2.0）/ 1 xtra / 2 卫星组合 / 3 SUPL 证书 |
| `AT+GTGPSCERT` | 导入/删除 SUPL 证书 | `.der` 格式，num 1–9 |

卫星组合 `AT+GTGPSCFG=2,<v>`：0 = GPS+GLO，2 = GPS+GAL，3 = GPS+QZSS，4 = GPS+BDS+GAL，5 = GPS+BDS+GLO，6 = GPS+BDS+QZSS，7 = GPS+GLO+GAL，**14 = GPS+BDS+GAL+GLO+QZSS（全开）**，15 = 仅 GPS。

- 全部命令**不需要 SIM、不需要注网、不需要数据连接**，响应 < 500 ms。
- 除 `AT+GTAGPSSERV`/`GTGPSCFG`/`GTGPSCERT` 是「掉电保存 = Yes」，`GTGPSPOWER`/`GTGPS`/`GTGPSEPO` **掉电不保存** ⇒ 每次开机要重新 `AT+GTGPSPOWER=1`。

---

## 7. 其它值得注意的官方细节

- `AT+COPS=3,2` 后可 `AT+COPS?` 按名称查运营商（多 SIM 场景选 APN 用）。
- 专网卡若运营商没给账号密码：电信卡惯用 `card`/`card`；鉴权默认 PAP 或 PAP&CHAP。
- `AT+CGDCONT?` 在 FM160 上会返回多条（cid 1 ip / cid 2 ims / cid 3 cmnet / cid 4 cmwap / cid 5 sos），**不要假设只有 1 条**。
- `AT+CPIN?` 返回 `+CPIN: READY` 才算识别到 SIM；连续 90 s 查不到 ⇒ 官方建议复位模块。
- `AT+CSQ` 的 `<rssi>` 必须 > 0 且 ≠ 99；连续 90 s 不正确 ⇒ 复位模块。

---

## 8. 真机实测（H69K + FM160-CN，89614.1000.00.04.01.02）

以下全部来自设备实测；与手册不一致处，**以本节为准**。

### 8.1 端口布局（VID:PID = `2cb7:0104`，`AT+GTUSBMODE?` = 32 = QMI 模式）

`option` 驱动绑定 iface 0–3，`qmi_wwan` 绑定 iface 4：

| 接口 | 设备 | 用途 | 能否应答 `AT` |
|---|---|---|---|
| 0 | `/dev/ttyUSB0` | DIAG | 否（超时） |
| 1 | `/dev/ttyUSB1` | NMEA（GNSS 输出） | 否（超时） |
| 2 | `/dev/ttyUSB2` | **AT 口** | **是，约 1 s 内** |
| 3 | `/dev/ttyUSB3` | MODEM | 否 |
| 4 | `wwan0` | QMI 数据面 | — |

⇒ 探测顺序按 `bInterfaceNumber` 把 **iface 2 排最前**，省掉两次 4 s 超时。

**sysfs 陷阱**：`/sys/class/tty/ttyUSBn/device` 解析到的是 **usb_interface**，
所以 `<那>/../idVendor` **仍然是接口目录** —— 它只有 `bInterfaceNumber`，**没有 `idVendor`**。
`idVendor` 在再上一层（usb_device）。⇒ 必须 `realpath()` 之后**逐级上溯**找，
硬编码一级会让 VID 过滤全程静默失效。另：`bInterfaceNumber` 在 sysfs 里是 **`%02x` 十六进制**。

### 8.2 ★ `+CME ERROR` 是终结符，且传输层把它报成 `success`

at-daemon 的默认 end_flag 列表 = `OK` / `ERROR` / `+CME ERROR:` / `+CME ERROR:` / `NO CARRIER`。
匹配到任一即 `result = 0` ⇒ 回报 `status: "success"`。

**所以 `success` 的含义是「这次交互结束了」，不是「模块接受了命令」。**
实测后果：无 SIM 时 `AT+ICCID` 回 `+CME ERROR: 13`，被当成成功，错误文本被存成了 ICCID。

⇒ 判成功必须**先看响应文本**再看 status：含 `ERROR` 一律 `AT_STATUS_ERROR`，
统一在 `atq.c` 的 `sendat_cb` 里做（那是所有 AT 的唯一收口）。
`AT_STATUS_ERROR` 只记 `last_fail_ms`、**不累加熔断计数**（熔断只认 timeout），
所以无 SIM 的常态 CME ERROR 不会把模块误判成故障。

### 8.3 实测响应时间与取值形状

| 命令 | 实测 | 备注 |
|---|---|---|
| `AT` | 首次 ~1 s（含开端口），之后 < 50 ms | 探测用 |
| `AT+CSQ` | 24–40 ms | RSSI=25 → −63 dBm |
| `AT+CGMI` | 快速 | `Fibocom Wireless Inc.`（**无**前缀） |
| `AT+CGMM` | 快速 | `FM160-CN` |
| `AT+CGMR` | 快速 | `89614.1000.00.04.01.02` |
| `AT+CGSN` | 快速 | IMEI，纯数字无前缀 |
| `AT+CFSN` | 快速 | ⚠️ 回 `+CFSN: "FP62PE002F"` —— **带前缀和引号**，必须剥 |
| `AT+ICCID` | 18 ms | 无 SIM 时 `+CME ERROR: 13` |
| `AT+GTUSBMODE?` | 16 ms | 本机 `32` |

全链最差响应 **36 ms**，`queue_depth` 恒 0 —— 15 s / 10 s 的分层轮询对这块模块足够宽松。

⇒ 取值统一处理：先剥开头的 `+PREFIX:`，再剥首尾引号
（`cmds.c` 的 `first_value_line()`；`AT+CGMI` 那种裸串不受影响）。

### 8.4 设备上的第三方争用（**直接伤害 AT 稳定性**）

实测该设备（iStoreOS 24.10.8，刷过自制镜像）上同时存在：

* **`ModemManager` 在跑**（`S70modemmanager`，pid 9115）—— 它会去开它找到的每个 ttyUSB。
* **`S99adb-enablemodem` 在跑**，`adb wait-for-device` + `adb fork-server` 常驻。
  该脚本只为 TP-LINK LTE 模块（`0x2357:0x000D`）写，对 FM160 永不匹配，等于白占一个 adb server。
* `network.2_1` = `proto dhcp` on `wwan0`（qmodem 遗留命名），实测引发内核
  `wwan0: NETDEV WATCHDOG: transmit queue 0 timed out 5170 ms`。
* 另有 `qmi_wwan 2-1:1.4 wwan0: Cannot change a running device`。

⇒ 「fm160d 是 AT 口唯一属主」这个设计前提，在**这台机器上并不成立**。
动手前先确认没有别的进程会去开 `ttyUSB2`（`grep ttyUSB /proc/*/fd`）。

**已处置（2026-09-18，用户选的「最小改动」方案）**：
`/etc/init.d/modemmanager disable && /etc/init.d/modemmanager stop` ⇒
rc.d 链接已撤、`ModemManager`/`-wrapper`/两个 `-monitor` 全部退出、服务状态 `DISABLED`。
处置后实测：`ttyUSB0/1/3` **无任何持有者**，`ttyUSB2` 仅 `ubus-at-daemon` 持有，
`fm160d` PID 不变、`worst_response_ms` 仍为 40 ⇒ 处理是安全且有效的。
⚠️ 停之前它虽然**没抢到** AT 口（`ubus-at-daemon` 先开了），但 USB 一旦重枚举它就会去探所有 ttyUSB。

**按用户决定暂留**：`adb-enablemodem`（对 FM160 永不匹配，白占 adb server）与
`network.2_1`/`2_1v6`（`wwan0` 上的 dhcp/dhcpv6，M2 拨号阶段会一并处理）。

### 8.5 ★★ `sendat` 的 `timeout` 单位是**秒**，不是毫秒 —— 传错 = 整个 daemon 失联

三条独立证据都指向「秒」：

* `at-daemon/src/const.h`：`#define DEFAULT_TIMEOUT 5   // seconds`
* QModem 的参考客户端：`ubus_invoke(..., timeout * 1000 + 1000)` —— 即它对外按**秒**记账，
  只有 `ubus_invoke` 的预算才乘 1000 换成毫秒。
* 我们自己的 `fm160d`：
  `fm160d.h` 写着 `int timeout_ms;  /* sendat timeout, seconds internally */`，
  `atq.c` 做 `secs = (req->timeout_ms + 999) / 1000;` 再上线，
  而 `ubus_invoke()` 的客户端预算用 `req->timeout_ms + 2000`（**这里**才是毫秒）。
  ⇒ LuCI（`debug.js` 传 5000/20000 ms）→ `fm160d` → `at-daemon` 这条链**是对的**。

**传错的后果不是「早一点超时」，而是整个 daemon 失联：**

* `at_handler.c` 的等待是 `abs_timeout.tv_sec += timeout;` + `pthread_cond_timedwait()`，
  单位是**秒**。所以 `"timeout":2000`（本想 2 s）⇒ daemon 原地等 **2000 秒（33 分钟）**。
* 更致命的是 `ubus_sendat_method` 是**在 ubus 主循环里同步执行**的：
  这一条请求卡住时，`list` / `close` / `open` … **所有**方法都不再应答。
* 实测（2026-09-18 21:00）：一条 `timeout:2000` 的探针打出去之后 ——
  * `ubus call at-daemon list` 30 s 无响应，客户端报 `Request timed out`；
  * `fm160d` 每 4 s 打一条 `sendat invoke failed: Request timed out`，
    状态掉到 `port_found=false`、`port=""`、`consec_timeout=3`、`last_ok_age_ms` 一路涨到 198634；
  * `procd` 收到 SIGTERM 后**杀不掉它**（阻塞在 `pthread_cond_timedwait` 里没看信号），
    最后是 `not stopped on SIGTERM, sending SIGKILL instead`。

**为什么这个坑一直藏着**：此前所有探针命令都在 14–30 ms 内命中 `OK` 终结符，
`timeout` 路径**一次都没被走到**。只有「模块根本不回答」时才会炸。

⇒ **纪律**：任何直接调 `at-daemon sendat` 的脚本/工具，`timeout` 一律按**秒**写（`2`、`6`）；
传 `2000`/`6000` 这种数就是 33 分钟 / 100 分钟的失联。
`_probe/` 下的 `04`–`07` 原来写的正是 `T=6000` / `t=${3:-6000}`，已全部改成秒并加了警告头。

### 8.6 四个接口的应答性（逐口探完，2026-09-18）

同一时刻的映射（`realpath /sys/class/tty/ttyUSBn/device` ⇒ `.../2-1:1.n/ttyUSBn`）：

| 端口 | USB 接口 | 驱动 | 应答 AT？ | 实测证据 |
|---|---|---|---|---|
| `/dev/ttyUSB0` | `2-1:1.0`（DIAG） | `option1` | **否，完全静默** | 4 条全 `status:timeout`、`response_time_ms:2000`，且**没有** `partial_response` |
| `/dev/ttyUSB1` | `2-1:1.1`（官方称 NMEA） | `option1` | **是** | `AT`→`\r\nOK\r\n` 20 ms；`ATI` 整段身份 142 B / 22 ms；`AT+CGMM`→`FM160-CN` 14 ms |
| `/dev/ttyUSB2` | `2-1:1.2`（AT） | `option1` | **是** | 14–29 ms，`fm160d` 的正式 AT 口 |
| `/dev/ttyUSB3` | `2-1:1.3`（MODEM） | `option1` | **否，只回显** | 全 `status:timeout`；`partial_response` **就是我们自己发出去的** `AT\r\n\r\n` |

★★ **「iface 1 = NMEA、iface 2 = AT、iface 3 = MODEM」这个官方端口描述在本机不成立**：

* **iface 1 是一个能用的 AT 口**，而且**不吐 NMEA** —— 用永不匹配的终结符做 2 秒原始抓取，
  只收到 `\r\nOK\r\n`，没有 `$GPxxx`/`$GNxxx` 之类的语句流。
* **iface 3 反而是半个死口**：有回显、没有 AT 解释器（`partial_response` 恰好等于发出去的命令）。
* ⇒ 本机实际可用 AT 口**有两个**（iface 1 与 iface 2）。这不是坏事：iface 1 是天然的**备份 AT 口**。

定位接口号的方法（别只看第一层）：`bInterfaceNumber` 在 `/sys/class/tty/ttyUSBn/device`
**上一层的 interface 目录**里，且格式是 **`%02x`**（`00/01/02/03`）；
`idVendor` 还要再往上到 usb_device 节点才有。

### 8.7 `fm160d` 的探测序（实测 ⇒ 已修正并部署）

实测启动日志：

```
AT candidates: 4 Fibocom (2cb7:*), 0 unclassified, 0 foreign
  | probe order: /dev/ttyUSB2(if2) /dev/ttyUSB3(if3) /dev/ttyUSB1(if1) /dev/ttyUSB0(if0)
AT port is /dev/ttyUSB2          <- 启动后 1 秒内命中首个候选
```

* 正常路径**不会**走到 if3/if0，因为它们排在第 3/4 位，而 if2 一击即中。
* 但 if3 与 if0 **永不响应**，每走到一个就要付一次完整超时（`fm160d` 的 `timeout_ms` 下限 1 s）。
  ⇒ if2 一旦短暂失败，就会先白付 1 s 在死口 if3 上，才轮到**真正可用的 if1**。
* **已实施（2026-09-18）**：候选序改为 **`if2 → if1 → if3`**，
  iface 1 提升到第二位；**完全静默的 iface 0 被扣留**（`CAND_SILENT`），
  只有在「别的候选一个都没有」时才作为最后手段使用 —— 保留这条兜底是因为
  丢掉真正的 AT 口会让 daemon 根本起不来，比多花一次超时严重得多，
  而且未来的 USB 模式有可能把 AT 挪回 iface 0。
* 实测新日志（重编部署后）：
  ```
  AT candidates: 4 Fibocom (2cb7:*), 0 unclassified, 0 foreign, 1 held back as silent
    | probe order: /dev/ttyUSB2(if2) /dev/ttyUSB1(if1) /dev/ttyUSB3(if3)
  AT port is /dev/ttyUSB2          <- 启动后 1 秒内
  ```
  `if0` 不再出现在探测序里，`1 held back as silent` 明确报出扣留了一个。
* 判据补充：`partial_response` 只在 `status: timeout` 时出现，
  正好用来区分「完全静默」（if0）与「有回显、无解析」（if3）。

### 8.8 一个容易误判的点：只改 `files/` 时构建指纹看不出变化

`06-rebuild-packages.sh` 的指纹是 `*.c`/`*.h` 的内容 md5。改 `files/` 下的脚本
（例如 `files/etc/uci-defaults/99-fm160`）时，**指纹不会变**，脚本仍报
`build <same> -> <same>`；但 `Build/Prepare` 的文件依赖**包含 `./files`**，
所以编译确实会跑、新内容也确实进了 `.ipk`。
⇒ 判据要成对看：**指纹管 `.c/.h` 是否重编，`.ipk` 的 sha256 管打包内容是否变**。
本次两轮就是活例子：候选序那次**载荷二进制**从 `4778c4d9…` 变成 `6053977f…`（仍是 66377 B，
**字节数不变、只有内容变**）；而 99-fm160 那次**载荷二进制没变**、只有 `.ipk` 的 sha 变了。

复现脚本：`_probe/10-at-facts.sh`（已用正确单位；**故意跳过 ttyUSB2**，
因为那是 `fm160d` 正在轮询的口）。`_probe/09-tty-truth.sh` 只做只读取证，不发任何 AT。

## 9. M4 真机事实（锁频 / 锁小区 / CA，2026-09-18 验收）

本节每一条都来自真机（H69K + FM160-CN，无 SIM，`/dev/ttyUSB2`），
且**只发 `?` 与 `=?`**：`AT+GTACT=` 与 `AT+GTCELLLOCK=` 都是
**Persistent = Yes**，会写进模组自己的 EFS，所以到本节为止从未在真机上执行过。

### 9.1 四条原始响应（逐字）

```
AT+GTACT?
+GTACT: 20,6,3,1,8,101,103,105,108,134,138,139,140,141,501,5028,5041,5078,5079

AT+GTACT=?
+GTACT: (1,2,4,10,14,16,17,20),(2,3,6),(2,3,6),( ),(1,8),
        (101,103,105,108,134,138,139,140,141),( ),( ),(501,5028,5041,5078,5079)

AT+GTCELLLOCK?
+GTCELLLOCK: 0

AT+GTCELLLOCK=?
+GTCELLLOCK: (0,1,2),(0-2),(0,1),(0-4294967295),(0-1007),(0-1),(501-50261)

AT+GTCAINFO?
OK
```

### 9.2 ★ 判据：`AT+GTACT?` 的前三个字段**不是频段**

19 个 token 里前 3 个是 `rat,pref1,pref2`（与 `AT+GTRAT?` 的 `20,6,3` 完全一致），
后面 16 个才是频段 —— 而 fm160d 读出 `rat=20 pref=6,3 bands umts=2 lte=9 nr=5`
（2+9+5 = 16 ✓）。若把 19 个都当频段，UMTS 会凭空多出 `20,6,3` 三个不存在的段。

**频段是「一条扁平混合列表」，没有分隔**，只能靠编码自身反推 RAT：

| 编码区间 | RAT | 例子 |
|---|---|---|
| `1..25` | UMTS（band = raw） | `1`→U1、`8`→U8 |
| `101..499` | LTE（band = raw − 100） | `141`→B41 |
| `501..509` / `5010..5099` / `50100..50999` | NR（−500 / −5000 / −50000） | `501`→n1、`5028`→n28、`5078`→n78 |

真机解码结果与 `=?` 自述**互相印证**：state 的 UMTS 集合 `{1,8}` 恰好等于
caps 的 UMTS 组 `(1,8)`。⇒ 解析器错一位就会立刻与 caps 矛盾，这是可用的自检。

### 9.3 ★ `AT+GTACT=?` 只回**受限频段表**，且 gsm/cdma/evdo **是空组**

九组顺序固定：`rat,pref1,pref2,gsm,umts,lte,cdma,evdo,nr`。
真机 LTE 只报 9 个（`101,103,105,108,134,138,139,140,141`），NR 只报 5 个
（`501,5028,5041,5078,5079`），而手册给的是全集 —— **手册的表不能当真机能力用**。
`gsm`/`cdma`/`evdo` 三组是 `( )`（括号内只有一个空格）。

⇒ `fm160d` 把每组的「是否非空」单独发布（`group_nonempty`，真机
`[true,true,true,false,true,true,false,false,true]`），UI 灰掉而不是照抄手册。

### 9.4 ★★ `AT+GTCELLLOCK=?` 报 `(0,1,2)`，而手册只定义 0 / 1

```
GTCELLLOCK caps: mode=3 (includes an undocumented value!) rat=0-2 type=2
                 earfcn_max=4294967295 pci<=1007 scs=0-1 nrband=501-50261
```

mode 2 是**未文档化值**。「设备支持」和「我们可以写」是两个问题：
`fm160_celllock_command()` 只接受 0/1，mode 2 只作为证据记录在
`celllock_caps.mode_undocumented` 里，UI 明说「记录但永不使用」。
同理 `earfcn` 上限是 `0-4294967295`，但 `nrband` 只到 `50261`，
与手册写的 `50512` **不一致** —— 以真机为准，范围校验用 `=?` 报的值。

### 9.5 ★★ `AT+GTCAINFO?` 无 CA 时回**裸 `OK`**（没有 `+GTCAINFO:` 头）

无 SIM / 无数据连接时就是这样。⇒「解析不到」是**正常态**，
必须清空状态而不是报错；否则 UI 会继续展示去注册之前的陈旧 CA 表。
`m4.ca.valid=false` + 空 `scc[]` 就是我们发布的这种状态。

### 9.6 ★ `GTCAINFO` 的 `<freq>` 是十进制，`GTCCINFO` 的同名列 `<earfcn>` 是十六进制

手册给 GTCCINFO 的范围带 `0x`（`0-0xFFFFFFF`），给 GTCAINFO 的 `0-65535` / `0-2229167`
**不带** —— 同一列两种进制。`ca_parse_row()` 按十进制解，`fm160_parse_celllock()`
对 `earfcn` 走 `strtoull(..., NULL, 0)`（认 `0x` 前缀）。

### 9.7 ★★★ blobmsg 把 u32 当**有符号** int32 输出 ⇒ `4294967295` 变成 `-1`

`blobmsg_add_u32()` 存的是 `BLOBMSG_TYPE_INT32`，`blobmsg_format_json()` 用
`%d` 打印 `(int32_t)`。真机实测：

```
"earfcn_max": -1          <-- 实际是 4294967295，cell lock 的 earfcn 上限
```

三个后果，全部要处理：

1. 凡可能 > `INT32_MAX` 的字段**必须用 `blobmsg_add_u64()`**。
   已改：`celllock_caps.earfcn_max`。
2. `age_ms` 这类**时长**也是无符号的，同样改 u64（否则开机 24.8 天后变负数）。
3. 前端 `api.js` 的 `reported()` 用**量级**判「未上报」（daemon 的 `NONE` 哨兵
   `-1000000` 经 uint32 变成 `4293967296`，见 §8 旧账）。但 `-1` 的量级很小，
   `reported(-1)` 会**通过** —— 所以 `has_pci` / `has_scs` / `has_nrband`
   必须和值分开判断，不能靠量级。

**判据（可复现）**：`ubus call fm160 status | sed -n '/celllock_caps/,/}/p'`，
`earfcn_max` 必须是 `4294967295`，不是 `-1`。

### 9.8 ★★★ `blobmsg_open_table()` 的句柄**不是** `blob_buf` —— 这是本轮 139 的根因

`blobmsg_open_table(&b, name)` 返回一个**句柄**，它唯一的用途是交回
`blobmsg_close_table(&b, handle)`；**往里写数据仍然用 `&b`**，因为 libubox 在
`struct blob_buf` 里跟踪当前 head。

把句柄当 `blob_buf` 用（`blobmsg_add_u32(&t, ...)`）**能编译通过、零警告**，
然后在运行期把 `void *` 当 `blob_buf` 解引用。真机现象：

```
procd: Instance fm160d::instance1 s in a crash loop 6 crashes, 1 seconds since last crash
ubus call service list '{"name":"fm160d"}'   ->   "exit_code": 139
```

崩溃点固定在**身份链走完那一刻**：`ident_next()` 末尾 `fm160_state_publish()`
→ `fm160_state_blob()` → M4 段第一句。⇒ 与 AT 无关、与端口无关，
纯粹是发布路径。修复：M4 段 75 处调用全部改回 `&b`（句柄只用于 close），
并在 `state.c` 该段开头写明这条约定。

### 9.9 M4 的测试手段（都可复现）

| 手段 | 脚本 | 覆盖什么 |
|---|---|---|
| 宿主侧命令构造器单测 | `_tools/istoreos-h69k/19-m4-builder-test.sh` | **从 `cmds.c` 按名+花括号匹配抽取**两个构造器**原样**编译（不复制，故不会与实现漂移），31+ 例：`AT+GTACT=,,,103,5078` 形状、`103,,5`/`,103`/`103,`/`10 3`/`103\n`/`+103`/超长全部拒绝、mode 2/3/−1 拒绝、`pci < 0` 整段省略（不能填 0，0 是合法 PCI）、`scs`/`nrband` 各自可缺、`earfcn=4294967295` 通过 |
| 真机只读验收 | `_tools/istoreos-h69k/15-verify-device.sh` 第 9 节 | caps 是否枚举成功（=写路径的许可）、九组顺序、未文档化 mode 的记录、`earfcn_max` 是否穿过 wire、**日志里绝不能出现 `band lock write`**，以及**不能出现 cell lock 的「锁定」写入**（禁用的 `AT+GTCELLLOCK=0` 单独计数，见 §9.15） |
| 前端冒烟 | `_tmp/jscheck/m4check.js` | `api.js` 的 M4 访问器（哨兵两种编码、`has_*`、`bandCsv` 去重与顺序、`setcelllock` 的 7 个参数名与 daemon 的 blobmsg policy 逐字对应） |
| 写前干跑（不碰模组） | `_tools/istoreos-h69k/20-m4-dryrun.sh` | 用**同一个抽取机制**把两个构造器编成一个打印器，问「这次写入到底会往线上发什么」。见 §9.10 |

`19-m4-builder-test.sh` 的一个副作用值得记：真机模式下 `earfcn` 可以到
`4294967295`，所以单测里必须有这条用例 —— 否则 `unsigned long long` 换成 `int`
这种改动不会被任何编译警告发现。

### 9.10 写前干跑：`20-m4-dryrun.sh`

两次持久写是整包里唯一「一次点击 → 一次 EFS 写入」的地方，所以在按下之前要能回答
「到底要打给模组什么」。`20-m4-dryrun.sh` 用与 `19-` 相同的方式从**真正编出该
`.ipk` 的那棵树**里抽取构造器并运行，打印结果就是 `at-daemon` 会发的字节：

```
$ sh 20-m4-dryrun.sh bands "1,8,101,103,105,108,134,138,139,140,141,501,5028,5041,5078,5079"
AT+GTACT=,,,1,8,101,103,105,108,134,138,139,140,141,501,5028,5041,5078,5079
len=75  (buffer 256)

$ sh 20-m4-dryrun.sh celllock 1 1 0 632448 123 -1 -1
AT+GTCELLLOCK=1,1,0,632448,123

$ sh 20-m4-dryrun.sh bands "103,"        # 每个都返回 -1，绝不构造
REJECTED by the builder (returned -1)
```

它顺便是字段错位 bug 的免费检查：两个构造器的全部职责就是把**可选参数的嵌套**
摆对（`pci<0` 截断整段、`rat!=1` 扔掉 `scs/nrband`、`scs>=0` 与 `nrband>=0`
各自独立），这里看错一眼就能发现，不用等它被写进 flash。

手算一遍也要能对上，`:,,,` 是「保留 `rat`/`pref1`/`pref2` 不动，只换频段表」——
`setbands` 因此不会碰到 `rat=20 pref=6,3`。≤ 25 是 UMTS、`101..499` 是 LTE、
`50xxx` 是 NR，所以那条 75 字节的命令正好还原真机读到的 2+9+5 个频段。

### 9.11 干跑输出与真机现状的对照

真机 `AT+GTACT?`（2026-09-18 23:16）读到的集合与 `AT+GTACT=?` 报出的**能力集完全
相同**：

| | UMTS | LTE | NR |
|---|---|---|---|
| 当前生效 | 1, 8 | 1,3,5,8,34,38,39,40,41 | 1,28,41,78,79 |
| `=?` 能力 | 1, 8 | 1,3,5,8,34,38,39,40,41 | 1,28,41,78,79 |

⇒ 模组当前**没有被限段**，它开着每一个自己支持的频段。这一点有两个后果：

1. UI 上「全选 → 写入」是**内容恒等的写**：EFS 里那张表不变，变的只是「重新
   写了一遍」这件事本身。这使它成为验证写路径最安全的实验。
2. 反过来，任何一条**真正**的频段限制从此都能被认出来 —— 判据就是写完之后
   `AT+GTACT?` 是否比 `=?` 少。没有这张对照表，少一个频段到底是「写成功」还是
   「模组本来就不支持」是分不清的。

### 9.12 部署安全性（先部署、只观察、写前先干跑）

M4 新增的全部后台动作都是**读**：`AT+GTACT?`（idle 600 s / 前台 15 s）、
`AT+GTCELLLOCK?`（同）、`AT+GTCAINFO?`（**仅前台**），加上开机身份链末尾的
两条 `=?`。两个写方法（`setbands` / `setcelllock`）**只能由显式 ubus 调用触发**，
且各有一道闸：caps 未枚举成功 → 直接拒绝；`m4_writing` 串行锁（手册禁止
GTACT/GTRAT/COPS/GTCELLLOCK 混用）；写后回读，只有回读结果被当作新状态；
**永不重启 UE**。所以「先部署、只观察」是安全的。

写下去之前的那一步在 §9.10：先用 `20-m4-dryrun.sh` 让**构造器自己**说出要发的
字节，再决定发不发。三件事合起来是「先部署 → 只观察 → 写前干跑 → 再按
§9.14 两步走」。

### 9.13 已知遗留

* `AT+GTACT?` 里 `auto_seen`（频段值为 `0` = 自动选频）真机**未出现**，`0` 的语义
  按手册实现、未实测。**注意不要把 `auto_seen=false` 读成「模组被限段了」**：
  §9.11 的对照表显示当前频段集与 `=?` 的能力集一字不差，所以模组其实是**全开**
  的，只是用显式列表表达而不是用 `0` 表达。这两种写法的差别是「有没有被限段」
  这个判断里最容易搞错的一处。
* `AT+GTCELLLOCK=`（写路径）**从未以「锁定」形式写入**，且当前**没有可写的
  东西**：cell lock 需要 UE 复位才生效，而 FM160 没有传统复位模式；而且无
  SIM ⇒ 无 serving cell ⇒ 没有真实目标可锁，写进去只能是猜的 EARFCN/PCI。
  ⚠️ 但 2026-09-19 有过一次**意外**：为探测参数解析而发出的
  `setcelllock '{"mode":0,...}'` 真的走到了写路径（§9.15）。发出的内容是
  `AT+GTCELLLOCK=0`，即**关闭**，而模组本来就是 disabled ⇒ 恒等回写，回读
  状态逐字段未变、无 UE 复位、AT 健康无变化。**禁用形式的写入不构成「锁
  小区」**，验收脚本据此把两者分开计数（`cell lock writes=N (of which
  locking: 0)`）。
* band lock 写路径**已真机验通**，见 §9.14。
* `AT+GTCAINFO?` 的**非空**形状（PCC/SCC 多行）尚未实测（无 SIM、无数据连接）。
  解析器按手册 + `_probe/out-05.json` 的存档实现，`ca.valid` 为假时不展示。

### 9.14 ★★ band lock 写路径真机验通（2026-09-18 23:35–23:37，两步走）

写路径的第一次真机动作，按「先拒绝探测、再恒等回写」两步做，每步各有独立判据。

**判据一：负向探测（`AT+GTACT=,,,999999`）**

```
$ ubus call fm160 setbands '{"bands":"999999"}'
{ "status": "error",
  "command": "AT+GTACT=,,,999999",
  "response": "\r\nERROR\r\n" }
```
* 模组回 **裸 `ERROR`**（**不是** `+CME ERROR`）。这是新事实 —— 越界频段走的是
  普通 ERROR 通道。⇒ 解析器**不能**只认 `+CME ERROR`。
* 写入前后的频段快照 `IDENTICAL` ⇒ **模组在校验阶段就拒了，没有落 EFS**。
* `quiet` 回到 `false` 且无 `quiet_reason` ⇒ `m4_write_cb` 失败分支的
  `atq_clear_quiet()` 真的跑了（否则会白静默 8 s）。
* 日志里**只有**一行 `band lock write: AT+GTACT=,,,999999`，
  **没有**配对的回读 ⇒ 失败分支正确地跳过了回读（不存在的写不需要回读）。

**判据二：恒等回写（把当前那张表原样写回）**

```
$ ubus call fm160 setbands '{"bands":"1,8,101,103,105,108,134,138,139,140,141,501,5028,5041,5078,5079"}'
{ "status": "ok",
  "command": "AT+GTACT?",                       ← 注意：报的是回读，不是写入
  "response": "\r\n+GTACT: 20,6,3,1,8,101,103,105,108,134,138,139,140,141,501,5028,5041,5078,5079\r\n\r\nOK\r\n" }
```
* 回复里的 `command` 是 **`AT+GTACT?`** 而不是写命令 —— 这正是设计意图：
  **回读结果才是被采纳的新状态**，所以 ubus 报的是回读。要看到写命令本身，
  去日志里看（见下）。任何「`command` 应该是写命令」的断言都是错的。
* 回读值与写入值**逐字节相同**，且 `rat=20 pref1=6 pref2=3` 一并保持原样。
  ⇒ **空字段（`AT+GTACT=,,,`）的语义被实测确认是「保留当前值」，不是「复位默认」。**
  这是此前唯一没法从手册确定、也无法用我们自己的构造器回滚的一项
  （构造器只会发 `,,,`，不会发 `rat,pref1,pref2`），现在这条风险消掉了。
* 日志里写入与回读**同一秒**成对出现：
  ```
  23:36:40 daemon.warn fm160d[19037]: band lock write: AT+GTACT=,,,1,8,101,…,5079
  23:36:40 daemon.info fm160d[19037]: GTACT: rat=20 pref=6,3 bands umts=2 lte=9 nr=5
  ```
  ⇒ 回读被提交、返回、并重新解析成功。
* 写入前后状态快照 `IDENTICAL`；`AT+GTCELLLOCK?` 完全未动（`enabled=false`）。

**判据三：写完之后 AT 面仍然健康**

| 项 | 值 |
|---|---|
| `fm160d` PID | **19037 不变**（23:12:15 启动，跨两次写入 **25+ 分钟**无重启） |
| 连发 3 次实时 `AT+GTACT?` | 3 次都回完整表 ⇒ AT 面完全可用 |
| `consec_timeout` / `queue_depth` | `0` / `0` |
| `worst_response_ms` | **40 → 40**（没有变差） |
| 60 s 浸泡 | PID 不变、计数器不变、无新崩溃行 |

⚠️ 读日志别被计数带偏：`logread | grep -c "crash loop"` 会命中 **23:01** 那条
（修 139 之前的历史记录）。判「有没有新崩」要把时间戳和**当前 PID 的启动时刻**
对起来看，只看条数会得出相反结论。

### 9.15 一次意外的 cell lock 禁用回写，和它带来的断言收紧（2026-09-19 10:29）

**发生了什么。** 为了验证 §12.12 那处 `blobmsg` 类型修复（INT64 策略会拒绝所有
小整数），顺手用 `setcelllock` 做了两种数值形状的对照。这两条命令**不只是
解析测试** —— 它们真的到达了写路径：

```
daemon.warn fm160d[16668]: cell lock write: AT+GTCELLLOCK=0
daemon.info fm160d[16668]: celllock: disabled rat=-1 type=-1 earfcn=0 pci=0
```

**实际影响：零。** 四条判据：

| 判据 | 观测 |
|---|---|
| 发出的命令 | `AT+GTCELLLOCK=0` = **关闭**（`mode=0`），不是锁定 |
| 写前状态 | `celllock: disabled rat=-1 type=-1 earfcn=0 pci=0` |
| 写后状态 | 同上，**逐字段一致**（`enabled:false`、`earfcn:0`、`pci:0`） |
| UE 复位 | 无（日志无 reset 相关行） |
| AT 健康 | `at_state=0`，`worst_response_ms` 仍 < 50 ms |

⇒ 这是一次**恒等回写**，与 §9.14 里 band lock 用的手法同源，只是这次并非
刻意安排。

**验收脚本因此改了一处，而且是收紧不是放松。** 原本的断言是「日志里不能出现
`cell lock write`」，现在把计数拆成两组：

```
cell lock writes=2 (of which locking: 0)
```

匹配 `AT+GTCELLLOCK=0[[:space:]]*$` 的行算**禁用**，其余算**锁定**。理由：这条
断言要防的是「在未授权的情况下把 UE 钉在某个小区上」（需要复位、可能掉线），
而「关闭锁定」既不钉住任何东西、又能立即回读证实状态未变。把两者混在一个数
里，会让一次无害的禁用动作把整个验收跑成 FAIL —— 而 FAIL 里混进假警报，代价
是有人开始放宽断言，那才是真正的损失。

**教训。** `ubus call` 打到一个写方法上，**没有「只测参数解析」这回事**。要测
参数形状，用 `_tools/istoreos-h69k/20-m4-dryrun.sh` 的干跑（把构造器源码编成
打印器），它一条命令都不发。


---

## 10. M5 GNSS 真机事实（2026-09-19 06:36–06:41，一次开机/关机往返）

本节全部来自设备实测。**§6 是手册派生的事实；两者冲突处以本节为准。**

### 10.0 结论：开 GNSS 对模块连接性的影响 = 实测 0

先给出用户明确要求的那个判据，因为它决定了后面所有实验能不能做。

| 判据 | 开机前 | `GTGPSPOWER=1` 之后 | 结论 |
|---|---|---|---|
| `idVendor:idProduct` | `2cb7:0104` | `2cb7:0104` | 未变 |
| `bNumConfigurations` | 1 | 1 | 未变 |
| `bConfigurationValue` | 1 | 1 | 未变 |
| `bNumInterfaces` | 5 | 5 | 未变 |
| 接口集合 | `2-1:1.0 … 1.4` | `2-1:1.0 … 1.4` | 未变 |
| `/dev/ttyUSB*` | 0,1,2,3 | 0,1,2,3 | 未变 |
| at-daemon 持有的口 | `/dev/ttyUSB2` | `/dev/ttyUSB2` | 未变 |
| `event_drop_count` | 0 | 0 | 无丢事件 |
| AT 面（`AT+CGMI`） | 应答 | 应答 | 通 |
| 内核日志里的 USB 事件 | — | **无重枚举、无 new device、无 disconnect** | — |
| `fm160d` PID | 19037 | **19037**（全程未变） | 未重启 |

关机后再次比对，与开机前**逐项相同**。整个往返（开机 → 5 次 NMEA 读 → 关机 →
再开机 → 计时 → 关机）结束时 `logread | grep -cE 'signal 11|segfault|crash loop|exit_code'`
= **0**。

**为什么这条能提前成立**（三条独立证据，任一单独都不够）：

1. **手册特性表**（GNSS 指南 §1.1）给出的是唯一关键组合：
   `Require Restart to Take Effect = No`（**不重启 UE**）+
   `Require Data Store at Power Down = No`（**不写 EFS，掉电即回 0**）+
   不需要 SIM / 注网 / 数据连接 + 同步命令（≤500 ms）。
   能伤到连接性的两件事 —— 重启、持久改配置 —— 手册都明确排除。
2. **本 USB 模式没有可供新增的接口**：设备是单一配置（`bNumConfigurations=1`）
   且 5 个接口已全部绑定驱动。要加第 6 个口必须换配置 ⇒ 必然重枚举。
   而 mode 17/32 的接口表里根本没有 GNSS 口（见 §10.2）。
3. **当前无 SIM ⇒ 本来就没有「连接」可掉。** 这条最朴素但最有力。

⚠️ 严格讲，「100%」对未文档化的固件行为无法先验证明。上面是
「文档排除 + 结构上不可能 + 实测不变」三重证据，再加上一条即时回退路径
（`AT+GTGPSPOWER=0`）和一条自愈路径（掉电后自动回 0）。据此判断风险可忽略。

### 10.1 ★★ NMEA 从 **AT 口**出；本 USB 模式下**不存在** GNSS 口

最重要的一条，因为它直接决定 M5 的架构：**没有端口可抢，只能轮询 AT。**

USB 模式表（`_ref/fm160/docs/USB-MODES.txt`，源自拨号集成指导「表 1」）里
`AP(GNSS)` 接口**只出现在 mode 40/41**，而本机是 **mode 17/32**（`0x0104`）：

```
17/32  0x0104   DIAG | Modem | AT | Pipe | RmNet
40     0x7126   RNDIS | … | AT | AP(GNSS) | META | DEBUG | NPT | ADB
41     0x7127   RNDIS | … | AT | AP(GNSS) | … | AP(LOG) | AP(META)
```

实测映射（`readlink -f /sys/class/tty/ttyUSBn/device`）与之逐项吻合：

| tty | 接口 | 角色 | 驱动 | 端点 |
|---|---|---|---|---|
| `ttyUSB0` | `2-1:1.0` | DIAG | `option` | 2 |
| `ttyUSB1` | `2-1:1.1` | Modem | `option` | 3 |
| **`ttyUSB2`** | `2-1:1.2` | **AT** | `option` | 3 |
| `ttyUSB3` | `2-1:1.3` | Pipe | `option` | 3 |
| — | `2-1:1.4` | RmNet | `qmi_wwan` | 3 |

⇒ `ttyUSB2` 就是 `AT Device Application Interface`，与 at-daemon 持有的口一致。
**5 个口里没有任何一个是 GNSS 口。**

而 GNSS 指南自己的例子就是 `AT+GTGPS?` 直接回 NMEA 段落：NMEA 是**命令响应**，
不是独立口上的 URC 流。

**架构结论：M5 不需要 open 任何新口、不需要 lease、不需要监听 URC。**
它走的是已经跑了几万行的 `ttyUSB2` 通道 —— 对「稳定性优先」这个要求来说，
这是最好的结果。

### 10.2 NMEA 原始响应（逐字，引擎已开、**无定位**）

```
\r\n+GTGPS: \r\n
$GPGSA,A,1,,,,,,,,,,,,,,,,*32\r\n
$BDGSA,A,1,,,,,,,,,,,,,,,,*23\r\n
$GAGSA,A,1,,,,,,,,,,,,,,,,*23\r\n
$PQGSA,A,1,,,,,,,,,,,,,,,,*24\r\n
$GPGSV,1,1,0,1*54\r\n
$BDGSV,1,1,0,1*45\r\n
$GLGSV,1,1,0,1*48\r\n
$GAGSV,1,1,0,7*43\r\n
\r\nOK\r\n
```

三点必须注意：

1. **`+GTGPS:` 后面直接换行**，语句在后续行 —— 解析器不能假设值在同一行。
2. **只出现 GSA 和 GSV，没有 RMC / GGA。** `GTGPSCFG` 的 x=2 真机值是 14
   （手册说五星座全开、NMEA 应含 RMC/GGA），但**无定位时带位置与时间的句子
   被整段省掉**。⇒ 判「NMEA 正常」**不能**要求 RMC 存在。
3. **无卫星的 GSV 仍带一个尾随字段**：`$GPGSV,1,1,0,1*54`。
   按 NMEA 4.10+ 的 `signalId` 约定，GPS/BeiDou/GLONASS 的 L1 记 `1`、
   Galileo E1 记 `7` —— 这与 `$GAGSV,1,1,0,7*43` 自洽，所以该字段**最可能是
   signalId 而不是星座编号**。⇒ 解析器必须**按字段数容错**（标准 GSV 在
   「可见 0 颗」时无尾随字段），并且**校验和要真算**，不能靠形状判断。

`$GPGSA,A,1,…` 的 `1` = **fix not available**；`$GPGSV,1,1,0` 的 `0` = 可见 0 颗。

### 10.3 ★ 这台机器**有** GNSS 天线，但室内看不到卫星（结论已更正）

> **更正记录（2026-09-19）**：本节原先写的是「本机未接 GNSS 天线」。用户指出
> FM160 **确实接了天线，只是只有四根**（与蜂窝 MIMO 共用一套），且设备在**室内**。
> 「天线未接」这个判断因此**不成立**，本节据此改写。归因从「射频输入不存在」
> 退回到「**归因未定**」。

**实测事实（未变）**：跨 **100 秒**五次采样
（`06:38:12 / 06:38:32 / 06:38:52 / 06:39:12 / 06:39:32`），响应**逐字节完全
相同**，含全部八个校验和，可见卫星恒为 **0**。

**改了的是对它的解释**。三个候选原因，都还没有被排除：

| 候选 | 为什么可能 | 怎么证伪 |
|---|---|---|
| 室内遮挡 | 混凝土/镀膜玻璃对 GPS L1 (1575.42 MHz) 的衰减足以让可见星数为 0 | 搬到室外复测同一序列 |
| 天线频段不匹配 | 四根天线同时承担 LTE/NR MIMO 与 GNSS 接收；蜂窝天线并不自然覆盖 L1，需要专门的 GNSS 天线或合路器 | 换一根确认可用的 GNSS 天线复测 |
| 射频通路确实不通 | 载板 GNSS 馈电/连接器问题 | 同上，以及量馈电 |

⇒ 在分清这三者之前，**不能断言射频通路坏了，也不能断言它好**。任何「模块坏 /
天线没接」的说法都是过度解读。

**三个后果（与归因无关，仍然成立）**：

* **M5 的「有定位」路径在本机无法联调。** 解析器只能按手册 + 本节的无定位帧形态
  实现，必须**明确标注未经实测**。因此「有定位」这条链由一支主机侧测试覆盖：
  把**用独立实现算出的校验和**喂给**真实的 `cmds.c`**（见 §11.7）。这样它至少
  被执行过一次，而不是靠祈祷。
* UI 必须能诚实地显示「引擎已开 / 可见 0 颗 / 无定位」，而**不能**把它当成错误
  —— 在室内这会是最常见的真实状态。
* UI 的文案要说「**室内 0 颗属正常**」，**不能**说「未接天线」——那是一条已经
  被推翻的结论，写进界面就等于把错误固定下来。

### 10.4 四处语法与行为细节

| 现象 | 原始响应 | 结论 |
|---|---|---|
| `<item>` **不加**引号 | `AT+GTGPS=RMC` → `\r\nERROR\r\n` | **必须带引号** |
| `<item>` **带**引号 | `AT+GTGPS="RMC"` → `\r\n+GTGPS: \r\n\r\nOK\r\n` | 接受；无定位时载荷为空 |
| 引擎**关**着时读 | `AT+GTGPS?` → `\r\nERROR\r\n` | **必须先开机再读** —— UI 的顺序约束 |
| 开机后**第一次**读 | 原始捕获 88 字节（只有 `+GTGPS: ` 头，无语句） | **空帧不是错误**，引擎还要一点时间出句子 |
| 开机后第二次起 | 原始捕获 304 字节（8 条语句） | 稳定形态 |

> ⚠️ **88 / 304 的口径未记录，不要用来反推 daemon 侧的数字。** 这两个数是
> **原始串口捕获**长度，无法判断是否含命令回显与 at-daemon 的封装。`fm160d`
> 只拿到 at-daemon 的 `response` 字段（`atq.c`：`resp = rep.response`），按
> 响应体算是 **18 B**（`\r\n+GTGPS: \r\n\r\nOK\r\n`）与 **220 B**
> （12 头 + 184 语句 + 16 换行 + 8 尾）。⇒ daemon 只报告 `strlen(response)`，
> 不试图复现 88/304；判「空帧」只看**有没有语句**，**不**看字节数。

⇒ `fm160d` 的构造器发 `AT+GTGPS=<item>` 时**必须自己补双引号**；
daemon 侧「空载荷」要走**与 ERROR 不同**的分支（空 = 还没出句子，ERROR = 没开机）。

### 10.5 成本（决定 M5 的轮询节奏）

计时用 `/proc/uptime`（厘秒），因为这台 busybox 的 `date` 没有 `%N`、也没有 `awk`。

| 调用 | 耗时 |
|---|---|
| `AT+GTGPSPOWER=1` | **50 ms** |
| `AT+GTGPS?`（稳态） | **30 ms** |
| `AT+GTGPS?`（开机后第一次） | 30 ms，但载荷为空 |

`AT+GTGPS?` 只要 **30 ms** —— 比 M4 已有的若干个轮询还便宜。⇒ 前景 2–5 s 一次
完全负担得起；后台 idle 可以直接进 600 s 档（与 `gtact` 同级），或者干脆
**只在 GNSS 开着时**才轮询（推荐：关着时 `GTGPS?` 只会回 ERROR，纯浪费）。

`worst_response_ms` 全程停在 **117**，未被 GNSS 拉动。

### 10.6 `AT+GTGPSCFG?` 真机值（与手册的差异）

```
\r\n+GTGPSCFG: \r\n0,2\r\n2,14\r\n3,0\r\n\r\nOK\r\n
```

| x | 含义 | 真机值 | 手册 | 说明 |
|---|---|---|---|---|
| 0 | SUPL 版本 | **2** | 0=SUPL1.0 / 1=SUPL2.0 | ⚠️ **2 未文档化** |
| 1 | xtra 开关 | **整行缺失** | 0 关 / 1 开（默认开） | ⚠️ 不报这一行 |
| 2 | 卫星组合 | **14** | 14 = GPS+BDS+GAL+GLO+QZSS | 五星座全开 |
| 3 | SUPL 证书 | 0 | 0 关 / 1 开 | 与 `AT+GTGPSCERT?` 回裸 `OK`（无证书）自洽 |

⇒ 解析器必须**按 x 逐行匹配**，不能按行号或行数取；`x=1` 缺失是正常态。
⇒ 与 M4 的结论一致：**手册的表不能当真机能力用，真机回什么就是什么。**

⚠️ 顺带记下 GNSS 指南自身的两处排版错误：§1.5 与 §1.6 的「Query command」
栏都写成了 `+GTGPSPOWER: (list of supported <x>s)`（应为 `+GTGPSCFG:` /
`+GTGPSCERT:`）。⇒ **这份指南的「=? 响应头」不可引用**。

#### 10.6.1 ★★ `AT+GTGPSCFG=?` 首次实测（2026-09-19，M5 部署后）

M5 之前这条命令**从未在真机发过**，解析器按「布局未知」写。部署新 ipk 后
ident 链第一次读到它，应答逐字如下：

```
\r\n+GTGPSCFG: 0,(0-2)\r\n+GTGPSCFG: 2,(0-15)\r\n+GTGPSCFG: 3,(0,1)\r\n\r\nOK\r\n
```

| x | 取值集 | 个数 |
|---|---|---|
| 0 | `0-2` | 3 |
| 1 | **整行缺失** | 0 |
| 2 | `0-15` | 16 |
| 3 | `0,1` | 2 |

三点结论：

1. **`=?` 的行集合与 `?` 完全一致**（都是 x=0/2/3，都缺 x=1）⇒ 上一节
   「按 x 匹配、不能按行号取」对 `=?` 同样成立，不是 `?` 的特例。
2. ★★ **每个 x 各有自己的值集，且它们不是同一个集合** —— x=0 只允许 `0-2`，
   而 x=2 允许 `0-15`。**绝不能把三者的并集当作写门禁**：并集问的是「这个值
   对**任意**字段合法吗」，而 `AT+GTGPSCFG=2,<v>` 要问的是「对 **x=2** 合法
   吗」。本机并集恰好等于 x=2 的集合（`0-15` 是另两个的超集），所以**第一版
   实现结果正确、机制错误** —— 换一版固件让别的字段变成更宽的那个，它就会
   放行模组拒绝的值。M5 的解析器已改为**按 x 分槽**，写许可只取 x=2 那一组。
3. 手册 §4.6 的卫星组合表给 9 个值（0,2,3,4,5,6,7,14,15），模组自称 `0-15`
   **全允许** —— 模组比手册宽。双重门禁（手册集 ∩ 模组 x=2 集）因此仍收敛到
   9 个值：宁可漏掉手册未载的组合，也不往持久设置里写一个没人定义过的数。

同一轮确认（`AT+GTGPSEPO?` / `AT+GTAGPSSERV?`）：`epo=0`、
`server=supl.qxwz.com`、`port=7276`，与 §6 记载一致。
`AT+GTGPSCFG?` 复读仍是 `0,2 / 2,14 / 3,0`（x=1 仍缺失）⇒ **观测 `=?` 没有
改变模块的任何设置**。

### 10.7 复现步骤

所有批次都只走 `fm160d` 已有的 `at` 方法（不新开进程、不抢端口、不 lease）：

| 阶段 | 脚本 | 断言 |
|---|---|---|
| 开机前快照 | `_tmp/m5-pre.sh` | 契约 5 项 + tty→接口映射 + `GTGPSPOWER?`=0 |
| 开机 + 验契约 | `_tmp/m5-on.sh` | 契约五项逐项不变、AT 面仍通、日志无重枚举 |
| 抓 NMEA | `_tmp/m5-nmea.sh` | 三次采样 + 四个 item + 引号对照 |
| 成本 | `_tmp/m5-timing.sh` | 50 ms / 30 ms / 空帧 |
| 关机 + 回退 | `_tmp/m5-off.sh` | 契约回到基线、`GTGPS?` 回 ERROR |
| **回归守卫** | `_tools/istoreos-h69k/15-verify-device.sh` **第 10 节** | 把本节的全部不变式变成机器断言：引擎=0、`GTGPS?`=ERROR、`bNumConfigurations=1`、接口=5（**无 GNSS 口**）、`ttyUSB2`→`2-1:1.2`、`GTGPSCFG?` 仍然是 `0,2/2,14/3,0` |

每支都先 `sh -n` 过语法、`sha256sum` 双端比对后再执行。

**为什么第 10 节值得单独做成守卫**：它守的不是 GNSS 本身，而是**「NMEA 只能从 AT 口出」
这条架构假设**。M2 拨号很可能要把 USB 模式换成 ECM/MBIM（§1 的 mode 19/23/33），
一旦换了模式，`AP(GNSS)` 接口的有无、接口数、tty 编号都可能变。
**与其让一条过时结论悄悄流进 GNSS 代码，不如让它当场报错。**

### 10.8 已知遗留

* **无卫星可看**（§10.3；**归因未定**，不要再写成「没有天线」）⇒ 有定位形态的
  NMEA（含 RMC/GGA）**未经实测**，由 §11.7 的主机侧测试代偿。
* `AT+GTGPSEPO` / `AT+GTAGPSSERV` / `AT+GTGPSCERT` 的**写路径**未测
  （前两者 `Require Data Store at Power Down = Yes`）。AGPS 本来也需要数据连接，
  无 SIM 时无意义。
* `GTGPS` 的**实际更新率**未测：模块内部刷新频率未知，30 ms 只是「读一次」的
  成本，不代表 NMEA 多久更新一帧。
* TCP 转发（`PLAN.md` M5 里的「可选」）需要数据连接，无 SIM 时无法验证。

---

## 11. M5（GNSS）实现事实

> 本节记录的是**实现**层面的判据 —— §10 记的是模块怎么答，这里记的是
> `fm160d` 为什么这么写。**两者冲突时以 §10 为准**（模块的行为不会因为
> 我们的代码而改变）。
>
> 每条后面都标了**依据**：`[实测]` 来自真机，`[手册]` 来自 AT 手册，
> `[推断]` 是我们的选择，可被推翻。

### 11.1 NMEA 从 AT 口出，因此 M5 不开新口、不抢租约

`[实测]` USB mode 17/32 的接口集合里**没有 GNSS 接口**（§1 的 tty 映射：
`ttyUSB0→DIAG`、`1→Modem`、`2→AT`、`3→Pipe`、`1.4→RmNet`）。
`AT+GTGPS?` 把 NMEA 作为**普通命令响应**返回。

后果，三条都影响代码结构：

* M5 **不打开任何新端口**、不申请任何 lease、不等任何 URC。整条 GNSS 读取链
  就是 `atq_submit(AT_PRIO_POLL, "AT+GTGPS?", ...)`。
* 开 GNSS 前后 USB 描述符**逐项不变**（ID / 配置数 / 配置值 / 接口数 / 接口
  集合 / tty 集合 / at-daemon 持有口 / `event_drop_count`），**无重枚举**
  `[实测]`。⇒ 开 GNSS 对连接性的影响是 0，这一点是 M5 敢默认部署的前提。
* 因为不开新口，**换 USB 模式也不会改变 GNSS 的数据通路** —— 只要新模式下 AT
  口还在，§11 全部结论继续成立。

### 11.2 引擎关着时**一条命令都不发**

`[实测]` 引擎关闭时 `AT+GTGPS?` 回 `ERROR`（§10.4）。⇒ `fm160_cmd_poll_gnss()`
在 `engine.on == false` 时**直接 return**，不提交、不试探。

这不是省一条命令，而是**避免让 ERROR 污染 AT 健康度统计**：at-daemon 的
`at_state` 会因连续的 ERROR 降级甚至停轮询，用一个我们**已经知道会失败**的
命令去换这个风险是纯亏。

### 11.3 ★ 静默窗对「开引擎」是**负作用**（这是 M5 唯一反直觉的设计）

`atq.c` 的 `atq_submit()` 在 `prio == AT_PRIO_POLL && atq_quiet_active()` 时
**直接拒绝**提交。

所以：若给 `AT+GTGPSPOWER=1` 开静默窗，紧跟其后的 `AT+GTGPS?`（POLL 优先级）
会被**自己开的窗挡掉**，而「开引擎」这个动作的全部意义就是**尽快看到 NMEA**。

⇒ `cfg_write()` 的静默窗改成 `quiet_s > 0` 才开；GNSS 开关传
`QUIET_NONE, NULL, 0`，**一个窗都不开**。测试 H 段用
`eq("NO quiet window for the engine switch", quiet_calls, 0)` 把它钉死。

`AT+GTGPSPOWER=1` 实测 50 ms、`AT+GTGPS?` 稳态 30 ms `[实测]` ⇒ 不需要保护的
窗口，本来就短得没有交织空间。

### 11.4 空帧 ≠ 错误，而且**不推 `read_ms`**

`[实测]` 开机后第一次读会拿到「OK，但一条语句都没有」的空帧（§10.4）。

设计上是**两件事分开**（`struct fm160_gnss_reading` 分两组字段）：

* **画面**（`read_ms` / `sentences` / `nmea_bytes` / `resp_bytes` / `raw` /
  `sats` / 位置 ……）描述**最后一个真的有语句的块**，空帧**一个字段都不动**。
* **轮询结果**（`empty_frame` / `empty_frames` / `empty_ms` / `empty_bytes` /
  `read_error`）描述**最近一次尝试**。

**为什么 `read_ms` 必须不动**：它是 UI 里 `age_ms` 的来源。若空帧把它推到当前
时刻，十分钟前的卫星会显示成「刚刚更新」—— 一个把陈旧数据伪装成新鲜数据的
bug，而且它只在引擎开着但收不到句子时出现，最难被发现。

`empty_frames` 是**连续**空帧计数（真块到达即归零）：一两次是启动正常，
**持续增长**才是「引擎声称开着却从不说话」的真故障。

### 11.5 ★ DOP 三元组：**第一帧完整者胜，稀疏的后续帧不许擦除**

`[实测]` 多星座接收机会**每星座发一条 GSA**（实测 GPS / BeiDou / Galileo / QZSS
四条），顺序由模块固定，GPS 在前。

原实现无条件 `r->pdop_x10 = …; r->hdop_x10 = …; r->vdop_x10 = …;`，有两个问题：

1. 后一条 GSA（BeiDou）**覆盖**前一条（GPS）⇒ 发布的 PDOP/HDOP/VDOP 取决于
   「哪个星座恰好最后被发出」，是任意的；
2. **更糟**：一条**稀疏**的后续 GSA（列出 PRN 但三个 DOP 全空 —— 这正是实测
   无定位帧的形态）会把已有的好三元组**擦成三个哨兵**。

⇒ 改成：**只有当三个都非空、且当前还没有三元组时才写入**。三条 DOP 必须
**同进同出**，因为「用 GPS 的 PDOP 配 BeiDou 的 VDOP」描述的是一个不存在的解。

C2 段测试用一帧「GPS 有 DOP + BeiDou 稀疏」把它钉死。

### 11.6 校验和**先于**形状检查

`$GPGS*00` 这种句子既是「格式符太短」又是「校验和不符」。实现选择**先校验和**：

理由：`*hh` 不符意味着**这句话任何位置都可能被破坏，包括格式符**，所以连
「它是不是一句格式合法的句子」都不可信。可见后果是短格式符 + 坏校验和被计入
`bad_checksum` 而不是 `bad_shape` —— 这是**诚实的分类**，也因此 `bad_shape`
只能读作「**在校验和通过的前提下**，模块发出了结构错误的东西」。

测试里那条短格式符用例特意给了**正确**的 `*03`（`"GPGS"` 的 XOR），
以隔离出形状这条路径（`$GPGS*00` 测的是校验和，不是形状）。

### 11.7 ★ 主机侧测试：把独立算出的帧喂给**真实的 `cmds.c`**

`_tools/istoreos-h69k/21-gnss-parser-test.sh`。存在理由：**本机看不到卫星**
（§10.3），「有定位」这条链（GGA/RMC/卫星表/ddmm.mmmm 换算/GSA↔GSV 交叉引用）
若不测就**一次都不会被执行**。

它做的三件不偷懒的事：

1. **不抄代码**：`cmds.c` **原样**编进测试（`-I` 指向真源码），`atq.c` 的
   `fm160_resp_find()` 用 `awk` 按名抽取。改名/改原型 ⇒ **编译失败**，
   而不是悄悄测一份旧副本。
2. **校验和独立算**：有定位帧的 `*hh` 由一支短 Python 脚本算出，**不是**解析器
   自己算的 ⇒ 校验通过是**真校验**，不是自证。
3. **不碰模块**：AT 队列打桩，记录 `last_cmd / last_prio / last_cb`，
   手工 `drive()` 三步序列（写 → 读回 → follow-up），因此四条写序列的**命令
   顺序、优先级、静默窗开关**都可以断言。

**结果：239 checks / 0 failures。**（224 是 M5 首次落地时的数；§10.6.1 的
按字段分槽修好后又加了 15 条，主要是「x=0 收而 x=2 不收的值必须被拒」。）

它抓到的两个**真** bug（都不是笔误，是设计缺陷）：

| # | bug | 修法 |
|---|---|---|
| 1 | DOP 三元组被后续 GSA 覆盖 / 被稀疏帧擦除 | §11.5 |
| 2 | 空帧把 `read_ms` 推新 ⇒ 旧数据伪装成新鲜 | §11.4 |

同时暴露了**测试自己**的 3 处错误期望（写反了「最后一条 GSA 胜出」、把
`$GPGS*00` 当成形状错、`+GTACT?` 假成了两行）—— 记录在此，因为「测试写错」
和「代码写错」必须分得清。

⚠️ 而它**没抓到**的第三个 bug 最值得记：写许可的「并集 vs 分字段」（§10.6.1）。
**测试自己也复制了同一个巧合** —— 合成帧里恰好奇妙地让被写字段的那一组最宽，
于是「并集」实现照样全绿。⇒ 合成数据必须**故意让别的字段更宽**，并且断言要
落在**负向**（「只在别的字段里合法的值必须被拒」）上。教训：**巧合不是检查。**

### 11.8 写许可分层：一个无门禁，一个双重门禁

| 写 | 门禁 | 依据 |
|---|---|---|
| `AT+GTGPSPOWER=<0\|1>` | **无门禁** | `[实测]` 它是模块**唯一不存储**的 GNSS 设置（`Require Data Store at Power Down = No`）⇒ 不会残留、不会写坏、0 和 1 都实测过 |
| `AT+GTGPSCFG=2,<v>` | **双重**：手册文档集 ∩ 模块 `=?` 的 **x=2 集** | 持久化（掉电保存）= Yes ⇒ 写错**不会被复位抹掉** |

`=?` 已于 2026-09-19 实测（§10.6.1）。它**按字段分别给值集**，所以解析器按 x
分槽、写许可**只取 x=2 那一组**（第一版取并集：在本机结果对、机制错）。
拿不到 x=2 的集 ⇒ `caps_valid = false` ⇒ **拒绝写**并记一条机器可见的日志
（`refusing to write GNSS config: AT+GTGPSCFG=? gave no set for x=2`）。
没有 x 头的组落进 unkeyed 槽：值不丢、日志里看得见，但**不授权任何写入** ——
没有字段归属的值无从校验。

因为 `GTGPSPOWER` 不存储，**autostart 必须由 fm160d 每次 ident run 补发一次**
（`option gnss_autostart`，默认 0）。「一次 per ident run」而不是「一次 per
observe」是关键：按观察补发会**永远和用户的「关掉」点击打架**。

### 11.9 未实测项清单（M5）

* ~~`AT+GTGPSCFG=?` 的响应布局~~ —— **2026-09-19 已实测**（§10.6.1）。
* ~~引擎开着时的读路径（分词 / 校验和 / 星座分槽 / 无定位形态）~~
  —— **2026-09-19 已真机验证**（§11.11）：8 句 / 184 B / 5 talker / 0 校验错，
  与主机侧测试对同一帧的预测逐项吻合。
* ~~`GTGPSPOWER` 写路径（`setgnss`）~~ —— **2026-09-19 已真机验证**（§11.11），
  且以模组自己的回读 `+GTGPSPOWER: 0/1` 为准关回 0。
* `AT+GTGPSCFG=2,<v>` 的**写**仍未在真机发生：恒等回写 `AT+GTGPSCFG=2,14`
  待设备侧授权再做（门禁本身已打开，x=2 有 16 个值）。
* ★ 手册之外的组合（模组允许 `0-15` 里的 1、8–13）**语义未知**，被双重门禁
  挡在外面；要放开得先弄明白它们是什么。
* **有定位**形态的 NMEA **全链** —— 室内拿不到定位，仍由 §11.7 代偿，非真机。
* **空帧**分支仍未真机触发（§11.11 末尾说明了为什么它比原来记的更窄）。
* AGPS 写路径（`GTGPSEPO` / `GTAGPSSERV` / `GTGPSCERT`）未测，且需要数据连接。
* `GTGPS` 的**实际刷新率**未知（30 ms 是「读一次」的成本，不是帧间隔）。
* 「室内 0 星」的**归因**未定（§10.3）—— 天线在、可能频段不对、可能室内。

### 11.10 与 §10 的对应关系

| §10 的实测事实 | §11 的实现落点 |
|---|---|
| NMEA 从 AT 口出、无 GNSS 接口 | §11.1 不开新口、只走 POLL 队列 |
| 引擎关 ⇒ `GTGPS?` 回 `ERROR` | §11.2 关着时一条不发 |
| `<item>` 必须带引号 | 构造器自己补 `\"%s\"`（`fm160_gnss_item_command`） |
| 开机后第一次读是空帧 | §11.4 空帧不动画面、不推 `read_ms` |
| 无定位时只有 GSA/GSV | 位置/时间字段**全部可缺**，`has_position` 是唯一闸门 |
| `$GPGSV,1,1,0,1*54` 带尾随字段 | 按字段数容错 + 记录 `gsv_trailing[_value]` |
| `AT+GTGPSCFG?` 是 `0,2 / 2,14 / 3,0` | 按 x **逐行**匹配，未知 x 计入 `unknown` 而非错位 |
| 各调用的成本（50/30 ms） | §11.3 不需要静默窗 |
| 开 GNSS 对连接性影响 = 0 | 默认部署的前提（M5 可只读上线） |

### 11.11 ★★ 真机联调：读路径已跑过**真实帧**（2026-09-19）

只读验收全程引擎关着 —— 那是**该把模组留成的样子**，但对解析器是很差的测试：
`read.sentences` 自从新 daemon 装上就一直是 0，**分词、校验和、星座表一次都没
见过真机的字节**。主机侧测试覆盖的是形状，它证明不了「模组真正发的帧在其中」。

所以让引擎开了半分钟（`_tools/istoreos-h69k/22-gnss-live-test.sh`）。可开性的
四条理由都成立：`GTGPSPOWER` 是模块**唯一不存储**的 GNSS 设置 ⇒ 无残留；开它
对连接性的影响**已实测为 0**；NMEA 轮询层是**仅前台**的 ⇒ 窗口一关就不再读；
脚本结束前**必须**关回去，且**以模组自己的回读为准**（不是 daemon 的想法）。

真机回的帧与 §10.2 **逐字节一致**：

```
$GPGSA,A,1,,,,,,,,,,,,,,,,*32     $GPGSV,1,1,0,1*54
$BDGSA,A,1,,,,,,,,,,,,,,,,*23     $BDGSV,1,1,0,1*45
$GAGSA,A,1,,,,,,,,,,,,,,,,*23     $GLGSV,1,1,0,1*48
$PQGSA,A,1,,,,,,,,,,,,,,,,*24     $GAGSV,1,1,0,7*43
```

daemon 的计数器与主机侧测试对同一帧的预测**完全吻合**：

| 项 | 真机 | 主机侧测试预测 |
|---|---|---|
| 语句数 | **8** | 8 |
| NMEA 字节 | **184** | 184 |
| 整响字节 | **218** | 218 |
| talker 数 | **5** | 5（GP/BD/GA/PQ/GL） |
| 校验和失败 | **0** | 0 |
| `fix_type` | **1**（no fix） | 1 |
| `snr_best_db` | **哨兵**（无 GSV 信噪比） | 哨兵 |
| `visible_total` | **0**（模组自称 0 颗） | 0 |

⇒ **读路径（分词 / 校验和 / 星座分槽 / 无定位形态）已在真机验证，不再是
「仅主机侧代偿」。** 仍未真机覆盖的只剩：**有定位**形态（本机室内无定位）、
**空帧**分支（这次第一个采样周期内就拿到了完整帧）、以及**写**路径
（`setgnsscfg` 恒等回写尚未授权）。

⚠️ 顺带一个分辨率修正：本次**没有**观察到「开机后第一次读是空帧」。上一次
读发生在开机后约 5 s，接收机已经在出句子了。§10.4 的 88/304 是当时原始抓取的
口径，**不可换算**成 daemon 侧计数器（前者按响应体算是 18/209）—— 空帧分支
因此仍**只有合成帧覆盖**，§11.4 的理由（空 ≠ 错）成立，但它**的触发时机**比
原来记的更窄。

⚠️ 一支脚本**必须**在工作站上跑，不能拷到设备上跑：它们全都通过 `ssh_d()` 干活，
在设备上跑等于让设备用 dropbear 往自己 ssh，`-o` 选项和密钥路径全不认，
返回的东西看着像数据其实是空的。症状**不像**「跑错了主机」，像「setgnss 没应答」。

## 12. M3（SMS）实现事实

> 同 §11 的约定：`[实测]` 真机（§8）、`[手册]` AT 手册、`[源码]` 上游或本项目
> 源码、`[推断]` 我们的选择（可被推翻）。

### 12.1 ★ D4 的「`sendat` prompt 补丁」不需要 —— 缺的是调用序列

PLAN 的 M3 一栏原本写着要给 `sendat` 追加 prompt/payload_hex 字段。**作废。**

`[源码]` AT 口由 `at-daemon` 持有，它的 `sendat` 已经有一个 `raw_at_content`
参数，语义是「hex 解码成裸字节后原样写出、**不追加 `\r`**、然后照常等
`end_flag`」。`AT+CMGS` 的第二阶段（PDU + `0x1A`，且**不能有回车**）正好就是
它描述的形状 —— 双向核对见 `HARDWARE-PROBE.md` §10.1。

⇒ 一次发送是**两次 `sendat`**：

| 阶段 | 提交 | 等 |
|---|---|---|
| 1 | `sendat {at_cmd:"AT+CMGS=27", end_flag:">"}` | 提示符 `>` |
| 2 | `sendat {raw_at_content:"<PDU hex>1A", end_flag:"OK"}` | `OK` / `+CMS ERROR` |

真正缺的是调用序列与租约管理，而它落在 `atq.c`，**不在 `at-daemon`**：

* `struct at_req` 增加 `prompt[]` 与 `payload_hex[]`；**两阶段在同一个队列项内
  完成** —— `atq_issue()` 在 stage1 与 stage2 之间**不返回**，`inflight` 全程
  置位。⇒ 任何轮询都不可能插进「提示符」与「载荷」之间。
* 载荷超长在 `atq_submit_full()` 就返回 `-ENOSPC`，**拒绝而不是截断**：截断的
  PDU 是一条模组会照收的、内容错的短信。
* 新增 `ATQ_PROMPT_TIMEOUT_MS`（10 s）。`>` 一直不来必须有终点，否则队列被
  一条命令永久占住。

### 12.2 ★ 短信绝不进轮询表 —— 这一条决定了整个页面的形状

`[实测]`（§8，无卡，即最坏情形）：

| 命令 | 耗时 | 回答 |
|---|---|---|
| `AT+CSQ`（**在轮询**） | **24 ms** | 正常 |
| `AT+CMGF?` | **5 373 ms** | `+CME ERROR: 10` |
| `AT+CPMS?` | **10 345 ms** | `ERROR` |

AT 口是**一条串行线**。往 `sched.c` 的轮询表里塞一条 `AT+CPMS?`，就等于每轮
把 10 秒交给一个**答案不会变**的问题，后面的信号 / 注网 / 身份轮询全部饿死 ——
模组看起来坏了，而它只是在回答一个没人需要的问题。

⇒ 三条硬规则：

* **`sched.c` 的轮询表里没有 SMS 档。** `housekeeping()` 只调
  `fm160_sms_setup_tick()`，那是**一次性 setup 的重试时钟**，不是轮询。
* **用户动作驱动**：`fm160.sms_sync` 是按钮，`AT+CMGL=4` 的 timeout 给到 45 s。
* **事件驱动**：`+CMTI` 到达时由 `fm160_sms_handle_urc()` 发起 `AT+CMGR=<index>`，
  页面完全不参与。

⇒ 后果：**LuCI 短信页是本项目唯一不 `poll.add()` 的页面**。它打开时读的是
`fm160.sms_list`（daemon 内存，**0 条 AT**），要新数据得有人按按钮。

### 12.3 `AT+CNMI=2,1` 而不是 `2,2`：不让模组把 PDU 吐进 AT 流

`[推断]`（有依据的选择）`AT+CNMI=2,1,0,0,0` 的含义是「新消息来了**只报索引**
（`+CMTI`），正文由主机自己 `AT+CMGR` 去取」。

用 `2,2` 会让模组把**整条 PDU 当 URC 主动推**进 AT 流。后果是 PDU 与普通命令
响应在同一条流上交织，`atq.c` 的单飞假设被打破：一串 16 进制会出现在**任何
命令**的响应中间。

`+CMTI` 只有一个整数。把它当事件、正文走正常命令通道，是唯一与现有 at-daemon
契约相容的做法。若实测看到 `+CMT`（说明 CNMI 没被接受），
`fm160_sms_handle_urc()` 记一条**警告**而不是去解析它 —— 一个不该出现的 URC
被当成正常数据吞掉，比报出来更糟。

### 12.4 ★ `index` 只能从行首读，绝不可按位置推

`[源码]`（**修掉的真 bug**）`AT+CMGL=4` 的响应形如
`+CMGL: <index>,<stat>,,<len>` 后跟一行 PDU 十六进制，第一个字段是**模组上的
存储索引**；而 `AT+CMGR` 的响应里**没有这个字段**（索引是命令参数）。

⇒ 解析器必须知道自己在处理哪一种，不能从「第几个字段」反推。

**同一类错误在 UDH 上又犯了一次**：`parse_udh()` 原本从**字节 0** 开始扫 IE，
但字节 0 是 **UDHL**（头长度本身），不是 IEI。症状是**所有拼接短信都被当成
独立短信**、每段的 `part` 都显示 0 —— 单条短信完全看不出问题。

⇒ 与 §11 的结论同源：**位置是推断，字段名与标志才是事实。**

### 12.5 ★「空」与「零」：`-1` 走 blobmsg 会变成 `4294967295`

`[源码]` blobmsg 没有有符号整数。`index = -1`（「模组上没有副本」）与
`cmgf = -1`（「还没读过」）一旦进快照，读出来就是 `4294967295` —— 一个
**看起来像有意义模式号**的数。

⇒ 每个这种字段都**同时**发一个布尔标志，前端以标志为准：

| 值 | 伴随标志 | 标志为假时 |
|---|---|---|
| `index` | `index_known` | 显示「模组上没有副本」 |
| `cmgf` | `cmgf_known` | 显示「尚未读取」 |
| 时间戳六件套 | `has_time` | 显示 `-`，而不是 `00:00:00` |

这与 §11 的 sentinel 处理是同一条纪律的第二次应用。

### 12.6 ★ 删除动两份副本，而且必须落盘

`[源码]`（**两个真 bug**）

**(a) 只删模组的、或只删本地的，都不够。** 模组存储会满，满了就收不到新
消息 —— 只从列表抹掉，那条消息会一直占着它的槽位。所以 `fm160.sms_delete`
是：先 `AT+CMGD=<index>`（**仅当**模组上确实有副本、`index >= 0`、且功能可用），
**然后无论上一步成败**都删本地条目。

回复**分开报**（`modem_tried` / `modem_deleted` / `local_deleted`），不折成一个
布尔：模组拒绝（无卡、或它的副本已经没了）而本地删成功，是一个**真实且可
预期**的结果，不该显示成「删除失败」。

**(b) 删除必须写进文件。** 存储是 append-only 的 JSONL，删除原本只改内存标志
—— 结果是**重启后删掉的消息全部复活**。修法是往同一个文件再追加一行墓碑
（`{"id":N,"del":1}`），加载时按**文件顺序**应用（墓碑必然在被删行之后）。

⚠️ 文件仍然**从不重写**：重写要么得逐字节复现剩余部分、要么在半途崩溃时赔上
整份历史 —— 为省几 KB flash 不值得。

### 12.7 ★ 列表顺序按 `id`，不按环形槽位

`[源码]`（**真 bug**）本地列表是 64 槽的环。原本「第 n 新」= 从 `head` 往回数
n 个，这在**从不删除**时成立。

一旦删掉中间一条，环上就出现一个洞；下一条新消息会被写进**环上最旧的位置**
（`head` 指向处）而不是末尾。从那一刻起**槽位位置不再等于消息年龄**：往回数
会跳号、会把已删的显示出来、会打乱顺序 —— 全都**看起来像数据损坏**。

⇒ `fm160_sms_get(nth)` 改为**按 `id` 排序选取**（`id` 单调递增、永不复用）。
64 条上的选择排序，可读性值这点开销。

### 12.8 ★ 分段 / 拼接路径上的三个真 bug（单条短信全都看不出来）

`[测试]` 这三条都是**104 项主机侧测试**抓到的，不是读代码看出来的：

| # | 症状 | 根因 |
|---|---|---|
| 1 | 第二段起**每段少首字符**、末段少尾字符 | `plan_segments()` 里一个字符放不下时 `text_step()` 已消费了它的字节，**游标没有回退** ⇒ 下一段从后一个字符开始 |
| 2 | 拼接短信每段 `part` 都是 0 | `parse_udh()` 从字节 0 开始（把 UDHL 当 IEI）—— §12.4 |
| 3 | UCS2 长短信返回 `-E2BIG`，段变成 141 octets | **UDH 填充位属于 GSM7**：UCS2 是字节流、头本身已整字节，再加填充就是 141 > 140 |

第 3 条尤其值得记：GSM7 的 UDH 后面必须补齐到 7 bit 边界（表头 6 octets =
48 bit ⇒ 补 1 bit），**这条规则不适用于 UCS2**。把它当成「UDH 的通用规则」写，
单条短信一样全绿（单条没有 UDH）。

⇒ 三条共同说明：**期望值必须来自外部**。测试先用一份**独立的 Python 实现**
算出 GSM7 打包与 SCTS，两份不一致就**拒绝运行** —— 否则 C 代码只是和自己的
常量自洽。

### 12.9 时间戳不换算时区，年份保持两位

`[推断]` SMSC 盖的是 `09:31`，页面就该显示 `09:31`。换算到路由器时区等于
**发明一个事实**：发送时刻没变，变的是我们显示它的方式。时区偏移**原样显示
在旁边**（`UTC+3`），要加减的人自己会做。

年份**只有两位**（PDU 里就两位），补成四位等于**猜世纪**。

半字节顺序是最容易写反的地方：`99 20 21 50 75 03 21` = `1999-02-12 05:57:30
GMT+3`，**低位半字节在前** —— `0x21` 读作 `12` 而不是 `21`，而且这个错误只在
**13 日以后**的日子上暴露。

### 12.10 长短信与 ZLP 判据：只计数，不打补丁

PLAN §4 的未知量「是否需要 ZLP 内核补丁」在此**落地为计数器**，而不是一个
结论：

| 计数器 | 含义 |
|---|---|
| `sent_ok` / `sent_fail` / `sent_timeout` | 全部发送的结果 |
| `long_sent` / `long_timeout` | **其中多段的**成功 / 超时 |

分两组是刻意的：一条长 PDU 跨越 USB 包边界，是「缺零长包终结」这个猜想的
唯一可疑形态；混进总失败率里会被单段失败淹没。

⇒ 判据是**看 `long_timeout` 是否非零**，而不是「超时率高就补丁」。**不因为一个
猜想改内核**（§3 的代价：一次内核改动 = 整核重编 + 全量回归）。

⚠️ 这组数字目前**全是 0**：无 SIM 连一条都发不出去（见 §12.11）。它们是
**已部署、但尚未产生数据**的证据收集器，不是结论。

### 12.11 ★ M3 的验收边界：无 SIM ⇒ 收发整条路径不可验

`[实测]` 本机**没有插 SIM**，因此：

| 路径 | 状态 |
|---|---|
| PDU 编解码 / 分段 / 拼接 / 号码规范化 | ✅ **104 checks / 0 fail**（主机侧，编译真实源码） |
| setup 在无卡时的降级（不反复重试、不刷 AT） | ✅ 可真机验证 |
| `AT+CMGS` 发送 | ❌ `+CME ERROR: 10`（= SIM not inserted） |
| `AT+CMGL` / `AT+CMGR` 拉取 | ❌ `AT+CPMS?` ERROR ⇒ 无存储可列 |
| 「发送成功」这个形态本身 | ❌ 本机产生不出来 |

⇒ **M3 的这一部分是「代码与自测已交付、真机未验收」**，与 M1/M4/M5 的
「已真机验收」是两种状态，不应混为一谈。

设计上因此有一个刻意的取舍：**setup 只重试 3 次**（首次 4 s 起，失败退避
60 s），之后**只等用户动作或 `+CMTI`**。一个无卡模组每 60 秒问一次
`AT+CPMS?`（10 秒）会永久占用串口 —— 「模组还没有卡」是一个**稳态**，不是
需要反复探测的瞬时故障。

### 12.12 ★★ 参数类型：声明 `BLOBMSG_TYPE_INT64` 的策略会拒绝所有小整数

`[实测]`（**这一类里最隐蔽的一个真 bug**）

libubox 解析 JSON 数字时，**放得进 int32 的存成 INT32 blob，只有超过 2^31 才
升成 INT64**。而 `blobmsg_parse()` 在策略声明了类型时，会比较 blob 类型。

⇒ 一个声明为 `BLOBMSG_TYPE_INT64` 的参数，**接受的是罕见的大值、拒绝的是所有
常见的小值** —— 与意图完全相反。真机上的表现：

```
$ ubus call fm160 sms_delete '{"id":9999}'
Command failed: ubus call fm160 sms_delete {"id":9999} (Invalid argument)
                                  # 本应是 Not found
```

`id` 永远是个小数字，所以 **`sms_delete` 与 `sms_markread` 是 100% 不可用的**
—— LuCI 的删除按钮一发就是 Invalid argument。这类 bug 不会被编译器或任何
「正常路径」测试发现：只有真的从 ubus 打进去才会暴露。

**修法**：策略里**不写 `.type`**（即 `BLOBMSG_TYPE_UNSPEC`）。`blobmsg_parse()`
只在策略声明了类型时才比对，UNSPEC 因此接受任何 blob；再用
`blobmsg_get_u64()` 读 —— 它对 INT32 / INT64 / 数字字符串三种形状都能给出正确
数值。

**同一处的另一个受害者**：M4 的 `setcelllock` 的 `earfcn`。它原来的注释写着
「earfcn 能到 4294967295，所以不能按 int32 走」，于是声明了 INT64 —— 结论正好
反了：`4294967295` 是**唯一**能通过的值，任何普通 EARFCN 都会被拒。因为这条
写路径在 M4 里从未被真机执行过，所以一直没暴露（§9.15 那次意外反而是它第一次
真的跑起来）。两处一起修。

修复后实测：

```
$ ubus call fm160 sms_delete '{"id":9999}'      -> Not found
$ ubus call fm160 sms_delete '{"id":"9999"}'    -> Not found        # 字符串形状也接受
$ ubus call fm160 sms_delete '{}'               -> Invalid argument # 缺参数仍被拒
```

⇒ 一般化的规则：**ubus 上任何接收数字的方法，策略里都别声明 INT64**，除非能
保证调用方永远传大于 2^31 的值。

---

## 13. 无 SIM 时的注册状态（2026-09-19 15:20，无卡台架）

**探针**：`ubus call fm160 at '{"cmd":"…","timeout":N}'`（走 daemon 串行锁；
手动命令会自带 `atq_set_quiet(QUIET_MANUAL, …, 10)` 静音轮询 10 s）。全部只读，
**没有改任何模式、没有写 EFS**。

### 13.1 四条注册域全是 `2`

```
AT+CPIN?     → ERROR                       （裸 ERROR，不是 +CME ERROR: 10）
AT+CFUN?     → +CFUN: 1,0                  （1 = 全功能；0 = 复位参数）
AT+CREG?     → +CREG:  0,2                 CS 域
AT+CGREG?    → +CGREG: 0,2                 PS 域
AT+CEREG?    → +CEREG: 0,2                 EPS / LTE
AT+C5GREG?   → +C5GREG: 0,2                5GS / NR
AT+COPS?     → +COPS: 0                    （没有 <oper> 字段 ⇒ 未选网）
AT+CSQ       → +CSQ: 24,99                 rssi 24 ≈ −65 dBm ⇒ 射频在收信
AT+CESQ      → +CESQ: 99,99,255,255,10,45,255,255,255   rsrq idx 10 / rsrp idx 45
AT+CGATT?    → ERROR
AT+CPOL?     → ERROR
AT+GTCAINFO? → OK                          （无 CA 时的正常态，§9.5）
```

`stat = 2` = **not registered, but MT is currently searching**。四条域**全部是 2**。
⚠️ `CGATT` / `CPOL` 的裸 `ERROR` 与无卡一致，但**不能单独当判据** —— 分不清
「无卡所以没有」和「本模块不支持该命令」。

### 13.2 ★★ 结论：无卡时 **LTE 与 5G 都注册不上**

「没卡只注 LTE、不注 5G」这个前提本身不成立 —— 两个都注不上。原因与 RAT 无关：
**注册是 NAS 层的鉴权过程**，必须由 USIM 提供 IMSI 与长期密钥 K。没有卡，
无论 EPS 还是 5GS 都会被拒，模块永久停在 `stat=2`。

### 13.3 AS 层确实驻留了 LTE —— 这才是「看起来只注 LTE」的来源

`AT+GTCCINFO?`（只读）逐字：

```
+GTCCINFO:
LTE service cell:
1,4,460,11,4580,8048A97,994,129,105,50,-4,45,45,10
LTE neighbor cell:
2,4,,,,,994,F4,,38,38,0
2,4,,,,,73A,BB,,25,25,8
```

服务小区解出：`IsServiceCell=1`、`rat=4`(LTE)、MCC/MNC = `460/11`（**中国电信**）、
`band=105 → B5`（850 MHz）。`GTCCINFO` 的 `earfcn`/`cellid` 是十六进制，见 §9.6。

★ **整段没有任何 NR 行。**

**这与 `stat=2` 不矛盾**：`+CEREG: 0,2` 说的是 **NAS 未注册**，而
`LTE service cell` 说的是 **AS 层（RRC）驻留到的 camped cell** —— 做限制服务
（limited service / 紧急呼叫）只需读广播，**不需要鉴权**。两层语义不同、同时成立。

**为什么驻留的是 LTE 而不是 NR**：NR 行只在 **NSA（EN-DC）** 下由 LTE 配置 B1/B2
测量、并添加 SCG 之后才出现；无卡永远进不了 RRC_CONNECTED ⇒ 永远不会有 NR 行。
SA 驻留则需要 5GS 注册，无卡同样不行。

### 13.4 ★ 但模块**能看到 5G** —— `AT+COPS=?` 扫到了

`AT+COPS=?`（耗时 **29 s**，只读）逐字：

```
+COPS: (1,"460 15","460 15","46015",7),(1,"CHINA MOBILE","CMCC","46000",12),
(1,"CHINA MOBILE","CMCC","46000",7),(1,"CHN-CT","CT","46011",7),
(1,"CHN-UNICOM","UNICOM","46001",7),(1,"460 15","460 15","46015",12),
(1,"CHN-CT","CT","46011",12),(1,"CHN-UNICOM","UNICOM","46001",12),
,(0,1,2,3,4,5),(0,1,2)
```

**四个运营商每个都出现两次：`AcT=7` 与 `AcT=12`。**

`<AcT>`（TS 27.007；多来源交叉核实 —— ModemManager 邮件列表引 27.007 v17.5.0、
Techship 针对 Fibocom 的排障页）：

| AcT | 含义 |
|---|---|
| 7 | E-UTRAN（4G LTE） |
| 11 | NR connected to 5GCN（5G SA） |
| **12** | **NG-RAN（5G 无线接入）** |
| 13 | E-UTRA-NR dual connectivity（EN-DC / 5G NSA） |

⇒ **扫描（idle 下主动扫频）能看到 NR；驻留 / 注册（需要 NAS）不行。** 这两件事的
差别就是「无卡时看不到 5G」的全部成因。

末尾两段是 `+COPS=?` 的**支持枚举**、不是 RAT：`(0,1,2,3,4,5)` = 支持的 `<mode>`
（0 自动 / 1 手动 / 2 注销 / 3 仅设格式 / 4 手动自动 / 5 手动自动回落），
`(0,1,2)` = 支持的 `<format>`（长名 / 短名 / 数字）。

### 13.5 ★ 模块的 RAT 配置是 **5G 优先**，不是「只 LTE」

```
AT+GTRAT?  → +GTRAT: 20,6,3
AT+GTACT?  → +GTACT: 20,6,3,1,8,101,103,105,108,134,138,139,140,141,501,5028,5041,5078,5079
```

（两者前三字段一致 ✓，见 §5.1 / §9.2）

`rat=20` = **NR-RAN/WCDMA/LTE 全模**；`pref1=6` = **NR 优先**；`pref2=3` = LTE 次优。
频段表里 NR 有 **n1/n28/n41/n78/n79 五个**，一个都没关。

⇒ 「只注 LTE」既不是频段限制、也不是 RAT 偏好造成的 —— 这两层都是好的。

### 13.6 可复用的判据

* **`+CSQ` / `+CESQ` 有值 ≠ 已注网** —— 射频收信与 NAS 注册是两件独立的事。
* **`GTCCINFO` 的 `service cell` ≠ 已注册** —— 那只是 AS 层的 camped cell。
* 「当前是 4G 还是 5G」不能只看一条命令。★ 无卡时**看 `AcT` 没有意义**，因为注册
  根本没发生；有卡后应交叉看 `+CEREG?`（AcT=7）与 `+C5GREG?`（AcT=11 / 13）。
* 无卡台架**能**验：端口与 AT 通路、RAT / 频段配置、PLMN 扫描、GNSS、SMS 的
  **解析**路径。
* 无卡台架**不能**验：注网、拨号、拿 IP、SMS 收发（见 §8 / §12）。
* ⚠️ 只有 `AT+C5GREG=2` 才会让 `+C5GREG?` 带回 AcT 字段。本次**没做** —— 它会改
  上报设置，属状态变更，不在只读取证范围内。
