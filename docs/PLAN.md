# openwrt-fm160 — 项目决策与实施路线

## 0. 已确认的前提（用户答复）

| 项 | 决定 |
|---|---|
| 目标固件 | **ImmortalWrt 25.x** |
| 硬件连接 | **USB 转接板外接 FM160** |
| 联调方式 | **先只出代码，暂不真机联调** ⇒ 一切设备相关取值必须运行期探测，不得硬编码 |
| 许可证 | 个人使用，不在意 GPL/商用限制 ⇒ **允许复用 QModem 组件** |

「暂不联调」是本次设计最强的一条约束：**所有只能靠真机确定的量，代码里必须写成探测 + 保守默认 + 显式告警**，不允许假定。见 §4 的「未知量处理表」。

## 1. 关键架构决策

### D1 — 复用 `ubus-at-daemon` 作为 AT 传输层（不改它）

调研结论：QModem 的 `application/ubus_at_daemon`（2485 行 C）已经是我们要的东西，而且做得比预期好：

| 已有能力 | 对本项目的价值 |
|---|---|
| 每端口独立 reader 线程 + 队列 mutex/cond + write mutex | 串口读写不互相踩 |
| `lease_acquire/renew/release`（带 TTL 的端口租约） | 显式声明「本机只有 fm160d 用这个口」 |
| `urc_register(port, owner, urc_id, prefix)` | 按前缀订阅 URC，不影响其它消费者 |
| **`ubus_send_event("qmodem.at.line", …)` 逐行全量推送** | 被动拿到所有行，不需要轮询 |
| **`ubus_send_event("qmodem.at.urc", …)` 按前缀推送** | 直接拿 `+CMTI` 等 |
| `correlation` ∈ IDLE/RESPONSE/TERMINAL/AMBIGUOUS | **区分「命令响应」与「真 URC」**——这正是轮询最容易搞错的地方 |
| `restart_epoch` / `sequence` / `drop_count` | 能检测队列溢出丢事件 |
| 端口 monitor 线程 + 自动重连 | 拔插模块自愈 |
| `tests/test_port_concurrency.c`、`test_reader_flood.c` | 有并发/洪泛测试背书 |

⇒ **零改动复用**。`fm160d` 只做策略层。

### D2 — 分层：传输层 / 策略层 / 表现层

```
LuCI JS ──ubus──▶ fm160d（策略层：状态机·调度·语义·解析）
                    │  ubus call at-daemon sendat / lease_acquire / urc_register
                    │  ubus subscribe qmodem.at.urc / qmodem.at.line
                    ▼
              at-daemon（传输层：串口独占 + 队列 + 行事件）
                    │ /dev/ttyUSBx
                    ▼
                  FM160
```

### D3 — 唯一的 AT 发起者

`fm160d` 内部维护一个 **优先级队列 + 单飞（one in flight）** 的 AT 客户端：
- 优先级：`0 交互（用户点击）` > `1 状态机（拨号/切换/短信）` > `2 轮询`
- 同命令去重（合并回调）
- 优先级 2 等待 > 30 s 自动提升为 1（防用户连点把轮询饿死）
- **任何组件都不允许绕过 `fm160d` 直接调 `at-daemon`**（ACL 里把 `at-daemon` 只授予 `fm160d` 的 ubus 用户）

### D4 — 短信发送需要一个**传输层补丁**（唯一需要动 at-daemon 的地方）

`AT+CMGS=<len>` / `AT+CMGW=<len>` 是两阶段事务：先等 `> ` 提示符，再写 PDU + `0x1A`。
而 `sendat` 是「发命令 → 等终态」的一次性模型，且**同端口串行** ⇒ 第二条 `sendat` 会排在第一条后面，形成死锁（第一条在等一个永远不来的终态）。

方案：给 `sendat` **追加式**加一个可选字段（不改现有语义）：

```
sendat {
  at_port, at_cmd, timeout,
  prompt: "> ",        // 新增：等到该字面量后，再写 payload
  payload_hex: "…",    // 新增：提示符到达后写出的裸字节（十六进制）
  end_flag: "+CMGS"    // 写出后期待的终态
}
```

约 30 行改动，落在 `at_handler.c`。**在补丁就位前**，SMS 走降级路径（见 §4）。

### D5 — 不引入 QModem 的 UI 与脚本层

它的 LuCI 页面、`cmds/*.sh`、`modem_ctrl.sh` 是通用多厂商框架，与「只适配 FM160、追求稳定」相冲突。**只复用 C 传输层，其余全部原创。**

## 2. 模块划分

```
openwrt-fm160/
├── at-daemon/            ← 复用 QModem application/ubus_at_daemon（保留其 MPL 头与出处说明）
│   └── patches/0001-sendat-prompt.patch    ← D4 的加法补丁
├── fm160d/               ← 原创策略层（C / uloop / libubus）
│   ├── src/
│   │   ├── fm160d.h        类型与接口
│   │   ├── main.c          ubus 连接、事件订阅、端口发现、启动自检
│   │   ├── atq.c           AT 优先级队列 + sendat 异步客户端
│   │   ├── sched.c         分级轮询 + 抖动 + 退避 + 静默窗 + 熔断
│   │   ├── state.c         状态快照 + 变更推送
│   │   ├── cmds.c          FM160 命令与解析（身份/注网/信号/小区）
│   │   └── ubus_methods.c  ubus 对象 fm160 的方法
│   └── files/etc/{init.d,config,uci-defaults,hotplug.d}
├── luci-app-fm160/       ← 原创 LuCI JS
│   ├── htdocs/luci-static/resources/{view/fm160/*.js, fm160/*.js}
│   └── root/usr/share/{luci/menu.d,rpcd/acl.d}/*.json
└── docs/{AT-FACTS.md, DESIGN.md, PLAN.md}
```

## 3. 里程碑

| 期 | 内容 | 本期状态 |
|---|---|---|
| **M1** | `fm160d` 核心：端口发现、AT 队列、事件订阅、分级调度、静默窗、熔断、状态缓存、ubus 接口、身份/注网/信号/小区解析；LuCI 概览页 + 信号页 + AT 调试页 | **本次交付** |
| **M2** | 拨号：QMI/MBIM 原生 proto 接入 + ECM 自定义 proto + 重连阶梯 + **模式切换白名单/回滚状态机** | ⚠️ **代码完成 + 主机侧两轮自测通过 + 未装机**。交付：`usbmode.c/.h`（模式表、风险判定、白名单交集）、`net.c/.h`（拨号阶梯、AT+CGDCONT/GTWWAN/GTRNDIS 命令构造、PDP 与地址解析）、`dialer.c`（阶梯状态机、熔断式重连、复位台账）、`modesw.c`（切换状态机 + 回滚 + 切换前后回写点）、`files/lib/netifd/proto/fm160.sh`（ECM 自定义 proto）、`files/usr/libexec/fm160-preflight.sh`、`dial.js` + 4 个 ubus 方法。**★ 自测 `25-usbmode-dial-test.sh` 341 checks / 0 fail**：`usbmode.c`+`net.c` 用**真编译器**驱动，且期望值有一半来自 node **真跑 `api.js`**（前端另有一份同源实现）后比对，不是抄副本。**★ `26-m2-structural-check.sh` 67 checks / 0 fail**：覆盖 C 测试够不到的那半（两个状态机接线、netifd proto、装机前检、菜单/ACL 可达性、页面不得自带风险策略或 profile 表）——grep 级，只回答「接线被拆掉会不会有人发现」。**未验收**：本机无 SIM ⇒ 拨号成功路径（注网→激活→拿到地址）**产生不出成功形态**，模式切换亦同。见 `AT-FACTS.md` §13 |
| **M3** | 短信：PDU 编解码（GSM7/UCS2/UDH）、收发、存储、长短信、`sendat` prompt 补丁 | ⚠️ **代码完成 + 自测通过 + 已装机，真机收发不验收**。交付：`pdu.c/pdu.h`（编解码、分段、UDH 拼接）、`sms.c`（存储/去重/持久化/setup 状态机/收发展开/解析器）、`atq.c` 两阶段事务、5 个 ubus 方法、LuCI 短信页。主机侧自测 **104 checks / 0 fail**（`_tools/istoreos-h69k/23-sms-pdu-test.sh`，期望值由**独立 Python 实现**算出，不一致则拒绝运行）；设备侧 **`DEVICE VERIFY OK`**（`15-verify-device.sh` 新增 §9b）。**真机无 SIM ⇒ 收发整条路径不可验**（`AT+CPMS?` 10 345 ms ERROR、`AT+CMGF?` 5 373 ms `+CME ERROR: 10`），只验到「无卡时优雅降级」这一形态。**★ D4 的 `sendat` prompt 补丁已判定不需要**（§12.1）。复查中修掉 **8 个真 bug**：UDH 起点、分段游标、UCS2 填充位、删除不落盘、列表按环形槽位取、删除不删模组副本、`BLOBMSG_TYPE_INT64` 策略拒绝小整数（删除/标记已读完全不可用）、前端 banner 分支顺序（无卡时把真正原因藏起来）。见 `AT-FACTS.md` §12 |
| **M4** | 锁频 / 锁小区 / 基站：动态能力枚举、一键锁当前小区、邻区表、CA | **已交付并真机验收**（`DEVICE VERIFY OK`）。band lock 写路径**两步走真机验通**（负向探测 + 恒等回写，实测确认 `AT+GTACT=,,,` 的空字段语义是「保留现值」）；cell lock **从未以锁定形式写入**（需 UE 复位，且无 SIM 无目标可锁）—— 2026-09-19 有过一次意外的**禁用**态回写（`AT+GTCELLLOCK=0`，恒等、状态逐字段未变），验收脚本据此把「禁用写入」与「锁定写入」分开计数。见 `AT-FACTS.md` §9（§9.15 = 那次意外） |
| **M5** | GNSS：开关、卫星组合、NMEA 解析、卫星图、可选 TCP 转发 | ✅ **已交付并真机验收**（`DEVICE VERIFY OK`；解析器自测 239 checks / 0 fail，`_tools/istoreos-h69k/21-gnss-parser-test.sh`；真机联调见 `22-gnss-live-test.sh`）。真机事实：**开引擎对连接性影响 = 0**（USB 契约逐项不变、无重枚举、不写 EFS）、NMEA **从 AT 口出**（本模式无 GNSS 口 ⇒ 无需抢端口）、**有天线（四根，与蜂窝 MIMO 共用）但室内恒 0 颗星、无定位、归因未定**、`GTGPS?` 30 ms。**读路径已在真机跑过真实帧**（8 句 / 184 B / 5 talker / 0 校验错，与主机侧预测逐项吻合）。仍未实测：有定位形态、空帧分支、`setgnsscfg` 的写。见 `AT-FACTS.md` §10/§11（§11.11 = 真机联调） |
| **M6** | 打磨：i18n、日志导出、CI | ⚠️ **代码完成 + 主机侧验收通过 + 未装机**。**i18n**：`po/zh_Hans/fm160.po` **506 条**，覆盖 **482/482 (100%)**（含 **16 条 `msgctxt` 消歧项** —— 与 luci-base 共享、译文不同的 msgid 会因 `lmo_load_catalog` 的链序决定服务端与浏览器读到**相反**的 archive，故占用 luci-base 不写的键；菜单标题带不了 context，所以 Signal 页改名 "Signal Quality"）。门禁 `tools/i18n/check.py` **80 项 0 失败**：真实 `po2lmo` 往返（真跑编译出的 `.lmo` 去对 `sfh` 键）、**真跑 `cbi.js`** 比对哈希、跨目录撞键扫描（29 个共享键 / **0** 个译文不同）、**`FM160_TRANSLATIONS` 与 `po/` 目录双向一致**（原检查只做子串匹配 —— `'FM160_TRANSLATIONS:=zh_Hans:zh-cn'` 是更长列表的**前缀**，于是列表尾部多出一个没有 `po/` 的语言时它仍是绿的；而 Makefile 自己的 `$(error)` 在**包扫描**阶段就中断，扫描只记下应用包、不记翻译包 ⇒ 光修 Makefile 也会刷进一个**没有中文**的镜像，故门禁补上双向集合比较 + 自测）、**`$(call)` 未被折行**（`$(call)` 不裁参数、折行处的 `\<newline>` 会变成一个空格 ⇒ 别名到手是 `" zh-cn"`，包名变成 `luci-i18n-fm160- zh-cn`（Kconfig 非法 ⇒ `make defconfig` 直接 Error 1）、装出来的 `.lmo` 叫 `fm160. zh-cn.lmo`（`load_catalog()` 按 `*.zh-cn.lmo` 匹配，永远找不到）、uci 默认值 `luci.languages. zh_cn` 也不是合法语法 —— Makefile 里三处都看不出异常）、**翻译包带 `DEFAULT:=LUCI_LANG_<po 目录>||(ALL&&m)`**（HIDDEN 包若没有 `DEFAULT`，生成出来只是一个 `tristate` + `default y if DEFAULT_<它自己>`，而那个符号无人定义 ⇒ **没有任何配置能选中它**，等于翻译从没写过：镜像里没有 `.lmo`，设备上只表现为「还是英文」，零症状）。覆盖率由门禁**判失败**兜底：任何没被 po 载入的前端消息都会让门禁变红；提取基另配**逐文件下限 canary**，防止正则悄悄少匹配一个文件时把「回归」显示成「待办」。**日志导出**：`fm160_log()` 旁挂 **128 行定长环形缓冲**（只进 syslog 的那份内容，级别闸门之后），新增只读 ubus `fm160 diagnostics` 返回**纯文本支持包**（状态 + 日志尾，**不碰模组** ⇒ 可在故障机上安全执行）；`diag.c` 为纯层，主机侧 `tools/hosttest/diag-export-test.sh` **92 checks / 0 fail**（含最坏情况定容：满环 128 行满长 + 所有字符串取满 = 32 192 / 34 816 B），另有回复键、只读性与页面读取互检 **8/8**（含「handler 不排队任何 AT 工作」这一只读性断言）。**CI**：`.github/workflows/check.yml`（ubuntu-latest + node22 + python3.12 + ziglang），跑 `tools/check.sh`（`STRICT=1`）。**未验收**：CI 未在真 GitHub runner 上跑过；M2/M6 的 `.ipk` 未编出、未装机 |

## 4. 未知量处理表（**因为没有真机，这节最重要**）

| 未知量 | 代码怎么处理 | 失败时的表现 |
|---|---|---|
| 哪个 `ttyUSB*` 是 AT 口 | 枚举宿主 USB 里 `idVendor == 2cb7` 的串口 → 逐个 `sendat "AT"` 探测 | 找不到 → `port_found=0`，UI 黄条提示，5 s 后重试（低频） |
| 当前 USB mode | `AT+GTUSBMODE?` 回读；**绝不用 VID:PID 推断** | 查询失败 → 显示 unknown + 提示 |
| 支持哪些 USB mode | `AT+GTUSBMODE=?` 取设备列表，与我们的白名单取交集 | 取不到 → **禁止一切模式切换**（只读） |
| 拨号激活命令是 `+GTWWAN` 还是 `+GTRNDIS` | M2 实现：先 `AT+GTWWAN=?`，ERROR 再试 `AT+GTRNDIS=?`，结果缓存到运行时 | 都失败 → 拨号报错并给出原始响应 |
| 是否需要 ZLP 内核补丁 | 代码侧不做假设；M3 起在 SMS 发送路径**统计长 PDU 超时率**并上报，作为是否打补丁的判据 | 超时率高 → UI 明确提示「疑似缺 ZLP 补丁」 |
| CESQ/GTCCINFO 的字段差异 | 解析器按 `+GTCCINFO:` 行内 `<rat>` 字段分支；未知字段容错跳过 | 解析失败 → 保留上一次有效值并标记 stale |
| 短信存储位置 | `AT+CPMS=?` 探测；`ME` 优先，回退 `SM` | 都不支持 → 短信功能置灰 |
| GNSS 是否可用 | 先 `AT+GTGPSPOWER?`，ERROR 则功能置灰 | — |

## 5. 「AT 脆弱」的落实清单（对照设计原则逐条可查）

| 编号 | 措施 | 落点 |
|---|---|---|
| A1 | 单飞：任意时刻 ≤ 1 条 AT | `atq.c` |
| A2 | 轮询分 6 级，间隔 5–300 s，带 ±20% 抖动 | `sched.c` |
| A3 | 失败退避 ×2，上限分级封顶 | `sched.c` |
| A4 | 熔断：连续 3 次超时 → 停轮询 60 s；10 次 → 停自动轮询 | `sched.c` |
| A5 | 静默窗：拨号 / CFUN / 模式切换 / COPS 扫描 / 手动 AT | `sched.c` |
| A6 | **流量统计读 `/sys/class/net/*/statistics`，0 条 AT** | `cmds.c` |
| A7 | URC 优先：`+CMTI`/`+CEREG`/`+C5GREG`/`+CGEREP` 事件驱动 | `main.c` |
| A8 | 慢变命令（CGMI/CGMM/CGSN/CCID/GTUSBMODE?）**不进轮询** | `cmds.c` |
| A9 | 优先级 2 饥饿保护（> 30 s 提升） | `atq.c` |
| A10 | 队列积压超过水位时**丢弃本轮轮询**并计数 | `sched.c` |
