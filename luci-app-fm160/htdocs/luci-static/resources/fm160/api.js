'use strict';
'require baseclass';
'require rpc';
'require fs';

/*
 * Thin wrapper around the fm160 ubus object.
 *
 * The front end never talks to the AT transport: every read is served from
 * fm160d's cached snapshot, and the only calls that reach the modem are the
 * explicitly user-triggered ones below.
 */

var callStatus   = rpc.declare({ object: 'fm160', method: 'status',   expect: {} });
var callIdentity = rpc.declare({ object: 'fm160', method: 'identity', expect: {} });
var callProfile  = rpc.declare({ object: 'fm160', method: 'profile',  params: [ 'active' ], expect: {} });
var callAt       = rpc.declare({ object: 'fm160', method: 'at',       params: [ 'cmd', 'timeout', 'end_flag' ], expect: {} });
var callRescan   = rpc.declare({ object: 'fm160', method: 'rescan',   expect: {} });
var callIdent    = rpc.declare({ object: 'fm160', method: 'ident',    expect: {} });
var callEnabled  = rpc.declare({ object: 'fm160', method: 'enabled',  params: [ 'enabled' ], expect: {} });

/*
 * M4 write paths.  Both are deferred on the daemon side: the answer only
 * arrives after the write AND a read-back of the same setting have both
 * completed, so the reply means "the modem agrees it is now set to this",
 * not merely "the command was sent".  Do not shorten the caller-side patience
 * for these two - the daemon holds them for up to ~10 s per step.
 */
var callSetBands    = rpc.declare({ object: 'fm160', method: 'setbands',
				    params: [ 'bands' ], expect: {} });
var callSetCellLock = rpc.declare({ object: 'fm160', method: 'setcelllock',
				    params: [ 'mode', 'rat', 'type', 'earfcn', 'pci',
					      'scs', 'nrband' ], expect: {} });

/*
 * M5 write paths, deferred in the same way as M4: the reply arrives only after
 * the write AND a read-back of the same setting have both completed.
 *
 * setgnss is the one write in this whole application with NO capability gate,
 * and that is not an oversight: AT+GTGPSPOWER is the single GNSS setting the
 * module does not store, so it cannot survive a power cycle, cannot be left in
 * an unknown state by a failed write, and both of its values have been observed
 * on hardware.  Nothing can be made worse by trying it.
 *
 * setgnsscfg is the opposite - stored, and gated on AT+GTGPSCFG=?.
 */
var callSetGnss    = rpc.declare({ object: 'fm160', method: 'setgnss',
				   params: [ 'enabled' ], expect: {} });
var callSetGnssCfg = rpc.declare({ object: 'fm160', method: 'setgnsscfg',
				   params: [ 'constellation' ], expect: {} });

/*
 * SMS.
 *
 * Only the send and the sync touch the modem.  Everything else - the list, the
 * deleting, the marking as read - is answered from fm160d's own copy, so
 * opening the page costs no AT traffic at all.
 *
 * "sms_sync" is a button rather than something that happens on a timer, and
 * that is the whole design: AT+CPMS? took 10.3 s on this modem with no card
 * fitted, against 24 ms for AT+CSQ.  A page that polled it would keep the one
 * serial port busy asking a question whose answer has not changed.
 */
var callSmsList   = rpc.declare({ object: 'fm160', method: 'sms_list',     expect: {} });
var callSmsSend   = rpc.declare({ object: 'fm160', method: 'sms_send',
				  params: [ 'number', 'text' ], expect: {} });
var callSmsDelete = rpc.declare({ object: 'fm160', method: 'sms_delete',
				  params: [ 'id' ], expect: {} });
var callSmsRead   = rpc.declare({ object: 'fm160', method: 'sms_markread',
				  params: [ 'id' ], expect: {} });
var callSmsSync   = rpc.declare({ object: 'fm160', method: 'sms_sync',     expect: {} });

/*
 * M2: the data plane, and the USB profile switch.
 *
 * None of these defer.  A dial answers with the daemon's verdict and the link
 * then comes up over the next seconds to minutes, so what a page watches is the
 * "dial" table in the snapshot; a profile change answers once the write has
 * been ACCEPTED, because the module re-enumerates for 30-90 s afterwards and no
 * ubus request should be held open for that.  What a page watches there is the
 * "modesw" table.
 *
 * dial_config is the only call here that writes to uci, and it writes on the
 * daemon's side so that the file and the running configuration cannot drift:
 * the alternative is a page that writes uci and then asks for a reload, with a
 * window in between where the two disagree.  Values are validated before
 * anything is stored, so a malformed APN cannot be saved and then discovered at
 * the next dial.
 */
var callDialStart  = rpc.declare({ object: 'fm160', method: 'dial_start',  expect: {} });
var callDialStop   = rpc.declare({ object: 'fm160', method: 'dial_stop',   expect: {} });
var callDialConfig = rpc.declare({ object: 'fm160', method: 'dial_config',
				   params: [ 'apn', 'pdp', 'cid', 'allow_reset',
					     'autostart' ], expect: {} });
/* On demand, not in the snapshot: fourteen static rows against a snapshot that
 * is rebuilt whenever a sysfs counter moves. */
var callProfiles   = rpc.declare({ object: 'fm160', method: 'profiles',    expect: {} });
var callSetUsbMode = rpc.declare({ object: 'fm160', method: 'setusbmode',
				   params: [ 'mode', 'advanced' ], expect: {} });

/*
 * M6.  The support bundle, as plain text.
 *
 * It is a read: the daemon builds it from the cached snapshot and its own log
 * ring and never touches the modem, so asking for it cannot change the state it
 * is describing.  The reply also carries a few scalars (the byte count, the log
 * line count, how many lines were dropped) so a page can label the download
 * without parsing the text.
 *
 * No params, and deliberately no "since" or "level" filter: the bundle is meant
 * to be the same artefact every time, so that two of them can be compared.
 */
var callDiagnostics = rpc.declare({ object: 'fm160', method: 'diagnostics',
				    expect: {} });

/*
 * USB profiles.
 *
 * The dial-up document carries FOUR port tables, one per platform, and the same
 * mode NUMBER means different things in each of them - mode 19 has an AT port on
 * Qualcomm (2CB7:0x0106) but none at all on 0x05C6:0x9025.  Only 表 1 (Qualcomm,
 * VID 2CB7, PID 010x) describes an FM160, so a switch is only ever offered for a
 * unit that reports exactly that identity.
 *
 * Two independent facts then decide whether a profile may be offered:
 *
 *   has_at   the composite descriptor contains an "AT Device Application
 *            Interface", so an AT port exists after re-enumeration.  A mode
 *            WITHOUT it is a one-way trip: AT+GTUSBMODE can never be sent again.
 *
 *   fm160    the mode is listed in the FM160 AT manual 11.1.2.4.  Modes known
 *            only from the port table are "device dependent" and stay behind an
 *            explicit opt-in.
 *
 * PID is informational: 17 and 32 share 0x0104, and 18 and 33 share 0x0105, so
 * the active profile can NEVER be inferred from VID:PID.  Read it back with
 * AT+GTUSBMODE?.
 */
var USB_PLATFORM = { vid: '2cb7' };

var USB_MODES = {
	17: { pid: '0x0104', kind: 'qmi',  has_at: true,  fm160: true,
	      layout: 'DIAG+MODEM+AT+PIPE+RMNET+ADB' },
	18: { pid: '0x0105', kind: 'ecm',  has_at: true,  fm160: true,
	      layout: 'DIAG+MODEM+AT+PIPE+ECM+ECM+ADB' },
	19: { pid: '0x0106', kind: 'ecm',  has_at: true,  fm160: false,
	      layout: 'DIAG+MODEM+AT+ECM+ECM' },
	20: { pid: '0x0107', kind: 'none', has_at: false, fm160: true,
	      layout: 'MODEM (no AT, no data)' },
	21: { pid: '0x0108', kind: 'none', has_at: true,  fm160: true,
	      layout: 'MODEM+AT (no data)' },
	22: { pid: '0x0109', kind: 'qmi',  has_at: true,  fm160: false,
	      layout: 'MODEM+AT+RMNET' },
	23: { pid: '0x010A', kind: 'ecm',  has_at: true,  fm160: false,
	      layout: 'MODEM+AT+ECM+ECM' },
	24: { pid: '0x010B', kind: 'none', has_at: false, fm160: true,
	      layout: 'RNDIS+RNDIS+MODEM+DIAG+ADB (no AT)' },
	28: { pid: '0x010F', kind: 'none', has_at: false, fm160: false,
	      layout: 'MBIM only (no AT)' },
	29: { pid: '0x0110', kind: 'mbim', has_at: true,  fm160: true,
	      layout: 'MBIM+MBIM+AT+DIAG' },
	30: { pid: '0x0111', kind: 'mbim', has_at: true,  fm160: true,
	      layout: 'MBIM+MBIM+MODEM+DIAG+AT' },
	31: { pid: null,     kind: 'none', has_at: false, fm160: true,
	      layout: 'DIAG+MODEM+RMNET+DPL+QDSS+ADB (no AT)' },
	32: { pid: '0x0104', kind: 'qmi',  has_at: true,  fm160: true,
	      layout: 'DIAG+MODEM+AT+PIPE+RMNET' },
	33: { pid: '0x0105', kind: 'ecm',  has_at: true,  fm160: true,
	      layout: 'DIAG+MODEM+AT+PIPE+ECM+ECM' }
};

/* Every PID the FM160 port table (表 1) lists, derived from USB_MODES so that a
 * mode added above cannot be forgotten here.
 *
 * This used to be a string prefix test -- "0x0104 starts with 010" -- which
 * quietly cut the set short, because 0x0110 (mode 29) and 0x0111 (mode 30) do
 * not start with 010.  The consequence was a lock-out rather than a cosmetic
 * bug: switching to MBIM re-enumerates the modem as 0x0111, usbPlatformKnown()
 * would then answer "not an FM160", candidates() would return nothing, and the
 * QMI profile the user came from could never be offered again.  Enumerate the
 * PIDs instead of guessing at them. */
var FM160_PIDS = (function() {
	var seen = {}, out = [];

	Object.keys(USB_MODES).forEach(function(m) {
		var pid = USB_MODES[m].pid;

		if (!pid || seen[pid])
			return;
		seen[pid] = true;
		out.push(normHex(pid, 4));
	});
	return out;
})();

/* Modes that expose no AT interface.  Switching to one of these removes the
 * only management channel this whole application depends on, and there is no
 * way back from the OS side: the FM160 has no reset-to-default that we can
 * trigger, so recovery is a manual power cycle.
 *
 * 20, 24 and 31 are all present in this unit's AT+GTUSBMODE=? answer.  28 is
 * not reported here but does appear in the vendor port tables, so it stays as
 * a guard for other firmware builds. */
var USB_MODE_BLACKLIST = [ 20, 24, 28, 31 ];

/*
 * Modes whose AT interface the KERNEL will not enumerate, even though the modem
 * does provide one.  This is a different failure from the blacklist above and
 * deserves its own name, because the USB descriptor looks fine -- the AT
 * interface is right there in the port table -- and only the host side is
 * missing.
 *
 * drivers/usb/serial/option.c matches on VID:PID before it looks at anything
 * else, so a PID with no entry means no ttyUSB is created for any interface.
 * For the Fibocom 0x2cb7 family that file carries entries for exactly
 * 0x0104, 0x0105, 0x0106, 0x010a, 0x010b and 0x0111 (checked against the
 * 6.6.127 source that ships with the build tree, and against every
 * target/linux patch in it: nothing adds more).
 *
 *   0x0107 mode 20   absent   0x010f mode 28   absent
 *   0x0108 mode 21   absent   0x0110 mode 29   absent
 *   0x0109 mode 22   absent
 *
 * Of the modes this firmware offers, 21, 22 and 29 are the ones where that
 * matters, because those three DO have an AT interface.  Switching to 29 or 22
 * would strand the modem with no management channel at all -- the exact outcome
 * this project refuses to risk -- while mode 21 would leave the AT port on
 * ttyUSB1 instead of the usual ttyUSB2 and so be reachable only by hand.
 *
 * 22 was missing from this list until the M2 host-side test derived the set
 * from the table instead of trusting it: the comment above had said "0109
 * absent" all along, while the code let 22 through as merely 'advanced', i.e.
 * offered behind a confirmation dialog that does not say the switch is
 * one-way.  Keep this array equal to { mode : has_at && no option.c entry };
 * 25-usbmode-dial-test.sh computes that set and fails if they differ.
 *
 * Clearing this list is a one-line kernel patch per PID; until that ships, the
 * UI must not offer these modes.
 */
var USB_MODE_NO_KERNEL_DRIVER = [ 21, 22, 29 ];

/*
 * Hardware-verified mode set.
 *
 * FM160-CN 89614.1000.00.04.01.02 answered AT+GTUSBMODE=? with
 *     +GTUSBMODE: (17-18,20-21,24,29-33)
 * i.e. exactly { 17, 18, 20, 21, 24, 29, 30, 31, 32, 33 }, and
 * AT+GTUSBMODE? currently reads 32.
 *
 * 19, 22, 23 and 28 are described in the vendor's per-platform port tables but
 * are NOT offered by this firmware.  They stay in USB_MODES only so a modem
 * that does report them gets a label instead of a bare number; they must never
 * be proposed as a switch target.
 */
var USB_MODE_HW = [ 17, 18, 20, 21, 24, 29, 30, 31, 32, 33 ];

/* Preference order per dial kind, restricted to USB_MODE_HW.
 *
 * This firmware offers no NCM, RNDIS or GobiNet target, so only the QMI, ECM
 * and MBIM families are reachable -- which matches the project's scope of
 * "QMI / MBIM / ECM only".
 *
 * MBIM lists 30 only.  29 is the other MBIM mode this firmware offers and it
 * is normally the better-looking one (fewer endpoints), but 0x0110 has no
 * option.c entry, so its AT port does not exist on the host side; see
 * USB_MODE_NO_KERNEL_DRIVER.  Keep this list in step with that one. */
var USB_MODE_PREFERRED = {
	qmi:  [ 32, 17 ],
	mbim: [ 30 ],
	ecm:  [ 33, 18 ]
};

function normHex(v, width) {
	if (v === undefined || v === null)
		return '';
	var s = String(v).toLowerCase().replace(/^0x/, '').replace(/^0+/, '');

	if (!s)
		s = '0';
	return s.padStart(width, '0');
}

/* True only for the platform whose mode table we actually know.
 *
 * The VID alone is not enough -- 0x2cb7 is Fibocom's own VID and also carries
 * FM650-CN (0x0a04-0x0a07), FG132 (0x0112), FM135 (0x0115) and FM101-GL
 * (0x01a2-0x01a4), whose mode numbers mean something else entirely.  Requiring
 * the PID to be one of the FM160 port table's is what makes the mode numbers
 * meaningful.
 *
 * It must accept EVERY FM160 PID, including the ones reached by switching:
 * if it stops recognising the device after a switch, the way back disappears. */
function usbPlatformKnown(idVendor, idProduct) {
	if (!idVendor || !idProduct)
		return false;
	return normHex(idVendor, 4) === USB_PLATFORM.vid &&
	       FM160_PIDS.indexOf(normHex(idProduct, 4)) >= 0;
}

/* Any mode number the modem reports that this table does not describe. */
function usbModeKnown(mode) {
	return Object.prototype.hasOwnProperty.call(USB_MODES, mode);
}

function usbModeHasAt(mode) {
	var e = USB_MODES[mode];
	return e ? e.has_at : false;
}

function usbModeDocumented(mode) {
	var e = USB_MODES[mode];
	return e ? e.fm160 : false;
}

/* Present in this firmware's AT+GTUSBMODE=? answer.  Used as a fallback when
 * the caller has not read the live list yet. */
function usbModeHwSupported(mode) {
	return USB_MODE_HW.indexOf(Number(mode)) >= 0;
}

/* False when the host has no driver for this PID, so the AT port will not
 * appear no matter how healthy the modem is. */
function usbModeKernelReachable(mode) {
	return USB_MODE_NO_KERNEL_DRIVER.indexOf(Number(mode)) < 0;
}

/*
 * usbModeRisk() -> 'safe' | 'advanced' | 'unknown' | 'forbidden'
 *
 * 'forbidden' is the answer for both ways of losing the management channel:
 * a mode with no AT interface at all, and a mode whose AT interface the kernel
 * refuses to enumerate.  They are different faults with the same consequence,
 * and the UI must not offer either.
 *
 * 'advanced'/'unknown' mean: we cannot prove the AT port survives the switch,
 * so the UI must ask for an explicit confirmation first.
 */
function usbModeRisk(mode) {
	if (!usbModeKnown(mode))
		return 'unknown';
	if (!usbModeHasAt(mode))
		return 'forbidden';
	if (!usbModeKernelReachable(mode))
		return 'forbidden';
	return USB_MODES[mode].fm160 ? 'safe' : 'advanced';
}

/* 17/32 and 18/33 are indistinguishable over USB, so the numeric mode can only
 * be confirmed with AT+GTUSBMODE?. */
function usbModePidClash(mode) {
	if (mode === 17 || mode === 32)
		return [ 17, 32 ];
	if (mode === 18 || mode === 33)
		return [ 18, 33 ];
	return null;
}

var REG_TEXT = {
	0:  _('not registered'),
	1:  _('registered (home)'),
	2:  _('searching'),
	3:  _('registration denied'),
	4:  _('unknown'),
	5:  _('registered (roaming)'),
	99: _('unknown')
};

var RAT_TEXT = {
	0:  _('no service'),
	2:  'WCDMA',
	4:  'LTE',
	9:  'NR / 5G'
};

function ratName(rat) {
	return RAT_TEXT[rat] || '?';
}

function regName(stat) {
	return REG_TEXT[stat] || _('unknown');
}

function usbModeLabel(mode) {
	var e = USB_MODES[mode];
	if (!e)
		return 'mode ' + mode + ' ' + _('(not in the known table)');
	return mode + ' - ' + e.layout;
}

function isServiceable(stat) {
	return stat === 1 || stat === 5;
}

/* Sentinel used by fm160d for "the modem did not report this". */
var NONE = -1000000;

/*
 * Every numeric field in the status blob is written with blobmsg_add_u32/u64,
 * so the -1000000 sentinel reaches JavaScript as 4293967296, not as -1000000.
 * A plain `value >= 0` test is therefore true for exactly the values it is
 * meant to reject, and a bare `value` test prints seven-digit rubbish.
 *
 * Both encodings are rejected here, along with a missing key -- the whole point
 * is that no view has to know which field happens to use which encoding.  Real
 * values (dBm, tenths of a dB, PCI, MHz, band numbers) are all far inside the
 * magnitude bound.
 */
var NONE_MAGNITUDE = 500000;

function reported(v) {
	return typeof v === 'number' && v > -NONE_MAGNITUDE && v < NONE_MAGNITUDE;
}

/* A power in dBm, or null.  Legitimate values are always negative. */
function dbm(v) {
	return (reported(v) && v < 0) ? v : null;
}

/* A value carried in tenths of a dB, converted to dB, or null. */
function db10(v) {
	return reported(v) ? v / 10 : null;
}

function csqOf(st)     { return (st && st.csq)     || {}; }
function cesqOf(st)    { return (st && st.cesq)    || {}; }
function trafficOf(st) { return (st && st.traffic) || {}; }

/*
 * AT+CSQ's first field carries RSSI on 2G/3G, but on the 5G path the modem puts
 * SS-RSRP in it instead; is_ss_rsrp says which.  When it is really an RSRP,
 * reporting it as an RSSI would be a wrong number under a right label, so the
 * RSSI accessor refuses and the RSRP accessor takes over.
 */
function isSsRsrp(st) { return !!csqOf(st).is_ss_rsrp; }

function rssiDbm(st) {
	return isSsRsrp(st) ? null : dbm(csqOf(st).rssi_dbm);
}

/* Raw AT+CSQ fields, for the views that show what the modem literally said. */
function csqRawRssi(st) { return csqOf(st).raw_rssi; }
function csqRawBer(st)  { return csqOf(st).raw_ber; }

/*
 * RSRP/RSRQ for the serving RAT: which of the two CESQ columns is the live one
 * follows is_ss_rsrp, and if the preferred column is empty the other is used
 * rather than showing nothing.
 */
function rsrpDbm(st) {
	var c = cesqOf(st), lte = dbm(c.lte_rsrp_dbm), nr = dbm(c.nr_ss_rsrp_dbm);

	return isSsRsrp(st) ? (nr !== null ? nr : lte) : (lte !== null ? lte : nr);
}

function rsrqDb(st) {
	var c = cesqOf(st), lte = db10(c.lte_rsrq_db10), nr = db10(c.nr_ss_rsrq_db10);

	return isSsRsrp(st) ? (nr !== null ? nr : lte) : (lte !== null ? lte : nr);
}

function cesqSinrDb(st) { return db10(cesqOf(st).nr_ss_sinr_db10); }

/* --- one cell row, as published by cell_to_blob() ----------------- */

function cellRsrp(cell)      { return dbm(cell && cell.rsrp_dbm); }
function cellRsrqDb(cell)    { return db10(cell && cell.rsrq_db10); }
function cellSinrDb(cell)    { return db10(cell && cell.sinr_db10); }
function cellBand(cell)      { return (cell && reported(cell.band) && cell.band > 0) ? cell.band : null; }
function cellPci(cell)       { return (cell && reported(cell.pci)) ? cell.pci : null; }
function cellBandwidthMhz(cell) {
	return (cell && reported(cell.bandwidth) && cell.bandwidth > 0) ? cell.bandwidth : null;
}
/* 0 and '' both mean "not reported" for these, and neither is worth printing. */
function cellPlmn(cell)      { return (cell && cell.mcc) ? (cell.mcc + '-' + cell.mnc) : null; }
function cellTac(cell)       { return (cell && cell.tac) ? cell.tac : null; }
function cellCellId(cell)    { return (cell && cell.cellid) ? cell.cellid : null; }
function cellEarfcn(cell)    { return (cell && cell.earfcn) ? cell.earfcn : null; }

function fmtDbm(v) {
	return (dbm(v) === null) ? '-' : (v + ' dBm');
}

/* Values that are transmitted in tenths of a dB. */
function fmtDb10(v) {
	var d = db10(v);

	return (d === null) ? '-' : (d.toFixed(1) + ' dB');
}

function fmtNum(v, unit) {
	return reported(v) ? (v + (unit ? ' ' + unit : '')) : '-';
}

/* NR/LTE bandwidth arrives decoded into MHz by fm160d. */
function fmtBandwidth(cell) {
	var mhz = cellBandwidthMhz(cell);

	return (mhz === null) ? '-' : (mhz + ' MHz');
}

function fmtBand(band) {
	return (band === undefined || band === null || band <= 0) ? '-' : ('B' + band);
}

function fmtBytes(v) {
	var units = [ 'B', 'KiB', 'MiB', 'GiB', 'TiB' ], i = 0, n = Number(v) || 0;

	while (n >= 1024 && i < units.length - 1) {
		n /= 1024;
		i++;
	}
	return (i === 0 ? n : n.toFixed(n >= 100 ? 0 : n >= 10 ? 1 : 2)) + ' ' + units[i];
}

function fmtRate(v) {
	var n = Number(v) || 0;

	if (n < 1000)
		return n + ' B/s';
	if (n < 1000 * 1000)
		return (n / 1024).toFixed(1) + ' KiB/s';
	return (n / 1024 / 1024).toFixed(2) + ' MiB/s';
}

function fmtAge(ms) {
	if (ms === undefined || ms === null)
		return '-';
	if (ms < 1000)
		return ms + ' ms';
	if (ms < 60000)
		return (ms / 1000).toFixed(1) + ' s';
	return Math.floor(ms / 60000) + ' min';
}

/* Health of the AT link, as a level used by the views for colouring. */
function atHealth(state) {
	if (!state || !state.port_found)
		return { level: 'down', text: _('AT port not found') };
	if (state.at_state === 2)
		return { level: 'down', text: _('AT unresponsive - automatic polling stopped') };
	if (state.at_state === 1)
		return { level: 'warn', text: _('AT degraded - polling slowed down') };
	return { level: 'ok', text: _('AT healthy') };
}

/*
 * --- M4: band lock / cell lock / carrier aggregation -----------------
 *
 * Everything below reads the "m4" sub-table of the snapshot.  Two rules run
 * through all of it:
 *
 *   caps.valid   The capability enumerations (AT+GTACT=?, AT+GTCELLLOCK=?)
 *                are the LICENCE to write.  Without them the daemon refuses
 *                the write outright, so a view must disable the control
 *                rather than let the user produce a command that will be
 *                rejected.  Both settings are persistent, so a guess is not
 *                a harmless one.
 *
 *   has_*        The cell-lock trailing fields are optional and the daemon
 *                collapses "absent" to 0 in state while keeping has_pci /
 *                has_scs / has_nrband.  Never render pci without checking
 *                has_pci: 0 is a legal PCI.
 */

function m4Of(st)          { return (st && st.m4) || {}; }
function bandsOf(st)       { return m4Of(st).bands || {}; }
function bandCapsOf(st)    { return m4Of(st).band_caps || {}; }
function celllockOf(st)    { return m4Of(st).celllock || {}; }
function celllockCapsOf(st){ return m4Of(st).celllock_caps || {}; }
function caOf(st)          { return m4Of(st).ca || {}; }

/* The RAT-prefixed band token.  AT+GTACT writes the raw token, not the band
 * number, so the raw string is what a "lock to the serving band" button has
 * to send: 3 is ambiguous, 103 is not. */
function bandRaw(b) { return (b && b.raw) || 0; }

/* Decoded band number, or null when fm160d could not map the token.  A
 * non-zero unknown count in the snapshot is the signal that a firmware changed
 * its encoding, and the right response is to show the raw token, not a guess. */
function bandNo(b) {
	return (b && b.band > 0) ? b.band : null;
}

function bandLabel(b) {
	if (!b)
		return '-';
	var n = bandNo(b);

	if (n === null)
		return _('raw', 'fm160 raw marker') + ' ' + bandRaw(b);
	if (b.rat === 9)
		return 'n' + n;
	return 'B' + n;
}

/* A band entry is only useful for locking if the decoder recognised it. */
function bandUsable(b) { return !!(b && b.raw > 0 && b.band > 0); }

/* Flatten the per-RAT arrays the daemon publishes into one list, keeping the
 * RAT on each entry. */
function bandListOf(g) {
	var out = [];

	if (!g)
		return out;

	[ 'umts', 'lte', 'nr' ].forEach(function(k) {
		(g[k] || []).forEach(function(b) {
			out.push({ raw: b.raw, band: b.band, rat: b.rat, kind: k });
		});
	});
	return out;
}

/* A comma-separated AT+GTACT band list built from entries, deduplicated but
 * otherwise in the order given (the modem does not care about order). */
function bandCsv(entries) {
	var seen = {}, out = [];

	(entries || []).forEach(function(b) {
		var raw = bandRaw(b);

		if (!raw || seen[raw])
			return;
		seen[raw] = true;
		out.push(String(raw));
	});
	return out.join(',');
}

/* Which of the three lockable RATs a band belongs to, as the <rat> value
 * AT+GTCELLLOCK wants: 0 LTE, 1 NR, 2 UMTS. */
function celllockRatOf(band_rat) {
	if (band_rat === 9)
		return 1;
	if (band_rat === 2)
		return 2;
	return 0;
}

var CELLLOCK_RAT_TEXT = { 0: 'LTE', 1: 'NR', 2: 'UMTS' };

function celllockRatName(rat) { return CELLLOCK_RAT_TEXT[rat] || ('RAT ' + rat); }

/* 'lock PCI' vs 'lock frequency' - <type>. */
var CELLLOCK_TYPE_TEXT = { 0: _('by PCI'), 1: _('by frequency') };

function celllockTypeName(t) { return CELLLOCK_TYPE_TEXT[t] || ('type ' + t); }

function celllockEarfcn(l) {
	/* earfcn travels as u64 and 0 is a legal value, so it is shown whenever
	 * the lock is on rather than filtered through reported(). */
	return (l && l.earfcn) ? l.earfcn : null;
}

function celllockPci(l) {
	return (l && l.has_pci && reported(l.pci)) ? l.pci : null;
}

function celllockNrband(l) {
	return (l && l.has_nrband && reported(l.nrband)) ? l.nrband : null;
}

function celllockScs(l) {
	return (l && l.has_scs && reported(l.scs)) ? l.scs : null;
}

/* The value AT+GTCELLLOCK=? advertised beyond what the manual defines.  Shown
 * as a note, never offered as a choice. */
function celllockModeUndocumented(st) {
	return !!celllockCapsOf(st).mode_undocumented;
}

/* SCC modulation arrives as the raw code; 6 means "no data". */
var CA_MOD_TEXT = {
	0: 'BPSK', 1: 'QPSK', 2: '16QAM', 3: '64QAM', 4: '256QAM', 5: '1024QAM'
};

function caModName(code) {
	return (code === undefined || code === null) ? '-' : (CA_MOD_TEXT[code] || _('unknown'));
}

function caStateName(s) {
	return s === 2 ? _('activated') : (s === 1 ? _('configured') : '-');
}

/* Highest MCS a cell reports, for a one-line summary. */
function caCellSummary(c) {
	if (!c)
		return '-';
	return fmtBand(c.band) + ' / ' + (c.dl_bw_mhz ? c.dl_bw_mhz + ' MHz' : '-') +
	       ' / ' + (c.dl_mimo ? c.dl_mimo + 'x' + c.dl_mimo : '-') +
	       ' / ' + caModName(c.dl_mod);
}

/*
 * --- M5: GNSS ---------------------------------------------------------
 *
 * Three measured facts shape everything below, and none of them is what a
 * reasonable person would assume:
 *
 *   The NMEA block comes out of the AT PORT.  USB mode 17/32 exposes no GNSS
 *   interface and the module emits no URC for it - AT+GTGPS? returns the
 *   sentences as its ordinary command response.  So there is nothing to
 *   connect, no port to lease, and nothing a page can do to make the polling
 *   cheaper or noisier than the daemon already has it.
 *
 *   "Engine on, 0 in view, no fix" is the NORMAL state.  It is what an indoor
 *   receiver reports, and it must be displayed as a plain fact rather than
 *   dressed up as a fault.  The measured no-fix frame carries no RMC and no
 *   GGA at all - only GSA and GSV - so "NMEA is healthy" cannot be defined as
 *   "an RMC arrived".
 *
 *   An empty answer is not a failure either.  Immediately after the engine is
 *   switched on, AT+GTGPS? answers OK with the header and not one sentence.
 *   read.empty_frames counts those in a row: one or two is the normal startup,
 *   a climbing count means the engine claims to be on and never speaks.
 *
 * Two more rules, carried over from M4 and unchanged here:
 *
 *   The daemon writes every number with blobmsg_add_u32, so the -1000000
 *   sentinel reaches JavaScript as 4293967296; api.reported() is the filter.
 *   Zero is NOT a sentinel - "0 satellites in view" is a measurement, while an
 *   absent elevation is the absence of one.
 *
 *   The capability answer is the licence to write, and it is answered one field
 *   at a time: AT+GTGPSCFG=? returns a line per x, each with its own value set
 *   (measured 2026-09-19 - x=0 is (0-2), x=2 is (0-15), x=3 is (0,1), x=1
 *   absent).  fm160d keeps them apart and licenses the constellation write from
 *   the x=2 set alone, so config_caps.values is exactly that set rather than a
 *   union of the three.  A page that ignores config_caps.valid offers buttons
 *   the daemon will refuse.
 */

function gnssOf(st)        { return (st && st.gnss) || {}; }
function gnssEngineOf(st)  { return gnssOf(st).engine  || {}; }
function gnssReadOf(st)    { return gnssOf(st).read    || {}; }
function gnssFixOf(st)     { return gnssOf(st).fix     || {}; }
function gnssCfgOf(st)     { return gnssOf(st).config  || {}; }
function gnssCfgCapsOf(st) { return gnssOf(st).config_caps || {}; }
/*
 * The per-field value sets as the modem reported them: [{ x, values }, ...].
 * x reads as the field number in AT+GTGPSCFG=x,<v>; the daemon's slot for a
 * group that arrived with no field name is FM160_GNSS_CFG_X_NONE, which it
 * publishes as 4 so that a page can tell it apart from a real field.
 */
function gnssCfgCapsByX(st) { return gnssCfgCapsOf(st).by_x || []; }

/* Which field AT+GTGPSCFG=2,<v> writes, i.e. the one config_caps.values is
 * about.  Taken from the snapshot rather than hardcoded, so the page and the
 * daemon cannot disagree if the daemon's target ever changes. */
function gnssCfgWriteX(st) {
	var v = gnssCfgCapsOf(st).write_x;

	return (typeof v === 'number') ? v : 2;
}
function gnssAgpsOf(st)    { return gnssOf(st).agps    || {}; }
function gnssConstsOf(st)  { return gnssOf(st).constellations || []; }
function gnssSatsOf(st)    { return gnssOf(st).satellites || []; }

/* Constellation codes as enum fm160_gnss_kind publishes them. */
var GNSS_KIND_TEXT = {
	0: _('other'), 1: 'GPS', 2: _('BeiDou'),
	3: 'GLONASS', 4: 'Galileo', 5: 'QZSS'
};

function gnssKindName(kind) { return GNSS_KIND_TEXT[kind] || _('other'); }

/* The talker is what the module actually sent ("GP", "BD", "PQ", "GL", "GA");
 * the kind is our reading of it.  An unrecognised talker decodes to kind 0,
 * and there the talker is the fact and every name would be a guess - so both
 * are shown, and the raw one is not thrown away. */
function gnssConstName(c) {
	if (!c)
		return '-';
	if (c.kind === 0)
		return c.talker + ' (' + _('unknown') + ')';
	return gnssKindName(c.kind);
}

/*
 * The one line that answers "is the GNSS working?" without lying either way.
 *
 * Ordered by what the user can act on.  No AT link and a switched-off engine
 * are decisions, an empty answer right after switching on is startup, and zero
 * satellites indoors is physics.  A fix is only claimed when the daemon has a
 * position: it sets has_position only from a GGA/RMC whose status is 'A', so a
 * "void" sentence carrying the receiver's last known position can never light
 * this up as if it were live.
 */
function gnssStateText(st) {
	var e = gnssEngineOf(st), r = gnssReadOf(st), f = gnssFixOf(st);

	if (!e.known)
		return _('engine state not read yet');
	if (!e.on)
		return _('engine off');
	if (r.read_error)
		return _('the module answers ERROR - the engine is not really on');
	if (f.has_position)
		return _('fix acquired');
	if (r.empty_frames > 1)
		return _('engine on, but no sentence yet') + ' (' + r.empty_frames +
		       ' ' + _('empty answers in a row') + ')';
	if (r.empty_frames === 1)
		return _('engine on, still starting up') + ' (' + _('one empty answer') + ')';
	/* "not reported" and "0" are different answers and must not be folded into
	 * one sentence.  0 is a measurement - and the normal one indoors - whereas
	 * the sentinel means nothing has been read yet, which is the real state in
	 * the window between switching the engine on and the first NMEA answer
	 * arriving.  Saying "0 satellites in view" there would be asserting a fact
	 * about the sky that nothing has looked at. */
	if (!reported(f.visible))
		return _('engine on, waiting for the first reading');
	if (f.visible === 0)
		return _('engine on, 0 satellites in view - normal indoors');
	return _('engine on') + ', ' + f.visible + ' ' + _('in view, no fix yet');
}

/* A level the views can colour by.  'off' and 'starting' are not faults. */
function gnssLevel(st) {
	var e = gnssEngineOf(st), r = gnssReadOf(st), f = gnssFixOf(st);

	if (!e.known || !e.on || r.read_error)
		return 'down';
	if (f.has_position)
		return 'ok';
	if (r.empty_frames > 1)
		return 'warn';
	return 'off';
}

/* raw NMEA of the last block that had sentences, for the debug pane. */
function gnssRaw(st) { return gnssReadOf(st).raw || ''; }

/*
 * u32 -> the int32 the daemon meant.
 *
 * Every signed field makes this round trip, and lat_1e7/lon_1e7 are the two
 * where it changes the answer rather than the formatting: a southern latitude
 * or a western longitude arrives as a large positive number, which rendered
 * naively would put the user somewhere in the Pacific.
 */
function toSigned(v) {
	var n = Number(v) || 0;

	return n > 2147483647 ? n - 4294967296 : n;
}

/*
 * Degrees, from the x1e7 integers, or null.
 *
 * has_position is the ONLY valid gate here.  The daemon deliberately does not
 * use a sentinel for these two, because -1000000 is a legal scaled coordinate
 * (-0.1 deg, in the Gulf of Guinea) - so "0,0" is a real place and a missing
 * fix has to be recognised some other way.
 */
function gnssCoord(st) {
	var f = gnssFixOf(st);

	if (!f.has_position)
		return null;
	return { lat: toSigned(f.lat_1e7) / 1e7, lon: toSigned(f.lon_1e7) / 1e7 };
}

function fmtCoord(v) {
	return (v === null || v === undefined) ? '-' : v.toFixed(6);
}

/* The hemisphere letters are only sent when they were present, so they are
 * appended rather than assumed. */
function fmtLat(st) {
	var c = gnssCoord(st), f = gnssFixOf(st);

	return c ? (fmtCoord(c.lat) + ' ' + (f.lat_ns || '')) : '-';
}

function fmtLon(st) {
	var c = gnssCoord(st), f = gnssFixOf(st);

	return c ? (fmtCoord(c.lon) + ' ' + (f.lon_ew || '')) : '-';
}

/*
 * AT+GTGPSCFG x=2 - the satellite combination.
 *
 * These are the values the FM160 AT manual documents.  1 is NOT among them:
 * the manual's list skips it, which is why fm160d's builder refuses it, and
 * offering it here would produce a control that always fails.  14 is this
 * module's value - all five constellations.
 */
var GNSS_CFG_VALUES = {
	0:  'GPS + GLONASS',
	2:  'GPS + Galileo',
	3:  'GPS + QZSS',
	4:  'GPS + BeiDou + Galileo',
	5:  'GPS + BeiDou + GLONASS',
	6:  'GPS + BeiDou + QZSS',
	7:  'GPS + GLONASS + Galileo',
	14: _('all five') + ' (GPS + BeiDou + Galileo + GLONASS + QZSS)',
	15: 'GPS'
};

function gnssCfgLabel(v) {
	return GNSS_CFG_VALUES[v] || (_('undocumented combination') + ' ' + v);
}

/*
 * What may be WRITTEN, which is much less than what may be read.
 *
 * Two gates, narrowest wins:
 *   - the documented set above, because the manual is the only source for what
 *     the number means;
 *   - the modem's own AT+GTGPSCFG=? answer, because fm160d checks exactly that
 *     before it will send anything.
 *
 * An empty list means "do not offer the control", not "offer everything":
 * either the answer never arrived, or the list it produced contains nothing we
 * can name.  Guessing a value here would mean writing an unknown satellite
 * combination into a setting that survives a power cycle.
 */
function gnssCfgChoices(st) {
	var caps = gnssCfgCapsOf(st);

	if (!caps.valid)
		return [];
	return (caps.values || []).filter(function(v) {
		return Object.prototype.hasOwnProperty.call(GNSS_CFG_VALUES, v);
	}).sort(function(a, b) { return a - b; });
}

var GNSS_FIX_TYPE_TEXT = {
	0: _('none'), 1: _('no fix'), 2: '2D', 3: '3D'
};

function gnssFixTypeName(t) { return GNSS_FIX_TYPE_TEXT[t] || ('type ' + t); }

/* GGA <quality>. */
var GNSS_QUALITY_TEXT = {
	0: _('invalid'), 1: 'GPS', 2: 'DGPS', 3: 'PPS',
	4: _('RTK fixed'), 5: _('RTK float'), 6: _('estimated')
};

function gnssQualityName(q) { return GNSS_QUALITY_TEXT[q] || ('quality ' + q); }

/* Dilution of precision, stored in tenths.  Absent prints '-' rather than a
 * number, because a DOP the modem did not report is not a DOP of zero. */
function gnssDop(v) { return reported(v) ? (v / 10).toFixed(1) : '-'; }

function gnssAltM(st)  { var v = gnssFixOf(st).alt_dm;   return reported(v) ? (v / 10) : null; }
function gnssSpeedKmh(st) {
	var v = gnssFixOf(st).speed_cmps;

	return reported(v) ? (v * 0.036) : null;
}

var GNSS_EPO_TEXT = { 0: _('off', 'fm160 off state'), 1: 'MSB', 2: 'MSA' };

function gnssEpoName(v) { return GNSS_EPO_TEXT[v] || ('EPO ' + v); }

/*
 * --- SMS ---------------------------------------------------------------
 *
 * smsOf() returns the section of the snapshot.  `usable` in it is the licence
 * for the whole feature: it means the modem accepted PDU mode, a storage, and
 * the new-message indication.  A page that offers to send while usable is false
 * is offering a command the daemon will refuse and the modem has already
 * refused once - so the page asks first.
 */
function smsOf(st) { return (st && st.sms) || {}; }

function smsUsable(st) { return !!smsOf(st).usable; }

function smsStorageOf(st) {
	var s = smsOf(st);

	return {
		name: s.storage || '',
		used: reported(s.used) ? s.used : null,
		total: reported(s.total) ? s.total : null,
		probed: !!s.probed
	};
}

function smsCounterOf(st, key) {
	var c = smsOf(st).counters || {};

	return reported(c[key]) ? c[key] : null;
}

/*
 * A message's time stamp, exactly as the SMSC wrote it.
 *
 * Deliberately NOT converted into the router's clock.  A message the network
 * stamped 09:31 was sent at 09:31; rendering it as something else because the
 * router keeps a different time zone would be inventing a fact, and the offset
 * the stamp arrived with is shown next to it so the reader can do the sum if
 * they want to.  The year is two digits because that is all a PDU carries -
 * padding it out to four would be a guess about the century.
 */
function smsTimestamp(m) {
	function p(n) { return (n < 10 ? '0' : '') + n; }
	var mins, sign;

	if (!m || !m.has_time)
		return null;
	mins = (m.tz_quarters || 0) * 15;
	sign = m.tz_negative ? '-' : '+';
	return {
		text: p(m.year) + '-' + p(m.month) + '-' + p(m.day) + ' ' +
		      p(m.hour) + ':' + p(m.min) + ':' + p(m.sec),
		zone: 'UTC' + sign + Math.floor(mins / 60) +
		      (mins % 60 ? ':' + p(mins % 60) : '')
	};
}

function smsAgeText(m) {
	if (!m || !reported(m.age_ms))
		return '';
	if (m.age_ms < 60000)
		return Math.round(m.age_ms / 1000) + 's';
	if (m.age_ms < 3600000)
		return Math.round(m.age_ms / 60000) + 'm';
	return Math.round(m.age_ms / 3600000) + 'h';
}

var SMS_ENCODING_TEXT = { 0: 'GSM 7-bit', 1: 'UCS2', 2: '8-bit' };

function smsEncodingName(v) { return SMS_ENCODING_TEXT[v] || _('unknown'); }

/* "2 of 3" for a message that arrived in pieces, empty otherwise. */
function smsPartsText(m) {
	if (!m || !m.concat || !m.parts)
		return '';
	return m.part + ' / ' + m.parts;
}

/*
 * How much of the send budget a message used, as a sentence the page can show
 * before the user presses send.  Mirrors sms_pdu_plan() in pdu.c: 160 septets
 * or 70 characters in one piece, 153 or 67 in each piece after that.
 */
function smsEstimate(text, maxParts) {
	var i, chars = 0, septets = 0, gsm7 = true, n;
	var ext = '^{}\\[\\]~|';

	if (!text)
		return { chars: 0, septets: 0, parts: 0, encoding: _('empty') };

	for (i = 0; i < text.length; i++) {
		var c = text.charCodeAt(i);

		chars++;
		if (c > 0x7F) {
			/* Anything outside ASCII lands outside the GSM alphabet
			 * as far as this estimate is concerned.  The real answer
			 * comes from the daemon; this is a preview. */
			gsm7 = false;
			continue;
		}
		septets += ext.indexOf(text.charAt(i)) >= 0 ? 2 : 1;
	}
	if (!gsm7)
		septets = 0;

	n = gsm7
		? (septets <= 160 ? 1 : Math.ceil(septets / 153))
		: (chars <= 70 ? 1 : Math.ceil(chars / 67));
	if (maxParts && n > maxParts)
		n = maxParts + 1;      /* "more than we will send" */

	return {
		chars: chars,
		septets: septets,
		parts: n,
		encoding: gsm7 ? 'GSM 7-bit' : 'UCS2',
		gsm7: gsm7
	};
}

/*
 * --- M2: the data plane ------------------------------------------------
 *
 * Two tables in the snapshot, kept apart because they fail differently.  "dial"
 * describes how the link is dialled; "modesw" describes which physical USB
 * profile the modem is in.  Only the second can take the management channel
 * away, and that is why nothing below mixes them.
 *
 * Three rules shape the page that reads this:
 *
 *   1. fm160d dials ECM and only ECM.  A QMI or MBIM profile is dialled by the
 *      kernel stack (uqmi / umbim) through netifd, and fm160_dial_start()
 *      answers -ENOTSUP - "wrong-profile" - rather than pretending otherwise.
 *      A page that offered Connect on a QMI profile would be offering a button
 *      that can only fail.
 *
 *   2. Disconnect is AT+GTWWAN=0,<cid> and NOTHING else.  Unplugging the cable,
 *      power-cycling the module and rebooting the router all leave the PDP
 *      context active on the network side; that is the vendor's own red line
 *      and the reason the page says so next to the button.
 *
 *   3. The verdict on a profile is the DAEMON's, not this file's.  fm160.profiles
 *      returns each row already decided by fm160_usbmode_decide() - the same
 *      function that guards the write - so a greyed-out row and a refusal
 *      cannot disagree.  What is derived here is only the CATEGORY, for
 *      grouping and colour.
 */

/* Mirrors enum fm160_net_step.  Used to name a state in prose, never to decide
 * anything: every branch that acts on the step reads the string the daemon
 * sent. */
var NET_STEP = {
	IDLE: 0, PIN: 1, REG: 2, APN: 3, ACTIVATE: 4,
	IP: 5, UP: 6, BUSY: 7, FAILED: 8, DOWN: 9
};

/* Mirrors enum fm160_modesw_state. */
var MODESW = {
	IDLE: 0, CAPS: 1, APPLY: 2, VERIFY: 3, STUCK: 4
};

/* Mirrors enum fm160_usbmode_verdict.  The numbers are part of the wire
 * contract with fm160d, so they are written out rather than inferred. */
var USB_VERDICT = {
	OK: 0, OK_SAME: 1,
	DENY_UNKNOWN: 2, DENY_NO_AT: 3, DENY_NO_DRIVER: 4,
	DENY_UNDOCUMENTED: 5, DENY_NOT_SUPPORTED: 6, DENY_LIST_UNKNOWN: 7
};

function dialOf(st)   { return (st && st.dial)   || {}; }
function modeswOf(st) { return (st && st.modesw) || {}; }

function dialUp(st)          { return !!dialOf(st).up; }
function dialStep(st)        { return dialOf(st).step || 'unknown'; }
function dialStepRaw(st)     { return dialOf(st).step_raw; }
function dialUsable(st)      { return !!dialOf(st).usable; }
function dialKind(st)        { return dialOf(st).kind || 'none'; }
function dialKindKnown(st)   { return !!dialOf(st).kind_known; }
function dialApn(st)         { return dialOf(st).apn || ''; }
function dialAddress(st)     { return dialOf(st).address || ''; }
function dialCid(st)         { return dialOf(st).cid; }
function dialPdp(st)         { return dialOf(st).pdp || 'IP'; }
function dialPdpInUse(st)    { return dialOf(st).pdp_in_use || ''; }
function dialAttempt(st)     { return dialOf(st).attempt; }
function dialRunning(st)     { return !!dialOf(st).running; }
function dialSimReady(st)    { return !!dialOf(st).sim_ready; }
function dialConfigError(st) { return !!dialOf(st).config_error; }
function dialStoppedHealing(st) { return !!dialOf(st).healing_stopped; }

/* True while the daemon is actively working on the link.
 *
 * `wanted` is the user's intent and `up` the result, so the pair says "a dial
 * is in progress".  FAILED is excluded because a failed attempt is waiting out
 * its backoff, not working - the step name and the next-try delay say more
 * there than a spinner would.  This is what the dial page holds the daemon's
 * foreground for, and nothing else is: the foreground also switches on the cell
 * polling tier, which that page has no use for. */
function dialWorking(st) {
	var d = dialOf(st);

	return !!d.wanted && !d.up && d.step_raw !== NET_STEP.FAILED;
}

/* null means "not probed yet", which is a fact about the unit rather than about
 * either verb: the vendor's AT manual says ECM/RMNET use +GTWWAN and only RNDIS
 * uses +GTRNDIS, while the same vendor's dial-up document shows AT+GTRNDIS=1,1
 * in its ECM chapter.  fm160d probes and caches; the page only reports. */
function dialVerb(st) {
	var d = dialOf(st);

	return d.verb_known ? (d.verb || '') : null;
}

function dialResets(st) {
	var r = dialOf(st).resets || {};

	return {
		inWindow: reported(r.in_window) ? r.in_window : null,
		limit:    reported(r.limit)     ? r.limit     : null,
		windowS:  reported(r.window_s)  ? r.window_s  : null,
		total:    reported(r.total)     ? r.total     : null
	};
}

/* The reset ledger as one sentence: how many of the allowance are used inside
 * the rolling window, and how many have ever been used.  Both matter - the
 * first is why the daemon may soon stop healing by itself, the second is what
 * the module has actually been through. */
function dialResetText(st) {
	var r = dialResets(st);

	if (r.limit === null || r.inWindow === null)
		return '';
	return r.inWindow + ' / ' + r.limit + ' ' + _('in the last') + ' ' +
	       Math.round((r.windowS || 0) / 3600) + ' h' +
	       (r.total ? ', ' + r.total + ' ' + _('total') : '');
}

/* fm160d's own dialler drives ECM only; QMI and MBIM belong to the kernel. */
function dialIsDaemons(kind) { return kind === 'ecm'; }

/*
 * The link, in one line, ordered by what the user can act on.
 *
 * A refusal that has not been attempted yet comes first (no APN), then the
 * routing decision (this profile is not ours to dial), then the failure, then
 * the progress.  "idle" is not an error and is not dressed as one.
 *
 * NOTE ON THE TWO KINDS OF TRANSLATABLE STRING
 *
 * This file, like every view, contains strings written here - and it also
 * RENDERS strings that arrived from the daemon: `d.step` is "waiting for
 * registration", `verdict_text` is "that profile has no AT interface".  Those
 * are labels the daemon publishes as part of its state machine, so they are
 * translated here by being passed through _() at the point of display; the
 * catalog carries them as hand-written msgids because no extractor can see a
 * value that only exists at run time.
 *
 * The line is drawn at LABELS versus DIAGNOSTICS.  A step name and a refusal
 * reason are vocabulary and get translated.  `last_error` is free-form
 * diagnostic text that names the specific command, profile number or AT error
 * code that failed, and it is shown exactly as the daemon wrote it - a
 * half-translated error is worse than an English one, and the reader of it is
 * usually looking it up.
 */
function dialStateText(st) {
	var d = dialOf(st);

	if (d.config_error)
		return _('refused: no usable APN is configured');
	if (!d.kind_known)
		return _('the USB profile has not been read yet');
	if (!dialIsDaemons(d.kind))
		return _('this profile is dialled by the kernel, not by fm160d');
	if (d.step_raw === NET_STEP.FAILED)
		return _('failed') + (d.last_error ? ': ' + d.last_error : '');
	if (d.up)
		return _('connected');
	return _(d.step);
}

function dialLevel(st) {
	var d = dialOf(st);

	if (d.up)
		return 'ok';
	if (d.config_error || d.step_raw === NET_STEP.FAILED)
		return 'bad';
	if (!dialIsDaemons(d.kind) || d.step_raw === NET_STEP.IDLE ||
	    d.step_raw === NET_STEP.DOWN)
		return 'off';
	return 'warn';
}

/* --- the USB profile switch ------------------------------------------- */

function modeswStateRaw(st) { return modeswOf(st).state_raw; }
function modeswPending(st)  { return !!modeswOf(st).pending; }
function modeswBusy(st)     { return !!modeswOf(st).busy; }

/* null until AT+GTUSBMODE? has answered.  It really can be unknown: 17 and 32
 * share a PID and so do 18 and 33, so the number cannot be inferred from
 * VID:PID and has to be read back. */
function modeswCurrent(st) {
	var m = modeswOf(st);

	return m.current_known ? m.current : null;
}

function modeswRollback(st) {
	var m = modeswOf(st);

	return reported(m.rollback_mode) && m.rollback_mode >= 0 ? m.rollback_mode : null;
}

function modeswTarget(st) {
	var m = modeswOf(st);

	return reported(m.target) && m.target >= 0 ? m.target : null;
}

/* The state machine is blocked on a module that never came back.  This is the
 * one state a page must shout about: switching again is refused, and the way
 * out is a hand on the hardware. */
function modeswStuck(st) { return modeswStateRaw(st) === MODESW.STUCK; }

function modeswStateText(st) {
	var m = modeswOf(st);

	if (m.state_raw === MODESW.STUCK)
		return _('the module has not answered since the switch');
	if (m.pending)
		return _('the module is restarting in profile') + ' ' + modeswTarget(st);
	if (m.rolled_back)
		return _('the switch was rolled back');
	if (modeswCurrent(st) === null)
		return _('profile not read yet');
	return _('in profile') + ' ' + modeswCurrent(st);
}

/* The category a verdict falls into, for grouping and colour.  Derived here
 * because the page needs three buckets, NOT because the decision is ours. */
function usbVerdictCategory(v) {
	switch (v) {
	case USB_VERDICT.OK:               return 'ok';
	case USB_VERDICT.OK_SAME:          return 'same';
	case USB_VERDICT.DENY_NO_AT:       return 'no-at';
	case USB_VERDICT.DENY_NO_DRIVER:   return 'no-driver';
	case USB_VERDICT.DENY_UNDOCUMENTED:return 'undocumented';
	case USB_VERDICT.DENY_NOT_SUPPORTED: return 'unsupported';
	case USB_VERDICT.DENY_LIST_UNKNOWN: return 'list-unknown';
	}
	return 'unknown';
}

/* One row of the fm160.profiles answer, shaped for a table.  `layout` comes
 * from the daemon rather than from USB_MODES above, so the text a user reads is
 * the text the daemon's own decision was made about.
 *
 * `selectable` and `selectableAdvanced` are the daemon's two answers to "may
 * this be switched to": the first without an opt-in, the second with one.
 * `needsOptin` is the difference, and it is the ONLY reason the page ever asks
 * the daemon for an advanced switch. */
function profileRows(res) {
	return ((res && res.profiles) || []).map(function(p) {
		return {
			mode: p.mode,
			pid: p.pid || '',
			kind: p.kind || 'none',
			layout: p.layout || '',
			hasAt: !!p.has_at,
			documented: !!p.documented,
			kernelEntry: !!p.kernel_entry,
			blacklisted: !!p.blacklisted,
			current: !!p.current,
			verdict: p.verdict,
			verdictText: p.verdict_text || '',
			verdictKind: usbVerdictCategory(p.verdict),
			selectable: !!p.selectable,
			selectableAdvanced: !!p.selectable_advanced,
			needsOptin: !p.selectable && !!p.selectable_advanced
		};
	});
}

/* Rows that may be offered as a target right now.  The current profile is not a
 * choice, so it is separated out rather than greyed in place; `advanced` admits
 * the device-dependent profiles, which is what the page's opt-in checkbox sets. */
function profileChoices(rows, advanced) {
	return (rows || []).filter(function(p) {
		return !p.current && (p.selectable ||
		       (advanced && p.selectableAdvanced));
	});
}

/* Rows that cannot be chosen, with the reason the daemon gave.  Shown rather
 * than hidden: "the modem offers 24, and we refuse it, because it has no AT
 * interface" is a more useful thing to read than a profile that is simply
 * missing from the list. */
function profileRefused(rows, advanced) {
	return (rows || []).filter(function(p) {
		return !p.current && p.verdictKind !== 'same' &&
		       !(p.selectable || (advanced && p.selectableAdvanced));
	});
}


/* ---------------------------------------------------------------------------
 * Radio cross-check queries (AT manual §5.8/5.15/5.17/5.20/8.22).
 *
 * These run live AT commands through fm160d's single AT owner, so they are
 * only issued from pages the user has open, on a 5 s cadence, and every
 * result is parsed defensively: the manual's bracketed fields are optional
 * and the module omits whole blocks (e.g. QCI on this firmware) when it has
 * nothing to report.
 * ------------------------------------------------------------------------ */

/* +GTCCINFO? -> LTE service cell line:
 * svc,rat,mcc,mnc,tac,cellid,earfcn,pci,band,bw,rssnr,rxlev,rsrp,rsrq
 * NR service cell: same with narfcn + ss-* fields. */
function parseCcinfo(text) {
	var m = /(?:^|\n)(?:LTE|NR)[^\n]*service cell:\s*\r?\n?([0-9a-fA-F,]+)/.exec(text || '');
	if (!m) return null;
	var f = m[1].split(',');
	if (f.length < 14) return null;
	var rat = parseInt(f[1], 10);
	return {
		rat:    rat,
		ratName: ({4:'LTE',5:'eMTC',6:'NB-IoT'})[rat] || (rat >= 4 ? 'LTE' : 'RAT' + rat),
		nr:     /NR/i.test(m[0]),
		mcc:    f[2], mnc: f[3],
		tac:    f[4], cellid: f[5],
		arfcn:  parseInt(f[6], 16) || 0, pci: parseInt(f[7], 16) || 0,
		band:   f[8], bwCode: parseInt(f[9], 10),
		snr:    parseInt(f[10], 10), rxlev: parseInt(f[11], 10),
		rsrp:   parseInt(f[12], 10), rsrq: parseInt(f[13], 10)
	};
}

/* +GTCAINFO? -> "PCC: b,pci,arfcn,dlbw,dlm,ulm,dlmod,ulmod,rsrp" plus
 * optional "SCCn: state,ulcfg,band,pci,arfcn,dlbw,ulbw,dlm,ulm,dlmod,ulmod,rsrp".
 * Band codes: LTE 101->B1..171->B71; NR 501->n1..509->n9, 5010->n10.. */
function caBandName(code) {
	code = parseInt(code, 10);
	if (code >= 101 && code <= 171) return 'B' + (code - 100);
	if (code >= 501 && code <= 509) return 'n' + (code - 500);
	if (code >= 5010) return 'n' + (code - 5000);
	return String(code);
}

function caBwMHz(code) {
	code = parseInt(code, 10);
	var lte = {6:'1.4',15:'3',25:'5',50:'10',75:'15',100:'20'};
	return lte[code] || (code > 0 ? code : null);   /* NR 直接报 MHz */
}

function parseCainfo(text) {
	var out = { pcc: null, scc: [] };
	var re = /(PCC|SCC\d+):\s*([0-9a-fA-F,]+)/g, m;
	while ((m = re.exec(text || '')) !== null) {
		var f = m[2].split(',');
		if (m[1] === 'PCC' && f.length >= 9) {
			out.pcc = {
				band: caBandName(f[0]), pci: parseInt(f[1], 16) || 0,
				arfcn: parseInt(f[2], 16) || 0, bw: caBwMHz(f[3]),
				dlMimo: f[4], ulMimo: f[5],
				dlMod: caModName(f[6]), ulMod: caModName(f[7]),
				rsrp: parseInt(f[8], 10)
			};
		} else if (m[1] !== 'PCC' && f.length >= 12) {
			out.scc.push({
				id: m[1],
				state: f[0] === '2' ? 'active' : (f[0] === '1' ? 'configured' : f[0]),
				ulCa: f[1] === '1',
				band: caBandName(f[2]), pci: parseInt(f[3], 16) || 0,
				arfcn: parseInt(f[4], 16) || 0, dlBw: caBwMHz(f[5]), ulBw: caBwMHz(f[6]),
				dlMimo: f[7], ulMimo: f[8],
				dlMod: caModName(f[9]), ulMod: caModName(f[10]),
				rsrp: parseInt(f[11], 10)
			});
		}
	}
	return out;
}

function caModName(code) {
	return ({0:'BPSK',1:'QPSK',2:'16QAM',3:'64QAM',4:'256QAM',5:'1024QAM',6:'?'})[String(parseInt(code,10))] || code;
}

/* +GTCELLINFO? -> mode + [LTE: CQI/Power/RANK/DLMCS/ULMCS [QCI]] [NR5G: ...] */
function parseCellinfo(text) {
	var out = { mode: null, lte: null, nr: null };
	var m = /\+GTCELLINFO:\s*(\d+)/.exec(text || '');
	if (m) out.mode = parseInt(m[1], 10);
	var lte = /LTE:\s*\r?\n?CQI:\s*(\d+)\s*\r?\n?Power:\s*([-\d]+)\s*\r?\n?RANK:\s*(\S+)\s*\r?\n?DLMCS:\s*(\d+)\s*\r?\n?ULMCS:\s*(\d+)(?:\s*\r?\n?TX_LTE_QCI:\s*(\d+)\s*\r?\n?RX_LTE_QCI:\s*(\d+))?/.exec(text || '');
	if (lte) out.lte = { cqi:+lte[1], power:+lte[2], rank:lte[3], dlmcs:+lte[4], ulmcs:+lte[5], txQci:lte[6]||null, rxQci:lte[7]||null };
	var nr = /NR5G:\s*\r?\n?SSB_BeamID:\s*(\d+)\s*\r?\n?NR_CQI:\s*(\d+)\s*\r?\n?NR_Power:\s*([-\d]+)\s*\r?\n?NR_RANK:\s*(\S+)\s*\r?\n?NR_DLMCS:\s*(\d+)\s*\r?\n?NR_ULMCS:\s*(\d+)(?:\s*\r?\n?TX_5G_QCI:\s*(\d+)\s*\r?\n?RX_5G_QCI:\s*(\d+))?/.exec(text || '');
	if (nr) out.nr = { beam:+nr[1], cqi:+nr[2], power:+nr[3], rank:nr[4], dlmcs:+nr[5], ulmcs:+nr[6], txQci:nr[7]||null, rxQci:nr[8]||null };
	return out;
}

/* +GTCCINFO? neighbour lines (AT manual §5.15):
 * LTE: 2,rat,mcc,mnc,tac,cellid,earfcn(hex),pci(hex),bw,rxlev,rsrp,rsrq
 * earfcn/pci/tac/cellid are HEX, like the service cell.  rsrp/rsrq arrive
 * as positive raw codes (the serving cell's 68/28 correspond to the
 * fm160d snapshot's ~-70 dBm / -5.5 dB), so they are displayed negated,
 * flagged as raw module units - precision the daemon does not have for
 * neighbours is not invented here. */
function parseCcinfoNeighbors(text) {
	var out = [];
	var lines = String(text || '').split(/\r?\n/);

	for (var i = 0; i < lines.length; i++) {
		var f = lines[i].split(',');
		if (f.length < 12 || f[0] !== '2')
			continue;
		var rat = parseInt(f[1], 10);
		var rsrp = parseInt(f[10], 10);
		var rsrq = parseInt(f[11], 10);

		out.push({
			rat:     rat,
			nr:      rat >= 5,
			mcc:     f[2], mnc: f[3],
			arfcn:   parseInt(f[6], 16) || 0,
			pci:     parseInt(f[7], 16) || 0,
			bwCode:  f[8] ? parseInt(f[8], 10) : null,
			rxlev:   parseInt(f[9], 10),
			rsrpRaw: isNaN(rsrp) ? null : -rsrp,
			rsrqRaw: isNaN(rsrq) ? null : -rsrq
		});
	}
	return out;
}

/* +GTSTATIS? -> rx_rate,tx_rate,rx_bytes,tx_bytes (bytes/s, bytes) */
function parseStatis(text) {
	var m = /\+GTSTATIS:\s*(\d+),(\d+),(\d+),(\d+)/.exec(text || '');
	if (!m) return null;
	return { rxRate:+m[1], txRate:+m[2], rxBytes:+m[3], txBytes:+m[4] };
}

/* +COPS? -> mode,format,operator,act */
var COPS_ACT = {0:'GSM',1:'GSM',2:'UTRAN',3:'GSM EGPRS',4:'UTRAN HSDPA',5:'UTRAN HSUPA',6:'UTRAN HSPA',7:'LTE'};

function parseCops(text) {
	var m = /\+COPS:\s*(\d+),(\d+),?"?([^,"\r\n]*)"?,?(\d*)/.exec(text || '');
	if (!m) return null;
	return { mode:+m[1], format:+m[2], operator:m[3] || '', act:m[4] === '' ? null : +m[4] };
}

function copsActName(act) {
	return (act === null || act === undefined) ? null : (COPS_ACT[act] || ('act ' + act));
}

/* Unified semantic operation log (fm160-oplog ring buffer). */
function opLog(action, detail, result) {
	return fs.exec('/usr/sbin/fm160-oplog', [ 'add', 'ui', action, detail || '', result || '' ]).catch(function() {});
}

function opLogTail(n) {
	return fs.exec('/usr/sbin/fm160-oplog', [ 'tail', String(n || 50) ]).then(function(res) {
		var lines = ((res && res.stdout) || '').split('\n').filter(function(l) { return l; });
		return lines.map(function(l) {
			try { return JSON.parse(l); } catch (e) { return null; }
		}).filter(function(e) { return e; });
	}).catch(function() { return []; });
}

function opLogClear() {
	return fs.exec('/usr/sbin/fm160-oplog', [ 'clear' ]).catch(function() {});
}

/*
 * Logged write wrappers.  Every user-triggered state change that the daemon
 * does not already record on its own gets one semantic entry here: fired
 * before the call ("initiated") and, when the promise settles, rewritten
 * with the outcome ("ok" / the failure reason).  The result line is what
 * makes the operations log worth reading - "sms send to 186..." with a
 * trailing "error: ..." tells the whole story without the daemon log.
 *
 * dial up/down and keepalive verdicts are logged server-side by
 * fm160-dial-ctl / fm160-keepalive, so those exports stay unwrapped to
 * avoid duplicate entries.
 */
function logged(call, action, detailOf, args) {
	var detail = detailOf ? detailOf.apply(null, args) : '';

	opLog(action, detail, 'initiated');

	return call.apply(null, args).then(function(res) {
		opLog(action, detail, 'ok');
		return res;
	}, function(err) {
		opLog(action, detail, 'error: ' + String(err && err.message || err));
		throw err;
	});
}

return baseclass.extend({
	USB_MODES: USB_MODES,
	USB_PLATFORM: USB_PLATFORM,
	FM160_PIDS: FM160_PIDS,
	USB_MODE_BLACKLIST: USB_MODE_BLACKLIST,
	USB_MODE_NO_KERNEL_DRIVER: USB_MODE_NO_KERNEL_DRIVER,
	USB_MODE_PREFERRED: USB_MODE_PREFERRED,
	USB_MODE_HW: USB_MODE_HW,
	USB_MODE_NONE: NONE,

	status: callStatus,
	identity: callIdentity,
	profile: callProfile,
	at: callAt,
	rescan: function() {
		return logged(callRescan, _('USB rescan'), null, arguments);
	},
	ident: function() {
		return logged(callIdent, _('identity re-read'), null, arguments);
	},
	setEnabled: function(enabled) {
		return logged(callEnabled, enabled ? _('management resume') : _('management pause'),
			function(v) { return 'enabled=' + (v ? '1' : '0'); }, arguments);
	},
	setBands: function(bands) {
		return logged(callSetBands, _('band lock change'),
			function(b) { return 'bands=' + JSON.stringify(b); }, arguments);
	},
	setCellLock: function(mode, rat, type, earfcn, pci, scs, nrband) {
		return logged(callSetCellLock, mode ? _('cell lock set') : _('cell lock clear'),
			function(m, r, t, e, p) {
				return 'mode=' + m + ' rat=' + r + ' type=' + t +
					' earfcn=' + e + ' pci=' + p;
			}, arguments);
	},
	setGnss: function(enabled) {
		return logged(callSetGnss, enabled ? _('GNSS power on') : _('GNSS power off'),
			function(v) { return 'enabled=' + (v ? '1' : '0'); }, arguments);
	},
	setGnssCfg: function(constellation) {
		return logged(callSetGnssCfg, _('GNSS config change'),
			function(c) { return 'constellation=' + c; }, arguments);
	},
	/* M3 */
	smsList: callSmsList,
	smsSend: function(number, text) {
		return logged(callSmsSend, _('SMS send'),
			function(n, t) { return 'to=' + n + ' chars=' + String(t || '').length; },
			arguments);
	},
	smsDelete: function(id) {
		return logged(callSmsDelete, _('SMS delete'),
			function(i) { return 'id=' + i; }, arguments);
	},
	smsMarkRead: function(id) {
		return logged(callSmsRead, _('SMS mark read'),
			function(i) { return 'id=' + i; }, arguments);
	},
	smsSync: function() {
		return logged(callSmsSync, _('SMS sync'), null, arguments);
	},

	/* M6 */
	diagnostics: callDiagnostics,

	ratName: ratName,
	regName: regName,
	usbModeLabel: usbModeLabel,
	usbPlatformKnown: usbPlatformKnown,
	usbModeKnown: usbModeKnown,
	usbModeHasAt: usbModeHasAt,
	usbModeDocumented: usbModeDocumented,
	usbModeHwSupported: usbModeHwSupported,
	usbModeKernelReachable: usbModeKernelReachable,
	usbModeRisk: usbModeRisk,
	usbModePidClash: usbModePidClash,
	isServiceable: isServiceable,
	reported: reported,
	dbm: dbm,
	db10: db10,
	csqOf: csqOf,
	cesqOf: cesqOf,
	trafficOf: trafficOf,
	isSsRsrp: isSsRsrp,
	rssiDbm: rssiDbm,
	csqRawRssi: csqRawRssi,
	csqRawBer: csqRawBer,
	rsrpDbm: rsrpDbm,
	rsrqDb: rsrqDb,
	cesqSinrDb: cesqSinrDb,
	cellRsrp: cellRsrp,
	cellRsrqDb: cellRsrqDb,
	cellSinrDb: cellSinrDb,
	cellBand: cellBand,
	cellPci: cellPci,
	cellBandwidthMhz: cellBandwidthMhz,
	cellPlmn: cellPlmn,
	cellTac: cellTac,
	cellCellId: cellCellId,
	cellEarfcn: cellEarfcn,
	fmtDbm: fmtDbm,
	fmtDb10: fmtDb10,
	fmtNum: fmtNum,
	fmtBandwidth: fmtBandwidth,
	fmtBand: fmtBand,
	fmtBytes: fmtBytes,
	fmtRate: fmtRate,
	fmtAge: fmtAge,
	atHealth: atHealth,

	/* --- M4 -------------------------------------------------------- */
	m4Of: m4Of,
	bandsOf: bandsOf,
	bandCapsOf: bandCapsOf,
	celllockOf: celllockOf,
	celllockCapsOf: celllockCapsOf,
	caOf: caOf,
	bandRaw: bandRaw,
	bandNo: bandNo,
	bandLabel: bandLabel,
	bandUsable: bandUsable,
	bandListOf: bandListOf,
	bandCsv: bandCsv,
	celllockRatOf: celllockRatOf,
	celllockRatName: celllockRatName,
	celllockTypeName: celllockTypeName,
	celllockEarfcn: celllockEarfcn,
	celllockPci: celllockPci,
	celllockNrband: celllockNrband,
	celllockScs: celllockScs,
	celllockModeUndocumented: celllockModeUndocumented,
	caModName: caModName,
	caStateName: caStateName,
	caCellSummary: caCellSummary,

	/* --- M5 -------------------------------------------------------- */
	gnssOf: gnssOf,
	gnssEngineOf: gnssEngineOf,
	gnssReadOf: gnssReadOf,
	gnssFixOf: gnssFixOf,
	gnssCfgOf: gnssCfgOf,
	gnssCfgCapsOf: gnssCfgCapsOf,
	gnssAgpsOf: gnssAgpsOf,
	gnssConstsOf: gnssConstsOf,
	gnssSatsOf: gnssSatsOf,
	gnssKindName: gnssKindName,
	gnssConstName: gnssConstName,
	gnssStateText: gnssStateText,
	gnssLevel: gnssLevel,
	gnssRaw: gnssRaw,
	gnssCoord: gnssCoord,
	gnssLat: fmtLat,
	gnssLon: fmtLon,
	gnssCfgLabel: gnssCfgLabel,
	gnssCfgChoices: gnssCfgChoices,
	gnssCfgCapsByX: gnssCfgCapsByX,
	gnssCfgWriteX: gnssCfgWriteX,
	gnssFixTypeName: gnssFixTypeName,
	gnssQualityName: gnssQualityName,
	gnssDop: gnssDop,
	gnssAltM: gnssAltM,
	gnssSpeedKmh: gnssSpeedKmh,
	gnssEpoName: gnssEpoName,
	/* M3 */
	smsOf: smsOf,
	smsUsable: smsUsable,
	smsStorageOf: smsStorageOf,
	smsCounterOf: smsCounterOf,
	smsTimestamp: smsTimestamp,
	smsAgeText: smsAgeText,
	smsEncodingName: smsEncodingName,
	smsPartsText: smsPartsText,
	smsEstimate: smsEstimate,

	/* --- M2 -------------------------------------------------------- */
	NET_STEP: NET_STEP,
	MODESW: MODESW,
	USB_VERDICT: USB_VERDICT,
	dialStart: callDialStart,
	dialStop: callDialStop,
	dialConfig: callDialConfig,
	profiles: callProfiles,
	setUsbMode: function(mode, advanced) {
		return logged(callSetUsbMode, _('USB mode switch'),
			function(m, a) { return 'mode=' + m + ' advanced=' + (a ? '1' : '0'); },
			arguments);
	},
	dialOf: dialOf,
	dialUp: dialUp,
	dialStep: dialStep,
	dialStepRaw: dialStepRaw,
	dialUsable: dialUsable,
	dialKind: dialKind,
	dialKindKnown: dialKindKnown,
	dialApn: dialApn,
	dialAddress: dialAddress,
	dialCid: dialCid,
	dialPdp: dialPdp,
	dialPdpInUse: dialPdpInUse,
	dialAttempt: dialAttempt,
	dialRunning: dialRunning,
	dialSimReady: dialSimReady,
	dialConfigError: dialConfigError,
	dialStoppedHealing: dialStoppedHealing,
	dialWorking: dialWorking,
	dialVerb: dialVerb,
	dialResets: dialResets,
	dialResetText: dialResetText,
	dialIsDaemons: dialIsDaemons,
	dialStateText: dialStateText,
	dialLevel: dialLevel,
	modeswOf: modeswOf,
	modeswStateRaw: modeswStateRaw,
	modeswStateText: modeswStateText,
	modeswPending: modeswPending,
	modeswBusy: modeswBusy,
	modeswCurrent: modeswCurrent,
	modeswTarget: modeswTarget,
	modeswRollback: modeswRollback,
	modeswStuck: modeswStuck,
	usbVerdictCategory: usbVerdictCategory,
	profileRows: profileRows,
	profileChoices: profileChoices,
	profileRefused: profileRefused,

	/*
	 * Modes that may be offered for a given dial kind.
	 *   supported      the modem's own AT+GTUSBMODE=? answer (numbers)
	 *   allowAdvanced  include modes present in the port table but not in the
	 *                  FM160 manual's value list
	 *   platformOk     result of usbPlatformKnown(); when false nothing is
	 *                  offered at all, because on any other platform the mode
	 *                  numbers mean something different
	 * Never returns a blacklisted mode or one known to lack an AT port.
	 */
	candidates: function(kind, supported, allowAdvanced, platformOk) {
		if (platformOk === false)
			return [];

		var prefs = USB_MODE_PREFERRED[kind] || [];
		/* Prefer the live list; fall back to the hardware-verified set rather
		 * than to "no filter", so a caller that never read AT+GTUSBMODE=?
		 * still cannot be offered a mode this firmware does not have. */
		var ok = (supported && supported.length) ? supported : USB_MODE_HW;

		return prefs.filter(function(m) {
			var risk = usbModeRisk(m);

			if (ok.indexOf(m) < 0)
				return false;
			if (risk === 'forbidden')
				return false;
			if (risk !== 'safe' && !allowAdvanced)
				return false;
			return true;
		});
	},

	/* Modes the modem reports that we cannot classify - callers must confirm. */
	unknownSupported: function(supported) {
		return (supported || []).filter(function(m) { return !usbModeKnown(m); });
	},

	/* Live radio cross-check (AT manual §5.8/5.15/5.17/5.20/8.22).  Each call
	 * issues 5 AT commands through fm160d; use on a 5 s poll, not faster. */
	radio: function() {
		function at(cmd, timeout) {
			return callAt({ cmd: cmd, timeout: timeout || 8000 }).then(function(r) {
				return (r && r.response) || '';
			}).catch(function() { return ''; });
		}
		return at('AT+GTCCINFO?').then(function(cc) {
			return at('AT+GTCAINFO?').then(function(ca) {
				return at('AT+GTSTATIS?').then(function(st) {
					return at('AT+COPS?').then(function(cops) {
						return at('AT+GTCELLINFO?').then(function(ci) {
							return {
								ccinfo:  parseCcinfo(cc),
								cainfo:  parseCainfo(ca),
								statis:  parseStatis(st),
								cops:    parseCops(cops),
								cellinfo:parseCellinfo(ci),
								at:      { cc:cc, ca:ca, st:st, cops:cops, ci:ci }
							};
						});
					});
				});
			});
		});
	},

	/* Live cell scan (AT manual §5.15/5.17).  AT+GTCELLSCAN (§5.19) was
	 * rejected: on this firmware (89614.1000.00.04.01.23, GTACT=2) it
	 * blocks the AT channel for ~50 s and never emits a single
	 * +GTCELLSCAN line, so the neighbour cells come from +GTCCINFO?
	 * (serving + up to ten LTE/NR neighbours, instant) and carrier
	 * aggregation from +GTCAINFO?.  Auto-captured on a 30 s poll while
	 * a page is open; a manual press of the search button calls the
	 * same function. */
	cellscan: function() {
		function at(cmd) {
			return callAt({ cmd: cmd, timeout: 15000 }).then(function(r) {
				return (r && r.response) || '';
			}).catch(function() { return ''; });
		}
		return at('AT+GTCCINFO?').then(function(cc) {
			return at('AT+GTCAINFO?').then(function(ca) {
				return {
					ccinfo:    parseCcinfo(cc),
					neighbors: parseCcinfoNeighbors(cc),
					cainfo:    parseCainfo(ca),
					at:        { cc: cc, ca: ca }
				};
			});
		});
	},

	caBandName: caBandName,
	copsActName: copsActName,
	parseCcinfo: parseCcinfo,
	parseCainfo: parseCainfo,
	parseCcinfoNeighbors: parseCcinfoNeighbors,
	parseCellinfo: parseCellinfo,
	parseStatis: parseStatis,
	parseCops: parseCops,
	opLog: opLog,
	opLogTail: opLogTail,
	opLogClear: opLogClear
});
