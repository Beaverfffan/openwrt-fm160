# HARDWARE-PROBE — FM160 真机取证记录

> 本文件是**实测记录**，不是设计文档。每条都带原始响应与耗时，用于给
> `docs/AT-FACTS.md`（手册推导）提供真机对照。
>
> 采集日期：2026-09-18  ·  一次性采集，非周期性。

## 1. 环境

| 项 | 值 |
|---|---|
| 载机 | H69K，RK3568，aarch64 |
| 固件 | iStoreOS 24.10.6 (`r29631-b1bb873944`)，kernel 6.6.127 |
| 载机地址 | `192.168.100.1`（br-lan），ssh 免密可直连 |
| 模块 | Fibocom **FM160-CN**，SN `FP62********`，IMEI `86170**********` |
| 固件版本 | `89614.1000.00.04.01.02` (SVN 02) |
| SIM | **未插卡** —— 所有"无卡"行为都是本记录的基线 |
| 载机上已有软件 | **QModem**（完整套件）+ `ubus-at-daemon`（May 8 版） |

### 1.1 AT 通道

载机上已跑着 QModem 的 `ubus-at-daemon`，它按需 open 端口，是本次唯一使用的
通道（不自建 `stty`/`cat`，避免与它抢口）：

```sh
ubus call at-daemon sendat '{"at_port":"/dev/ttyUSB2","timeout":6000,
                             "end_flag":"OK","at_cmd":"AT+CGMI"}'
```

`end_flag` 接受**逗号分隔多值**（如 `"OK,ERROR"`），实测生效。

**实测：`sendat` 结束后端口保持打开**（`ubus call at-daemon list` 显示
`is_open:1, fd:7`，不会被自动关闭）。这是两阶段短信可行的前提。

## 2. USB 枚举（真机）

```
2cb7:0104   Fibocom FM160 Modem_SN:33FB7276    (bDeviceClass 00)

2-1:1.0  class=ff/ff/30  driver=option    -> ttyUSB0      (DIAG)
2-1:1.1  class=ff/ff/40  driver=option    -> ttyUSB1      (NMEA)
2-1:1.2  class=ff/ff/40  driver=option    -> ttyUSB2      (AT)   ← 管理口
2-1:1.3  class=ff/00/40  driver=option    -> ttyUSB3
2-1:1.4  class=ff/ff/50  driver=qmi_wwan_f -> wwan0 / cdc-wdm0   (RawIP)
```

`dmesg` 关键行：`qmi_wwan_f 2-1:1.4: Fibocom ... work on RawIP mode`，
`rx_urb_size = 16384`。

⇒ 与拨号文档**表 1（高通 `2CB7:010x`）**一致，**AT 口 = `/dev/ttyUSB2`**。

## 3. 身份类命令

| 命令 | 耗时 | 结果 |
|---|---|---|
| `ATI` | 15 ms | `Manufacturer: Fibocom Wireless Inc.` / `Model: FM160-CN` / `Revision: 89614.1000.00.04.01.02` / `SVN: 02` / `IMEI: 86170**********` / `+GCAP: +CGSM` |
| `AT+CGMI` | 16 ms | `Fibocom Wireless Inc.` |
| `AT+CGMM` | 15 ms | `FM160-CN` |
| `AT+CGMR` | 15 ms | `89614.1000.00.04.01.02` |
| `AT+CGSN` | 16 ms | `86170**********` |
| `AT+CFSN` | 16 ms | `+CFSN: "FP62********"` |
| `AT+ICCID` | **18 ms** | `+CME ERROR: 13`（SIM failure；命令本身被识别） |
| `AT+CNUM` | 17 ms | `ERROR` |
| `AT+CCID` | **10 345 ms** | 超时（无 `OK` 也无 `ERROR`） |
| `AT+CIMI` | **5 221 ms** | 仅泄出 URC `+GTDUALSIM`，无数据 |
| `AT+GTPKGVER?` | **12 901 ms** | `ERROR` + 泄出 URC `+GTDUALSIM` |

⇒ **`AT+CCID` 与 `AT+GTPKGVER?` 在本机不可用于任何自动路径**；

⇒ **`AT+ICCID`（18 ms）远优于 `AT+CCID`（10.3 s）** —— QModem 的
`fibocom.sh:493-496`（先 ICCID 后 CCID 回退）与此一致。
注意：AT 手册 3.1.12 只收录了 `AT+CCID`，**未收录 `AT+ICCID`**，但真机对
`AT+ICCID` 回 `+CME ERROR`（而非裸 `ERROR`），说明命令被固件识别。

## 4. USB 模式（★ 权威数据）

```
AT+GTUSBMODE?   -> +GTUSBMODE: 32
AT+GTUSBMODE=?  -> +GTUSBMODE: (17-18,20-21,24,29-33)
```

**本机支持的 mode 集合 = {17, 18, 20, 21, 24, 29, 30, 31, 32, 33}**

- 当前运行 **32**（QMI）⇒ 印证「17 与 32 共用 PID `0x0104`，PID 无法反推 mode」。
- **19 / 22 / 23 / 28 本机不报告**（虽然出现在厂商端口表里）。
- 无 AT 口的 **20 / 24 / 31** 本机**确实支持** ⇒ 三者是真·单向门。
- 本机无 NCM / RNDIS / GobiNet 目标。

## 5. 注册与信号

| 命令 | 耗时 | 结果 |
|---|---|---|
| `AT+CPIN?` | 22 ms | `+CME ERROR: 10`（SIM not inserted） |
| `AT+CFUN?` | 26 ms | `+CFUN: 1,0` |
| `AT+CSQ` | 24 ms | `+CSQ: 31,99` |
| `AT+CESQ` | 25 ms | `+CESQ: 99,99,255,255,22,64,255,255,255` |
| `AT+CREG?` | 36 ms | `+CREG: 0,2` |
| `AT+CGREG?` | 26 ms | `+CGREG: 0,2` |
| `AT+CEREG?` | 35 ms | `+CEREG: 0,2` |
| `AT+COPS?` | 16 ms | `+COPS: 0` |
| `AT+GTSIMTYPE?` | 14 ms | `ERROR`（本机不支持） |
| `AT+GTDUALSIM?` | 15 ms | `+GTDUALSIM: 0,"SUB1","No Service"` |

**`AT+CESQ` 返回 9 个字段**（rxlev, ber, rscp, ecno, rsrq, rsrp, ss_rsrq,
ss_rsrp, ss_sinr）—— 实测确认，`rsrq=22 → -8.5 dB`、`rsrp=64 → -76 dBm`。

`<stat>=2` = not registered, but searching（无卡基线）。

`+GTDUALSIM: 0,"SUB1","No Service"` 会**周期性出现在别的命令响应里**
（`AT+CIMI`、`AT+CGDCONT?`、`AT+GTGPSCFG?` 都抓到过）⇒ 它是 URC，必须靠
`correlation` 机制与命令响应区分，且**本机是双卡版本**。

`AT+CSQ` 在无卡时给 `31`（不是 99）⇒ 代码里「`rssi > 31 && rssi != 99` 才
视为 ss_rsrp」的探测**不会误触发**。

## 6. RAT / 频段

```
AT+GTACT?  -> +GTACT: 20,6,3,1,8,101,103,105,108,134,138,139,140,141,
                       501,5028,5041,5078,5079
```

按手册 9.1.14 的语法
`+GTACT: [<rat>[,[<PreferredAct1>],[<PreferredAct2>][,<band_1>[,…]]]]`：

| 位置 | 值 | 含义 |
|---|---|---|
| rat | `20` | NR-RAN/WCDMA/LTE |
| PreferredAct1 | `6` | NR-RAN 优先 |
| PreferredAct2 | `3` | LTE 次优先 |
| bands | `1,8` | UMTS B1 / B8 |
| bands | `101,103,105,108,134,138,139,140,141` | LTE B1/B3/B5/B8/B34/B38/B39/B40/B41 |
| bands | `501,5028,5041,5078,5079` | NR n1/n28/n41/n78/n79 |

⇒ **band 列表是一个扁平的混合列表，没有 count 字段**，制式只能靠数值区间
区分（1–99 UMTS / 101–164 LTE / 50x NR）。

### 6.1 能力枚举（★ M4 的数据源）

```
AT+GTACT=? -> +GTACT: (1,2,4,10,14,16,17,20),(2,3,6),(2,3,6),( ),(1,8),
                     (101,103,105,108,134,138,139,140,141),( ),( ),(501,5028,5041,5078,5079)
```

九组依次是 `<Rat>` / `<PreferredAct1>` / `<PreferredAct2>` / `<gsm_band>` /
`<umts_band>` / `<lte_band>` / `<cdma_band>` / `<evdo_band>` / `<nr_band>`。

⇒ GSM / CDMA / EVDO 三组**为空**——本机不支持这些制式，UI 不应展示。

⇒ 这也是 FM160-CN（中国版）的指纹：LTE 含 B34/B39/B40/B41，NR 含 n41/n78/n79。

⚠️ QModem 的 `fibocom.sh:745` 注释里记着**另一台**设备的答案，只有 **3 组**
（`(1,2,4,10),(2,3),(),0,1,3,5,8,101,…`）⇒ **`AT+GTACT=?` 的组数在不同固件
上不一致**，解析器必须两种都容忍，不能硬编码 9 组。

## 7. `AT+GTCCINFO?`（★ 与手册差异最大）

真机原始响应（多行 + 文本标签）：

```
+GTCCINFO:
LTE service cell:
1,4,460,01,4F50,52BF337,672,1E4,103,100,44,63,63,24
LTE neighbor cell:
```

按手册 9.1.15 的 LTE 服务小区定义
`<IsServiceCell>,<rat>,<mcc>,<mnc>,<tac>,<cellid>,<earfcn>,<physicalcellId>,<band>,<bandwidth>,<rssnr_value>,<rxlev>,<rsrp>,<rsrq>`
—— 14 个字段，**逐字对上**。

| 字段 | 原始 | 解析 | 说明 |
|---|---|---|---|
| IsServiceCell | `1` | 服务小区 | 手册：1=Service，**2**=Not Service |
| rat | `4` | LTE | 0 invalid / 2 WCDMA / 4 LTE / 9 NR |
| mcc / mnc | `460` / `01` | 十进制 | |
| tac | `4F50` | **0x4F50 = 20304** | **十六进制** |
| cellid | `52BF337` | **0x52BF337 = 86725431** | **十六进制** |
| earfcn | `672` | **0x672 = 1650** | **十六进制**（B3 的 EARFCN 范围含 1650 ✅） |
| pci | `1E4` | **0x1E4 = 484** | **十六进制** |
| band | `103` | B3 | 与 `AT+GTACT?` 同一编码（100+n） |
| bandwidth | `100` | 20 MHz | LTE 用 RB 数 |
| rssnr_value | `44` | 22 dB | 0.5 dB/step |
| rxlev / rsrp / rsrq | `63` / `63` / `24` | -77 / -77 / -7.5 dB | |

**三条与手册/直觉相悖、必须在代码里显式处理的点**：

1. **标签行存在**，且真机标签（`LTE service cell:`）比手册措辞
   （`LTE/eMTC/NB-IoT service cell:`）**短**——不能按手册字符串匹配。
2. **EN-DC 会把两行不同结构的行放在同一个 `LTE-NR EN-DC service cell:` 标签下**
   ⇒ 列布局只能取自**行内的 `IsServiceCell,rat`**，不能取自标签。
3. **数值是十六进制**，手册只以 "range is 0-0xFFFFFFF / 0-0xFFFFFFFF" 暗示。

⚠️ `+GTCCINFO:` 独占一行，**内容在后续行**。任何"取前缀同行内容"的工具
（如本项目 `fm160_resp_lines()`）在此**返回 0 行**，会静默失效。

## 8. GNSS

命令**不在 AT 手册里**，出自 *Application Guide_GNSS_V1.0*。

| 命令 | 耗时 | 结果 |
|---|---|---|
| `AT+GTGPSPOWER?` | 22 ms | `+GTGPSPOWER: 0`（关闭） |
| `AT+GTGPSPOWER=?` | 16 ms | `+GTGPSPOWER: (0,1)` |
| `AT+GTGPS=?` | 15 ms | `+GTGPS: "RMC","GGA","GSA","GSV"` |
| `AT+GTGPSEPO?` | 16 ms | `+GTGPSEPO: 0` |
| `AT+GTGPSEPO=?` | 15 ms | `+GTGPSEPO: (0-2)` ← 手册示例写 `(0,2)` |
| `AT+GTAGPSSERV?` | 36 ms | `+GTAGPSSERV: "supl.qxwz.com",7276` |
| `AT+GTGPSCERT?` | 14 ms | `OK`（无证书，无数据行） |
| `AT+GTGPSCFG?` | 15 ms | `+GTGPSCFG:` / `0,2` / `2,14` / `3,0` |
| `AT+GTCAINFO?` | 16 ms | `OK`（无数据） |
| `AT+GTGPS?`（GNSS 关闭时） | 27 ms | `ERROR` |

**要点**：

- GNSS 默认**关闭**（`GTGPSPOWER = 0`），所以直接读 `AT+GTGPS?` 会 `ERROR`；
  必须先 `AT+GTGPSPOWER=1`。
- 命令名是 **`AT+GTAGPSSERV`**（`GT` 前缀 + 两个 S），默认 SUPL 是千寻位置
  `supl.qxwz.com:7276`。
- **`AT+GTGPSCFG?` 只回 3 行（x=0/2/3），缺 x=1（xtra）**，而手册说应回 4 行
  ⇒ 解析器不能假设行数/顺序。当前值：supl version=2 (SUPL2.0)、卫星组合
  =**14（全星座）**、supl 认证=关闭。
- `AT+GTGPSCERT` 是**两阶段命令**（等 `>` 提示符后送证书），与 `AT+CMGS` 同型。

## 9. 慢命令清单（★ 决定轮询设计）

无卡时实测（同一通道、同一超时设置）：

| 命令 | 耗时 |
|---|---|
| `AT+GTPKGVER?` | **12 901 ms** |
| `AT+CCID` | **10 345 ms** |
| `AT+CPMS?` | **10 345 ms** |
| `AT+CMGF?` | **5 373 ms** |
| `AT+CIMI` | **5 221 ms** |
| `AT+CGDCONT?` | **5 211 ms** |
| `AT+GTURCMODE?` | 26 ms（`+CME ERROR: 10`） |
| `AT+CGACT?` | 15 ms（`ERROR`） |

对照组（**本项目轮询实际使用的命令**）：

| 命令 | 耗时 |
|---|---|
| `AT+GTUSBMODE?` | 16 ms |
| `AT+CSQ` | 24 ms |
| `AT+GTCCINFO?` | 25 ms |
| `AT+CREG?`/`AT+CGREG?`/`AT+CEREG?` | 26–36 ms |
| `AT+CESQ` | 25 ms |
| `AT+GTCELLLOCK?` | 16 ms |

⇒ **轮询表（reg/signal/cell）全部 ≤ 36 ms，没有被慢命令污染**。

⇒ 但端口是串行的，一条 10 s 的卡顿会**饿死所有其他命令**——这正是用户所说
"高通的 AT 很脆弱"的量化形态。慢命令必须留在静默窗 + 显式用户动作之后。

## 10. 载机上的 at-daemon：两个版本

**真机装的（May 8）只有 4 个方法**：

```
open / sendat / list / close
```

其二进制字符串里**没有 `ubus_send_event`、没有 `qmodem.at.line`/`qmodem.at.urc`、
没有 `correlation`/`drop_count`**，而是 `callback_prefix`/`callback_reg`/
`callback_script` + `system()` ⇒ **脚本回调模型，不推 ubus 事件**。

**本项目复用的上游 `main` 版（`src/` 已补齐）有 11 个方法**：

```
open / sendat / list / close
lease_acquire / lease_renew / lease_release
urc_register / urc_unregister / urc_list
```

且 `line_events.c` 里确有：

```c
ubus_send_event(ctx, "qmodem.at.line", b.head);   /* 每一行 */
ubus_send_event(ctx, "qmodem.at.urc",  b.head);   /* correlation==IDLE 且前缀命中 */
blobmsg_add_string(&b, "correlation", at_correlation_name(...));
blobmsg_add_u64(&b, "drop_count", ...);
```

⇒ **结论：能力齐全的是"上游新版"，不是"载机上那个旧版"。** 在载机上做联调
时如果用它对测事件功能，会得到"事件不存在"的错误结论——那是版本差异，不是
我们的代码问题。

### 10.1 两阶段短信：不需要改 at-daemon

`sendat` policy 字段为
`at_port, timeout, end_flag, at_cmd, raw_at_content, sendonly, owner`。

`at_handler.c:166-179` 的 `is_raw` 分支把 `raw_at_content` 当**十六进制字符串**
解码成裸字节送出，**不加 `\r`**，且**照常等待 `end_flag`**。

⇒ `AT+CMGS` 两阶段可以直接用：

```sh
# 1) 拿到提示符（配合 lease_acquire 独占，避免插队）
sendat { at_cmd:"AT+CMGS=27", end_flag:">", timeout:5000 }
# 2) 送 PDU + Ctrl-Z，并等 OK
sendat { raw_at_content:"<PDU hex>1A",  end_flag:"OK", timeout:60000 }
```

⇒ README 里"需要给 `sendat` 追加 `prompt`/`payload_hex` 字段"的**判断是错的**，
上游能力已足够。真正需要补的是 `fm160d` 侧的调用序列与租约管理。

## 11. 本轮真机取证导致的代码修正

| # | 修正 | 依据 |
|---|---|---|
| 1 | `fm160_parse_ccinfo()` **整体重写**：改按行扫描、跳过标签行、按行内 `rat` 选列布局 | §7 |
| 2 | 新增 `csv_hex_ll()`，`tac`/`cellid`/`earfcn`/`pci` 改十六进制解析 | §7 |
| 3 | `ident_plan[]` 的 `AT+CCID` → `AT+ICCID` | §3 |
| 4 | `api.js` 新增 `USB_MODE_HW`（真机 10 个 mode）+ `usbModeHwSupported()` | §4 |
| 5 | `USB_MODE_PREFERRED` 去掉 19/22/23（本机不支持）：qmi `[32,17]`、mbim `[30,29]`、ecm `[33,18]` | §4 |
| 6 | `candidates()` 在缺 live list 时回退到 `USB_MODE_HW` 而非"不过滤" | §4 |
| 7 | `sched.c` 加入慢命令黑名单注释（防未来回归） | §9 |
| 8 | `at-daemon/src/` 补齐 10 个源文件（此前只有 Makefile，包无法编译） | §10 |
| 9 | `decode_band()` 注释改为"真机已证实用前缀式编码" | §6 |

## 12. 尚未验证（下次上机应做）

1. **有卡后**的 `AT+GTCCINFO?` —— 邻区行、NR 行、EN-DC 双行的真实形状。
2. **`AT+GTGPSPOWER=1` 后的 `AT+GTGPS?`** —— NMEA 实际行数与字段。
3. **`AT+GTCAINFO?` 在有卡/有业务时的数据行格式**。
4. **`>` 提示符能否作为 `end_flag` 命中**（本轮未试，避免在无卡时触发写操作）。
   —— 可从上游 `check_end_flags()` 的匹配逻辑静态确认。
5. **`AT+GTACT=?` 在别的固件上的组数**（已知至少两种，见 §6.1）。
6. 短信面（`AT+CPMS`/`AT+CMGF`/`AT+CMGL`）是否在**有卡后**变快。
7. `AT+GTCELLLOCK?` 的完整回读格式（本轮只有 `+GTCELLLOCK: 0`）。

---

*采集方式：ssh 到载机 → 经 `ubus call at-daemon sendat` 逐条发送 → 本地 python
解析 JSON 响应。原始响应留档在 `fm160-luci/_probe/out-0*.json`（旧仓）。*
