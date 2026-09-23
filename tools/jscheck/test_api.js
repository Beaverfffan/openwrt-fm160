'use strict';

/*
 * Unit tests for the front-end schema layer (fm160/api.js).
 *
 * There is no other way to test this code here: LuCI's JavaScript runs only in
 * a browser, behind rpcd, against a live daemon, on a router.  But api.js is
 * almost entirely pure functions, so it can be loaded in node against a stub
 * baseclass/rpc/_ and checked directly.
 *
 * The two things worth guarding are the ones that were wrong in practice and
 * that no type checker could have caught:
 *
 *   the sentinel    fm160d writes "not reported" as -1000000 with
 *                   blobmsg_add_u32, so JavaScript sees 4293967296.  Every
 *                   accessor must reject that, not print it.
 *
 *   the mode sets   which USB modes may be offered is a policy decision that
 *                   depends on the kernel, not on the modem, and offering mode
 *                   29 would strand the unit with no AT channel.
 *
 * Usage: node tools/jscheck/test_api.js
 */

const fs = require('fs');
const path = require('path');

const API_JS = path.join(__dirname, '..', '..', 'luci-app-fm160',
	'htdocs', 'luci-static', 'resources', 'fm160', 'api.js');

let failures = 0;
let checks = 0;

function eq(actual, expected, what) {
	checks++;
	const a = JSON.stringify(actual);
	const e = JSON.stringify(expected);

	if (a !== e) {
		failures++;
		console.log('  FAIL  ' + what);
		console.log('        expected ' + e);
		console.log('        actual   ' + a);
	}
}

function ok(cond, what) {
	checks++;
	if (!cond) {
		failures++;
		console.log('  FAIL  ' + what);
	}
}

/* --- load api.js with LuCI's globals stubbed -------------------------- */

const src = fs.readFileSync(API_JS, 'utf8');

const baseclass = { extend: function(o) { return o; } };
const rpc = { declare: function() { return function() {}; } };
const _ = function(s) { return s; };

/* The file ends in a top-level `return`, which is legal inside a function
 * body, so wrapping it in one is the whole trick. */
const api = new Function('baseclass', 'rpc', '_', src)(baseclass, rpc, _);

console.log('api.js loaded: ' + Object.keys(api).length + ' exports');

/* --- the sentinel ----------------------------------------------------- */

const NEG = -1000000;                 /* as fm160d's signed value */
const WRAPPED = 4293967296;           /* the same value after uint32 cast */

eq(api.reported(WRAPPED), false, 'reported() rejects the uint32-wrapped sentinel');
eq(api.reported(NEG), false, 'reported() rejects the signed sentinel');
eq(api.reported(undefined), false, 'reported() rejects undefined');
eq(api.reported(null), false, 'reported() rejects null');
eq(api.reported('12'), false, 'reported() rejects a string');
eq(api.reported(0), true, 'reported() accepts 0 (live, but "not reported" to callers)');
eq(api.reported(-85), true, 'reported() accepts a real dBm');

eq(api.dbm(WRAPPED), null, 'dbm() rejects the wrapped sentinel');
eq(api.dbm(NEG), null, 'dbm() rejects the signed sentinel');
eq(api.dbm(-95), -95, 'dbm() passes a real value');
eq(api.dbm(0), null, 'dbm() rejects 0 (dBm is never 0)');

eq(api.db10(WRAPPED), null, 'db10() rejects the wrapped sentinel');
eq(api.db10(-125), -12.5, 'db10() divides tenths');

/* --- a realistic status blob ----------------------------------------- */

const CSR_RSRP = {
	port: '/dev/ttyUSB2', port_found: true, enabled: true, at_state: 0,
	csq: { raw_rssi: WRAPPED, raw_ber: 99, is_ss_rsrp: true },
	cesq: {
		lte_rsrp_dbm: WRAPPED, lte_rsrq_db10: WRAPPED,
		nr_ss_rsrp_dbm: -95, nr_ss_rsrq_db10: -115, nr_ss_sinr_db10: 145
	},
	cell_valid: true,
	serving: {
		valid: 1, rat: 9, mcc: 460, mnc: 0, tac: 12345, cellid: 987654,
		earfcn: 504990, pci: WRAPPED, band: 78, bandwidth: 100,
		rsrp_dbm: -96, rsrq_db10: -118, sinr_db10: 152
	},
	neighbours: [],
	traffic: { netdev: 'wwan0', rx_bytes: 1000, tx_bytes: 2000, rx_bps: 10, tx_bps: 20 }
};

/* The 5G path: CSQ's first field is SS-RSRP, so there is no RSSI to show. */
eq(api.isSsRsrp(CSR_RSRP), true, 'is_ss_rsrp is detected');
eq(api.rssiDbm(CSR_RSRP), null, 'no RSSI is invented when the field is SS-RSRP');
eq(api.rsrpDbm(CSR_RSRP), -95, 'RSRP comes from the NR column when is_ss_rsrp');
eq(api.rsrqDb(CSR_RSRP), -11.5, 'RSRQ comes from the NR column when is_ss_rsrp');
eq(api.cesqSinrDb(CSR_RSRP), 14.5, 'CESQ SINR is decoded from tenths');

const c = CSR_RSRP.serving;
eq(api.cellRsrp(c), -96, 'cell RSRP');
eq(api.cellRsrqDb(c), -11.8, 'cell RSRQ from tenths');
eq(api.cellSinrDb(c), 15.2, 'cell SINR from tenths');
eq(api.cellPci(c), null, 'cell PCI is rejected when the modem did not report one');
eq(api.cellBand(c), 78, 'cell band');
eq(api.cellBandwidthMhz(c), 100, 'cell bandwidth is MHz, not a 3GPP code point');
eq(api.cellPlmn(c), '460-0', 'PLMN');
eq(api.cellEarfcn(c), 504990, 'EARFCN');
eq(api.fmtBandwidth(c), '100 MHz', 'bandwidth is labelled MHz');

/* The LTE path: RSSI is real, PCI was reported. */
const LTE = {
	csq: { raw_rssi: 20, raw_ber: 99, is_ss_rsrp: false, rssi_dbm: -73 },
	cesq: { lte_rsrp_dbm: -101, lte_rsrq_db10: -95, nr_ss_rsrp_dbm: WRAPPED },
	cell_valid: true,
	serving: { rat: 4, pci: 301, band: 3, bandwidth: 20, rsrp_dbm: -101, rsrq_db10: -95, sinr_db10: 80 }
};

eq(api.rssiDbm(LTE), -73, 'RSSI is shown when the field really is an RSSI');
eq(api.rsrpDbm(LTE), -101, 'RSRP comes from the LTE column when is_ss_rsrp is false');
eq(api.rsrqDb(LTE), -9.5, 'RSRQ comes from the LTE column');
eq(api.cellPci(LTE.serving), 301, 'a reported PCI survives');
eq(api.fmtDbm(api.rsrpDbm(LTE)), '-101 dBm', 'formatting a real dBm');
eq(api.fmtDbm(api.rsrpDbm(CSR_RSRP)), '-95 dBm', 'formatting the NR dBm');
eq(api.fmtDbm(null), '-', 'formatting a missing dBm');
eq(api.fmtNum(null), '-', 'formatting a missing number');

/* An empty daemon snapshot must not throw and must show nothing. */
const EMPTY = {};
eq(api.rssiDbm(EMPTY), null, 'empty state -> no RSSI');
eq(api.rsrpDbm(EMPTY), null, 'empty state -> no RSRP');
eq(api.rsrqDb(EMPTY), null, 'empty state -> no RSRQ');
eq(api.cellRsrp(undefined), null, 'undefined cell -> no RSRP');
eq(api.cellPci(null), null, 'null cell -> no PCI');

/* --- the USB mode policy --------------------------------------------- */

const HW = api.USB_MODE_HW;                       /* what the modem offers */
const ALL = HW.concat([ 19, 22, 23, 28 ]);        /* plus what other builds offer */

eq(api.usbModeRisk(29), 'forbidden', 'mode 29 is forbidden: no kernel driver for 0x0110');
eq(api.usbModeRisk(21), 'forbidden', 'mode 21 is forbidden: no kernel driver for 0x0108');
eq(api.usbModeRisk(20), 'forbidden', 'mode 20 is forbidden: no AT interface');
eq(api.usbModeRisk(24), 'forbidden', 'mode 24 is forbidden: no AT interface');
eq(api.usbModeRisk(31), 'forbidden', 'mode 31 is forbidden: no AT interface');
eq(api.usbModeRisk(32), 'safe', 'mode 32 is safe');
eq(api.usbModeRisk(17), 'safe', 'mode 17 is safe');
eq(api.usbModeRisk(30), 'safe', 'mode 30 is safe');
eq(api.usbModeRisk(33), 'safe', 'mode 33 is safe');
eq(api.usbModeRisk(18), 'safe', 'mode 18 is safe');

eq(api.usbModeKernelReachable(29), false, 'mode 29 is not kernel reachable');
eq(api.usbModeKernelReachable(30), true, 'mode 30 is kernel reachable');

eq(api.candidates('mbim', ALL, true, true), [ 30 ],
	'MBIM offers exactly mode 30, even with advanced modes allowed');
eq(api.candidates('qmi', ALL, true, true), [ 32, 17 ], 'QMI prefers 32 then 17');
eq(api.candidates('ecm', ALL, true, true), [ 33, 18 ], 'ECM prefers 33 then 18');

/* A mode the modem does not report must not be offered, however good it looks. */
eq(api.candidates('mbim', [ 17, 18 ], true, true), [],
	'no MBIM candidate is offered when the modem does not report one');
eq(api.candidates('qmi', HW, true, true), [ 32, 17 ], 'the hardware set is enough');
eq(api.candidates('qmi', [], true, true), [ 32, 17 ],
	'an unread AT+GTUSBMODE=? falls back to the verified set, never to "anything"');

/* Wrong platform: the mode numbers mean something else entirely, so nothing. */
eq(api.candidates('qmi', ALL, true, false), [],
	'nothing is offered when the VID:PID is not the FM160 platform');

eq(api.usbModeRisk(99), 'unknown', 'an unlisted mode is unknown, not safe');
eq(api.usbModeRisk(23), 'advanced', 'a mode documented only for other builds is advanced');
eq(api.candidates('ecm', ALL, false, true), [ 33, 18 ],
	'advanced modes are excluded when not explicitly allowed');

eq(api.usbPlatformKnown('2CB7', '0x0104'), true, 'the FM160 platform is recognised');
eq(api.usbPlatformKnown('2cb7', '0x0110'), true, 'mode 29 PID is the same platform');
eq(api.usbPlatformKnown('2cb7', '0x0111'), true, 'mode 30 PID is the same platform');
eq(api.usbPlatformKnown('1508', '0x1001'), false, 'the NL668 platform is not ours');

/*
 * The regression that matters most: after a switch the device re-enumerates
 * with a different PID, and if that PID is not recognised then the way back
 * disappears.  Every PID the modem can present -- including the target of every
 * offered switch -- must pass, or the UI offers a one-way trip.
 */
let pidsChecked = 0;

Object.keys(api.USB_MODES).forEach(function(m) {
	const pid = api.USB_MODES[m].pid;

	if (!pid)
		return;
	pidsChecked++;
	eq(api.usbPlatformKnown('2cb7', pid), true,
		'mode ' + m + ' (PID ' + pid + ') is recognised as the FM160 platform');
});
ok(pidsChecked >= 10, 'every mode with a PID was covered (' + pidsChecked + ')');

/*
 * And the other direction: 0x2cb7 is Fibocom's VID, shared with FM650-CN,
 * FG132, FM135 and FM101-GL.  Recognising one of those as an FM160 would mean
 * offering mode numbers that mean something else.
 */
eq(api.usbPlatformKnown('2cb7', '0x0a05'), false, 'FM650-CN NCM is not an FM160');
eq(api.usbPlatformKnown('2cb7', '0x0112'), false, 'FG132 is not an FM160');
eq(api.usbPlatformKnown('2cb7', '0x01a2'), false, 'FM101-GL is not an FM160');
eq(api.usbPlatformKnown(null, '0x0104'), false, 'a missing VID is not the FM160 platform');
eq(api.usbPlatformKnown('2cb7', null), false, 'a missing PID is not the FM160 platform');

/*
 * The lock-out, stated as a test: standing in MBIM (mode 30, PID 0x0111) the QMI
 * candidates must still be offered, because that is how the user gets back.
 */
ok(api.usbPlatformKnown('2cb7', '0x0111'), 'the MBIM PID is recognised');
eq(api.candidates('qmi', api.USB_MODE_HW, true, api.usbPlatformKnown('2cb7', '0x0111')),
	[ 32, 17 ], 'from MBIM, the QMI modes are still offered (no lock-out)');
eq(api.candidates('ecm', api.USB_MODE_HW, true, api.usbPlatformKnown('2cb7', '0x0110')),
	[ 33, 18 ], 'from mode 29, the ECM modes are still offered (no lock-out)');

eq(api.usbModePidClash(32), [ 17, 32 ], '17 and 32 are indistinguishable over USB');
eq(api.usbModePidClash(33), [ 18, 33 ], '18 and 33 are indistinguishable over USB');
eq(api.usbModePidClash(30), null, 'mode 30 has no PID clash');

/* --- M2: the data plane ----------------------------------------------- */

/*
 * A unit dialling ECM, caught mid-registration.  The shape is the snapshot's,
 * field for field, because the point is to exercise the accessors that the dial
 * page reads rather than to invent a convenient object.
 */
const DIALING = {
	port_found: true,
	dial: {
		wanted: true, step: 'waiting for registration', step_raw: 2,
		kind: 'ecm', kind_known: true, usable: true, cid: 1, pdp: 'IP',
		apn: 'internet', up: false, address: '', dns1: '', dns2: '',
		pdp_in_use: '', verb_known: false, verb: '', attempt: 1, ladder_len: 3,
		ip_polls: 0, running: false, sim_ready: true, was_up: false,
		allow_reset: false, autostart: false, config_error: false,
		healing_stopped: false, starts: 1, failures: 1, last_error: '',
		resets: { in_window: 0, limit: 3, window_s: 86400, total: 0 }
	},
	modesw: {
		state_raw: 0, state: 'idle', current_known: true, current: 32,
		target: -1, rollback_mode: -1, caps_valid: true,
		supported: [ 17, 18, 20, 21, 24, 29, 30, 31, 32, 33 ],
		pending: false, busy: false, rolled_back: false, verify_result: 0
	}
};

eq(api.dialWorking(DIALING), true, 'a dial in progress counts as working');
eq(api.dialIsDaemons(api.dialKind(DIALING)), true, 'ECM is the profile fm160d dials');
eq(api.dialUsable(DIALING), true, 'an ECM unit is usable');
/* The verb is probed, never assumed: the same vendor's two documents disagree
 * (AT manual says +GTWWAN for ECM, the dial-up document shows +GTRNDIS). */
eq(api.dialVerb(DIALING), null, 'an unprobed verb reads as null, not as a guess');
eq(api.dialVerb({ dial: { verb_known: true, verb: 'GTRNDIS' } }), 'GTRNDIS',
	'a probed verb is reported');
eq(api.dialStateText(DIALING), 'waiting for registration',
	'the state line is the daemon\'s own step name');
eq(api.dialLevel(DIALING), 'warn', 'a dial in progress is not yet an error');
eq(api.dialResetText(DIALING), '0 / 3 in the last 24 h', 'the reset ledger reads back');

/* QMI and MBIM belong to the kernel, and the page must say so rather than
 * offering a button that can only end in -ENOTSUP. */
const QMI = { dial: { kind: 'qmi', kind_known: true, step_raw: 0, step: 'idle' } };

eq(api.dialIsDaemons('qmi'), false, 'a QMI profile is not the daemon\'s to dial');
eq(api.dialUsable(QMI), false, 'fm160_dial_usable() is false on a QMI profile');
ok(api.dialStateText(QMI).indexOf('kernel') >= 0,
	'the state line names the kernel as the dialler, not fm160d');
eq(api.dialLevel(QMI), 'off', 'a profile we do not dial is not a fault');

/* A unit that has never had its profile read: unknowable, not "none". */
eq(api.dialUsable({ dial: { kind_known: false, kind: 'none' } }), false,
	'an unread profile is not usable');
eq(api.dialLevel({ dial: { kind_known: false } }), 'off',
	'an unread profile is not a fault either');

/* No APN.  This is the state the page has to refuse in, before any AT traffic. */
const NOAPN = { dial: { kind: 'ecm', kind_known: true, config_error: true,
			step_raw: 0, step: 'idle' } };
ok(api.dialStateText(NOAPN).indexOf('APN') >= 0, 'a missing APN is named as the reason');
eq(api.dialLevel(NOAPN), 'bad', 'a missing APN is a refusal, not a warning');

/* A failed attempt is waiting out its backoff - not "working", and the error
 * text has to reach the user rather than being swallowed. */
const FAILED = { dial: { kind: 'ecm', kind_known: true, wanted: true, up: false,
			 step: 'failed', step_raw: 8, last_error: '+CME ERROR: 50' } };
eq(api.dialWorking(FAILED), false,
	'a failed attempt is waiting out its backoff, not working');
eq(api.dialLevel(FAILED), 'bad', 'a failed attempt is an error');
ok(api.dialStateText(FAILED).indexOf('+CME ERROR: 50') >= 0,
	'the modem\'s own error text survives into the state line');

/* An empty snapshot must not throw: the page paints before the first poll. */
eq(api.dialStateText({}), 'the USB profile has not been read yet',
	'an empty dial table reads as "not read yet"');
eq(api.dialStateText(undefined), 'the USB profile has not been read yet',
	'and so does a missing one');

/* modesw */

eq(api.modeswCurrent(DIALING), 32, 'the active profile is read back');
/* The sentinel problem again, in the table where a wrong answer changes which
 * heading the user is standing under. */
eq(api.modeswCurrent({ modesw: { current_known: false, current: WRAPPED } }), null,
	'an unread profile reads as null, not as a mode number');
eq(api.modeswRollback({ modesw: { rollback_mode: WRAPPED } }), null,
	'the uint32-wrapped -1 rollback point is rejected');
eq(api.modeswRollback({ modesw: { rollback_mode: 32 } }), 32, 'a real rollback point passes');
eq(api.modeswRollback({ modesw: { rollback_mode: -1 } }), null,
	'the signed -1 rollback point is rejected too');
eq(api.modeswStuck({ modesw: { state_raw: 4 } }), true, 'STUCK is detected');
eq(api.modeswStuck({ modesw: { state_raw: 3 } }), false, 'VERIFY is not STUCK');
eq(api.modeswPending({ modesw: { pending: true, target: 30 } }), true, 'a pending switch is visible');
eq(api.modeswTarget({ modesw: { target: 30 } }), 30, 'the pending target is named');
eq(api.modeswTarget({ modesw: { target: WRAPPED } }), null, 'a wrapped target is rejected');

/* The profile list, with the daemon's verdicts as fm160.profiles sends them. */
const PROFILES = {
	current: 32, current_known: true, caps_valid: true,
	supported: [ 17, 18, 20, 21, 24, 29, 30, 31, 32, 33 ],
	profiles: [
		{ mode: 32, pid: '0104', kind: 'qmi', layout: 'DIAG+MODEM+AT+PIPE+RMNET',
		  has_at: true, documented: true, kernel_entry: true, blacklisted: false,
		  current: true, verdict: 1, verdict_text: 'the modem is already in that profile',
		  selectable: true, selectable_advanced: true },
		{ mode: 33, pid: '0105', kind: 'ecm', layout: 'DIAG+MODEM+AT+PIPE+ECM+ECM',
		  has_at: true, documented: true, kernel_entry: true, blacklisted: false,
		  current: false, verdict: 0, verdict_text: 'allowed',
		  selectable: true, selectable_advanced: true },
		{ mode: 24, pid: '010b', kind: 'none', layout: 'RNDIS+RNDIS+MODEM+DIAG+ADB (no AT)',
		  has_at: false, documented: true, kernel_entry: true, blacklisted: true,
		  current: false, verdict: 3, verdict_text: 'that profile has no AT interface',
		  selectable: false, selectable_advanced: false },
		{ mode: 23, pid: '010a', kind: 'ecm', layout: 'MODEM+AT+ECM+ECM',
		  has_at: true, documented: false, kernel_entry: true, blacklisted: false,
		  current: false, verdict: 5, verdict_text: 'needs an explicit opt-in',
		  selectable: false, selectable_advanced: true }
	]
};

const rows = api.profileRows(PROFILES);

eq(rows.length, 4, 'every profile the daemon sent is described');
eq(api.profileChoices(rows, false).map(function(p) { return p.mode; }), [ 33 ],
	'the current profile is not a choice, and 23 needs the opt-in');
eq(api.profileChoices(rows, true).map(function(p) { return p.mode; }), [ 33, 23 ],
	'the opt-in adds exactly the device-dependent profile');
eq(api.profileRefused(rows, false).map(function(p) { return p.mode; }), [ 24, 23 ],
	'a profile the opt-in would unlock is "not offered yet", not hidden');
eq(api.profileRefused(rows, true).map(function(p) { return p.mode; }), [ 24 ],
	'and the opt-in moves 23 out of that list without reaching 24');
ok(rows.filter(function(p) { return p.mode === 23; })[0].needsOptin,
	'23 is marked as needing the opt-in, which is the only reason to pass advanced=true');
ok(!rows.filter(function(p) { return p.mode === 33; })[0].needsOptin,
	'33 does not need the opt-in');
eq(rows.filter(function(p) { return p.mode === 32; })[0].current, true,
	'the active profile is marked as such');
eq(rows.filter(function(p) { return p.mode === 24; })[0].verdictKind, 'no-at',
	'the refusal reason is grouped for display');

eq(api.profileRows(null), [], 'no reply -> no rows, and no exception');
eq(api.profileRows({}), [], 'an empty reply -> no rows');
eq(api.profileChoices(null, true), [], 'no rows -> no choices');
eq(api.profileRefused(undefined, false), [], 'no rows -> nothing refused');

/* The verdict numbers are part of the wire contract; a silent renumbering in
 * fm160d would otherwise turn every refusal into a category of 'unknown'. */
eq(api.USB_VERDICT.OK, 0, 'verdict OK is 0');
eq(api.USB_VERDICT.OK_SAME, 1, 'verdict OK_SAME is 1');
eq(api.USB_VERDICT.DENY_NO_AT, 3, 'verdict DENY_NO_AT is 3');
eq(api.USB_VERDICT.DENY_NO_DRIVER, 4, 'verdict DENY_NO_DRIVER is 4');
eq(api.USB_VERDICT.DENY_UNDOCUMENTED, 5, 'verdict DENY_UNDOCUMENTED is 5');
eq(api.USB_VERDICT.DENY_NOT_SUPPORTED, 6, 'verdict DENY_NOT_SUPPORTED is 6');
eq(api.USB_VERDICT.DENY_LIST_UNKNOWN, 7, 'verdict DENY_LIST_UNKNOWN is 7');
eq(api.usbVerdictCategory(1), 'same', 'OK_SAME groups as "same"');
eq(api.usbVerdictCategory(3), 'no-at', 'DENY_NO_AT groups as "no-at"');
eq(api.usbVerdictCategory(7), 'list-unknown', 'DENY_LIST_UNKNOWN groups on its own');
eq(api.usbVerdictCategory(99), 'unknown', 'an unrecognised verdict is not guessed at');
eq(api.usbVerdictCategory(undefined), 'unknown', 'a missing verdict is not guessed at');

/* The step numbers are mirrored for readability only, so they must agree with
 * the enum fm160_net_step that fm160d names them from. */
eq(api.NET_STEP.IDLE, 0, 'step IDLE is 0');
eq(api.NET_STEP.ACTIVATE, 4, 'step ACTIVATE is 4');
eq(api.NET_STEP.UP, 6, 'step UP is 6');
eq(api.NET_STEP.FAILED, 8, 'step FAILED is 8');
eq(api.NET_STEP.DOWN, 9, 'step DOWN is 9');
eq(api.MODESW.IDLE, 0, 'modesw IDLE is 0');
eq(api.MODESW.STUCK, 4, 'modesw STUCK is 4');

/* --- every api.<name> the views call must be exported ------------------ */

/*
 * The failure this catches
 * ------------------------
 * A LuCI view is never compiled and is never executed here, so `api.something`
 * that api.js does not export is accepted by every build stage and by every
 * check in this directory.  It fails in the browser, as a blank section, on the
 * router, with nothing in any log - which is the same class of failure as the
 * missing brace that shipped overview.js.
 *
 * api.js is already loaded above, so its export list is known exactly; the
 * views are read as text.  Crude, and it is the entire point: this is a name
 * check, and a name check is what an untyped require() needs.
 */
const VIEW_DIR = path.join(__dirname, '..', '..', 'luci-app-fm160',
	'htdocs', 'luci-static', 'resources', 'view', 'fm160');

const viewFiles = fs.readdirSync(VIEW_DIR).filter(function(f) {
	return f.endsWith('.js');
}).sort();

ok(viewFiles.length >= 7, 'the view directory was found (' + viewFiles.length + ' files)');

/*
 * Comments are stripped BEFORE the scan, and not as a nicety: every one of
 * these files explains itself by name - "the list comes from fm160.profiles,
 * not from api.js's copy of the table" - and a scan over the prose reports the
 * file's own filename as a missing export.  It did exactly that on the first
 * run, in three files.
 */
function stripComments(text) {
	return text.replace(/\/\*[\s\S]*?\*\//g, ' ')
		   .replace(/^[ \t]*\/\/.*$/gm, ' ');
}

/* The names a block of code uses that api.js does not export. */
function unresolvedApiNames(code) {
	const re = /\bapi\.([A-Za-z_][A-Za-z0-9_]*)/g;
	const out = [];
	let m;

	while ((m = re.exec(code)))
		if (!Object.prototype.hasOwnProperty.call(api, m[1]))
			out.push(m[1]);
	return out;
}

const missing = [];
const aliasless = [];

viewFiles.forEach(function(name) {
	const text = fs.readFileSync(path.join(VIEW_DIR, name), 'utf8');

	/* The contract below is only as good as the alias it scans for, so a view
	 * that requires the module under another name is called out rather than
	 * silently skipped.  This one has to look at the raw text: the require is
	 * a string, and stripping comments does not touch it either way. */
	if (text.indexOf("'require fm160.api as api'") < 0)
		aliasless.push(name);

	unresolvedApiNames(stripComments(text)).forEach(function(n) {
		missing.push(name + ': api.' + n);
	});
});

ok(aliasless.length === 0,
	'every view requires the module as "api"' +
	(aliasless.length ? ' -- not found in: ' + aliasless.join(', ') : ''));
ok(missing.length === 0,
	'every api member the views call is exported' +
	(missing.length ? ' -- missing: ' + missing.join(', ') : ''));

/*
 * And the scan itself is tested, because a name check is one typo away from
 * being vacuous: a regex that matches nothing passes forever and reports
 * success.  The three cases below are the three ways it can go wrong - refusing
 * names that exist, accepting names that do not, and reading prose.
 */
eq(unresolvedApiNames('var x = api.dialWorking(s);'), [],
	'the scan accepts an exported member');
eq(unresolvedApiNames('var x = api.thisWasNeverExported(s);'),
	[ 'thisWasNeverExported' ],
	'the scan rejects a member that does not exist');
eq(unresolvedApiNames(stripComments('/* api.ghost, see api.js */ var x = 1;')), [],
	'and the comment stripper keeps prose out of it');

/* --- report ----------------------------------------------------------- */

console.log(checks - failures + '/' + checks + ' assertions passed');
if (failures) {
	console.log('test_api: FAILED');
	process.exit(1);
}
console.log('test_api: clean');
