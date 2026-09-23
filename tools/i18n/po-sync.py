# -*- coding: utf-8 -*-
# One-shot po sync: add entries the extractor now requires, drop orphans the
# rewrite removed, keep every msgstr non-empty (identity = honest English
# fallback, which po2lmo omits by design).  Uses check.py's own extractor so
# the result is exactly what the i18n gate computes.
import io, os, json, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'repo', 'tools', 'i18n'))
import check

root = os.path.abspath(os.path.join(HERE, '..', 'repo'))
pkg = os.path.join(root, 'luci-app-fm160')
res = os.path.join(pkg, 'htdocs', 'luci-static', 'resources')
po_path = os.path.join(pkg, 'po', 'zh_Hans', 'fm160.po')

# msgid -> zh translation, for the strings this sync adds.  Anything not here
# falls back to an identity msgstr (shown as English), which is deliberate for
# tokens luci-base also carries (active/down/running/...) so a bare key can
# never disagree with another catalogue.
ZH = {
 '%d consecutive / %d in 24 h / %d total': '%d 连续 / 24 小时内 %d / 累计 %d',
 '%s, %s MHz, PCI %d, %s': '%s，%s MHz，PCI %d，%s',
 'A failed round triggers a redial; after the consecutive-failure threshold the module USB profile is bounced and reloaded; after the 24 h budget is exhausted the watchdog stops by itself.':
   '一轮失败先触发重拨；连续失败达到阈值后模块 USB 配置会被弹掉重载；24 小时预算耗尽后看门狗自行停止。',
 'APN is required.': '必须填写 APN。',
 'APN may only contain letters, digits, dot, dash and underscore.': 'APN 只能包含字母、数字、点、短横线和下划线。',
 'Actor': '操作方',
 'At least one of IPv4 / IPv6 must be enabled.': 'IPv4 / IPv6 至少启用一项。',
 'Autoscroll': '自动滚动',
 'CQI %d, %s, DL MCS %d / UL MCS %d': 'CQI %d，%s，DL MCS %d / UL MCS %d',
 'CQI / RANK / MCS': 'CQI / RANK / MCS',
 'Check interval (seconds)': '检查间隔（秒）',
 'Check interval must be 10-600 seconds.': '检查间隔必须在 10-600 秒之间。',
 'Clear log': '清空日志',
 'Connect (switches USB profile first)': '连接（必要时先切换 USB 配置）',
 'Connect automatically at boot': '开机自动连接',
 'Connecting...': '正在连接…',
 'Connection': '连接',
 'Could not clear the log': '无法清空日志',
 'Could not read the log': '无法读取日志',
 'Could not read the log: ': '无法读取日志：',
 'DL %sx %s / UL %sx %s': 'DL %sx %s / UL %sx %s',
 'Delegate IPv6 /64 to LAN': '向 LAN 下发 IPv6 /64',
 'Detail': '详情',
 'Dial command sent. The link comes up over the next seconds; a mode switch can take a minute.':
   '拨号命令已发送。链路将在随后几秒内建立；模式切换可能需要一分钟。',
 'Dial mode': '拨号模式',
 'Disconnect': '断开',
 'ECM (native)': 'ECM（原生）',
 'Enable keepalive': '启用保活',
 'Every action this plugin performs on the modem - dials, redials, module reloads, band and cell-lock changes, USB mode switches, SMS sends, keepalive verdicts - is appended here with its outcome. The log lives in /var/log/fm160-ops.log and keeps the most recent 300 entries.':
   '本插件对模组的每一项操作——拨号、重拨、模块重载、频段与锁小区变更、USB 模式切换、短信发送、保活判定——都会连同结果追加到这里。日志位于 /var/log/fm160-ops.log，保留最近 300 条。',
 'Every interval the module-reported DNS is pinged over the carrier path (IPv4/IPv6 honouring the stack toggles).':
   '每个间隔通过承载网 ping 模组上报的 DNS（按 v4/v6 开关选择族）。',
 'Failed rounds': '失败轮次',
 'GNSS config change': 'GNSS 配置变更',
 'GNSS power off': 'GNSS 电源关闭',
 'GNSS power on': 'GNSS 电源打开',
 'Give up after N rounds within 24 h': '24 小时内 N 轮后放弃',
 'Give-up threshold must be 2-100 rounds.': '放弃阈值必须在 2-100 轮之间。',
 'IPv4 address': 'IPv4 地址',
 'IPv4 ping target may only contain digits and dots (or empty).': 'IPv4 目标只能包含数字和点（或留空）。',
 'IPv4/IPv6 map to the PDP type on ECM (-4 -6 = IPV4V6) and to -4/-6 flags on QMAP / ip-type on MBIM.':
   'IPv4/IPv6 在 ECM 上映射为 PDP 类型（-4 -6 = IPV4V6），在 QMAP 上映射为 -4/-6 标志，在 MBIM 上映射为 ip-type。',
 'IPv6 address': 'IPv6 地址',
 'IPv6 ping target must be an IPv6 address (or empty).': 'IPv6 目标必须是 IPv6 地址（或留空）。',
 'In Network -> Interfaces': '在 网络 → 接口 中',
 'Keepalive': '保活',
 'Keepalive (link watchdog)': '保活（链路看门狗）',
 'Last check': '最近检查',
 'Link adaptation (AT+GTCELLINFO)': '链路自适应（AT+GTCELLINFO）',
 'Loading…': '加载中…',
 'MBIM (standard)': 'MBIM（标准）',
 'MCS DL / UL': 'MCS DL / UL',
 'MIMO / modulation': 'MIMO / 调制',
 'Module profile': '模块配置',
 'Module rate (AT+GTSTATIS)': '模块速率（AT+GTSTATIS）',
 'Module reload threshold must be 2-50 rounds.': '模块重载阈值必须在 2-50 轮之间。',
 'Module reloads': '模块重载',
 'Operations Log': '操作日志',
 'Operations log': '操作日志',
 'Operator (AT+COPS)': '运营商（AT+COPS）',
 'PCC band': 'PCC 频段',
 'Ping target IPv4 (empty = module DNS)': 'IPv4 探测目标（留空 = 模组 DNS）',
 'Ping target IPv6 (empty = module DNS)': 'IPv6 探测目标（留空 = 模组 DNS）',
 'QCI TX / RX': 'QCI TX / RX',
 'QMAP (vendor)': 'QMAP（厂商）',
 'QMAP channels': 'QMAP 通道',
 'RX %s / TX %s': 'RX %s / TX %s',
 'Radio (AT cross-check)': '无线（AT 交叉验证）',
 'Rank': 'Rank',
 'Refresh': '刷新',
 'Registered': '已注册',
 'Reload module after N consecutive failed rounds': '连续 N 轮失败后重载模块',
 'Result': '结果',
 'SMS delete': '短信删除',
 'SMS mark read': '短信标记已读',
 'SMS send': '短信发送',
 'SMS sync': '短信同步',
 'Save settings': '保存设置',
 'Saving does not dial — press Connect. Changing the mode may restart the USB profile (30-90 s).':
   '保存不会拨号——请按连接。切换模式可能重启 USB 配置（30-90 秒）。',
 'Session total': '会话累计',
 'Settings saved. Press Connect to dial.': '设置已保存。按连接开始拨号。',
 'Switching USB profile...': '正在切换 USB 配置…',
 'TX %s / RX %s': 'TX %s / RX %s',
 'USB mode switch': 'USB 模式切换',
 'USB rescan': 'USB 重新扫描',
 'auto (module DNS)': '自动（模组 DNS）',
 'band lock change': '频段锁变更',
 'cell lock clear': '锁小区清除',
 'cell lock set': '锁小区设置',
 'died unexpectedly': '异常退出',
 'down %s/s / up %s/s': '下行 %s/s / 上行 %s/s',
 'entries': '条',
 'fibocom-dial over qmi_wwan_f with QMAP aggregation. Multi-channel capable; single-flow throughput varies (modem firmware).':
   'fibocom-dial 跑在 qmi_wwan_f 上，QMAP 聚合。支持多通道；单流速率视模组固件而定。',
 'fm160d dials over AT; the module serves DHCP on usb0. Most mature path.':
   'fm160d 通过 AT 拨号；模组在 usb0 上提供 DHCP。最成熟的路径。',
 'identity re-read': '重新读取身份',
 'management pause': '暂停管理',
 'management resume': '恢复管理',
 'mbimcli over cdc_mbim. Best measured single-flow speed (15-20 MB/s on this module).':
   'mbimcli 跑在 cdc_mbim 上。实测单流速度最佳（此模组 15-20 MB/s）。',
 'none (PCC only)': '无（仅 PCC）',
 'not yet (connect once to create it)': '尚未生成（先连接一次以创建）',
 'stopped (given up)': '已停止（自动放弃）',
 'up on %s': '运行于 %s',
 'yes (proto %s)': '是（协议 %s）',
 # --- cell scan (cells.js / signal.js) ---
 'Auto-capturing every 30 s while this page is open…': '本页打开期间每 30 秒自动抓取…',
 'Cell scan (live AT)': '小区扫描（实时 AT）',
 'Neighbour cells (live AT)': '邻区（实时 AT）',
 'Search cells now': '立即搜索小区',
 'Last capture:': '最近抓取：',
 'neighbour cells': '个邻区',
 'No neighbour cells reported this capture.': '本次抓取未上报邻区。',
 'Live AT+GTCCINFO? neighbour list and AT+GTCAINFO? carrier aggregation, captured automatically; press the button for an immediate pass.':
   '实时下发 AT+GTCCINFO?（邻区列表）与 AT+GTCAINFO?（载波聚合），自动抓取；按按钮立即抓取一轮。',
 'Sends AT+GTCCINFO? (serving cell and up to ten LTE / NR neighbours) and AT+GTCAINFO? (carrier aggregation) live, instead of relying on the daemon snapshot. AT+GTCELLSCAN is not used: on this firmware it blocks the AT channel for ~50 s and returns nothing.':
   '实时下发 AT+GTCCINFO?（服务小区及最多十个 LTE / NR 邻区）与 AT+GTCAINFO?（载波聚合），不依赖守护进程快照。不使用 AT+GTCELLSCAN：该固件上它会阻塞 AT 通道约 50 秒且无任何返回。',
 'manual trigger from the cells page': '小区页手动触发',
 'manual trigger from the signal page': '信号页手动触发',
 'cell scan': '小区搜索',
 'raw bw code': '原始带宽码',
 'dBm (raw)': 'dBm（原始值）',
 'dB (raw)': 'dB（原始值）',
 'PCC:': 'PCC：',
 'no carrier aggregation': '无载波聚合',
 # --- overview radio cross-check refresh ---
 'overview refresh': '概览刷新',
 'manual radio cross-check refresh': '无线交叉验证手动刷新',
 'initiated': '已发起',
 'No radio capture yet - press Refresh to take one.': '尚无无线抓取——按"刷新"抓取一轮。',
 # --- mwan3 auto-registration (mwan.js / fm160-mwan3-sync) ---
 'MWAN3': 'MWAN3 均衡',
 'The status snapshot failed': '状态快照获取失败',
 'mwan3 is not installed.': 'mwan3 未安装。',
 'The registration below is written to /etc/config/mwan3, but without the package installed it does nothing. Install it with': '以下注册会写入 /etc/config/mwan3，但没装该包不会生效。安装命令：',
 'Auto-register into mwan3': '自动注册到 mwan3',
 'Wired priority (metric)': '有线优先级（metric）',
 'Wired weight': '有线权重',
 'Mobile priority (metric)': '移动优先级（metric）',
 'Mobile weight': '移动权重',
 'Track targets IPv4 (space separated)': 'IPv4 探测目标（空格分隔）',
 'Track targets IPv6 (space separated)': 'IPv6 探测目标（空格分隔）',
 'Ping count per round': '每轮 ping 次数',
 'Ping timeout (s)': 'ping 超时（秒）',
 'Check interval (s)': '检测间隔（秒）',
 'Failure check interval (s)': '故障检测间隔（秒）',
 'Rounds to declare down': '判为掉线的轮数',
 'Rounds to declare up': '判为恢复的轮数',
 'Lower metric wins failover; equal metrics balance by weight.': 'metric 越小故障切换优先级越高；只有 metric 相同的接口之间才按权重分流。',
 'With the defaults above this is FAILOVER, not balancing: wired eth (metric 10) carries all traffic while it is up, and the FM160 link (metric 20) only takes over when wired goes down. Weights apply between members of the same metric tier only - set both classes to the same metric (e.g. 10/3 wired and 10/1 mobile) for true weighted balancing while both are up. Interfaces are discovered automatically - wired (device ethN) and the FM160 dial interface (whichever dial mode is active). Everything this feature writes into mwan3 is tagged and cleaned up when the interface disappears or auto-registration is turned off; your own mwan3 sections are never touched.':
   '以上默认值是**故障转移**而不是分流：有线 eth（metric 10）在线时承担全部流量，有线掉线后 FM160 链路（metric 20）才接管。权重只在同一 metric 层的成员间生效——把两类的 metric 设成相同（例如有线 10/3、移动 10/1）才是双在线时按权重真分流。接口自动发现——有线（设备 ethN）与 FM160 拨号接口（当前激活的拨号模式）。本功能写入 mwan3 的内容全部带标记，接口消失或关闭自动注册时自动清理；你自己写的 mwan3 配置不会被触碰。',
 'Save and sync': '保存并同步',
 'Sync now': '立即同步',
 'Remove registration': '移除注册',
 'Discovered interfaces': '已发现的接口',
 'Nothing discovered yet. Auto-registration is off, or no WAN-side interface exists.': '尚无可发现接口。自动注册关闭，或不存在 WAN 侧接口。',
 'Interface': '接口',
 'Family': '协议族',
 'Class': '类别',
 'Device': '设备',
 'No fm160-managed mwan3 sections yet.': '尚无 fm160 托管的 mwan3 配置段。',
 'Registered in mwan3: %s': '已注册进 mwan3：%s',
 'The hotplug hook re-runs the registration on every interface up/down, so plugging or unplugging a cable is enough - no save needed.': 'hotplug 钩子会在每个接口 up/down 时重跑注册，插拔网线即可生效——无需保存。',
 'Live state': '实时状态',
 'Live "mwan3 interfaces" output appears here after Sync now.': '按「立即同步」后此处显示 mwan3 interfaces 实时输出。',
 'Saved and synced.': '已保存并同步。',
 'Save failed': '保存失败',
 'Synced.': '已同步。',
 'Sync failed': '同步失败',
 'fm160-mwan3-sync exited with': 'fm160-mwan3-sync 退出码',
 'Remove mwan3 registration?': '移除 mwan3 注册？',
 'All fm160-managed mwan3 interface and member sections are deleted and removed from every policy. Sections you wrote yourself are not touched. The interfaces themselves keep working - only the mwan3 side is unwound.':
   '所有 fm160 托管的 mwan3 interface/member 配置段会被删除并从所有策略移除。你自己写的配置段不受影响。接口本身继续工作——只是撤掉 mwan3 侧。',
 'Remove registration': '移除注册',
 'Registration removed.': '注册已移除。',
 'mwan3 config': 'mwan3 配置',
 'settings changed from mwan page': 'mwan 页修改设置',
 'mwan3 sync': 'mwan3 同步',
 'manual sync from mwan page': 'mwan 页手动同步',
 'wired': '有线',
 'mobile': '移动',
}

# --- extract what the gate extracts --------------------------------------
sites = {}
for d, _, fs in os.walk(res):
    for f in fs:
        if f.endswith('.js'):
            p = os.path.join(d, f)
            got = check.js_literal_calls(io.open(p, encoding='utf-8').read())
            if got:
                sites[os.path.relpath(p, res).replace(os.sep, '/')] = got

menu = json.load(io.open(os.path.join(pkg, 'root', 'usr', 'share', 'luci',
                                      'menu.d', 'luci-app-fm160.json'),
                         encoding='utf-8'))
net = io.open(os.path.join(root, 'fm160d', 'src', 'net.c'), encoding='utf-8').read()
usb = io.open(os.path.join(root, 'fm160d', 'src', 'usbmode.c'), encoding='utf-8').read()

src_of = {}
def ext(src, s, c=None):
    src_of.setdefault((s, c), []).append(src)

for name, calls in sites.items():
    for s, c in calls:
        ext(name, s, c)
for v in menu.values():
    if 'title' in v:
        ext('menu.d', v['title'])
for s in check.daemon_labels(net, 'fm160_net_step_name'):
    ext('fm160d/net.c', s)
for s in check.daemon_labels(usb, 'fm160_usbmode_verdict_text'):
    ext('fm160d/usbmode.c', s)

# --- load the po, split header -------------------------------------------
raw = io.open(po_path, encoding='utf-8').read()
# The header block itself is a msgid "" entry, so split on the first REAL
# entry: a line starting 'msgid "' whose first char after the quote is not
# another quote.
import re
m = re.search(r'^msgid "(?!")', raw, re.M)
header = raw[:m.start()].rstrip() + '\n'

_, entries = check.parse_po(po_path)
old = {(e['msgid'], e['ctxt']): e['msgstr'] for e in entries}

kept = {(s, c): old[(s, c)] for (s, c) in old if (s, c) in src_of}
# Upgrade identity fallbacks once a real translation exists: an entry whose
# msgstr equals its msgid is the "shown as English" state, so replacing it
# with the translated text is always the right move.
for (s, c) in list(kept):
    if kept[(s, c)] == s and s in ZH:
        kept[(s, c)] = ZH[s]
added = {}
for k in src_of:
    if k not in kept:
        s, c = k
        added[k] = ZH.get(s, s)   # untranslated -> identity fallback
for k, v in list(kept.items()):
    if not v:
        kept[k] = ZH.get(k[0], k[0])

all_entries = dict(kept)
all_entries.update(added)

# --- write ----------------------------------------------------------------
def esc(s):
    return check.po_escape(s)

out = [header]
for (s, c) in sorted(all_entries, key=lambda k: (k[0], k[1] or '')):
    refs = ' '.join(sorted(set(src_of.get((s, c), []))))
    out.append('#: %s' % refs)
    if c is not None:
        out.append('msgctxt "%s"' % esc(c))
    out.append('msgid "%s"' % esc(s))
    out.append('msgstr "%s"' % esc(all_entries[(s, c)]))
    out.append('')

io.open(po_path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
ident = sum(1 for v in added.values() if v not in ZH.values())
print('kept %d, added %d (of which %d identity fallback), removed %d'
      % (len(kept), len(added), sum(1 for k in added if added[k] == k[0] and k[0] not in ZH),
         len(old) - len(kept)))
