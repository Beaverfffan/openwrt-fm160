'use strict';
'require view';
'require poll';
'require ui';
'require fs';
'require rpc';
'require fm160.api as api';

/*
 * Dial page: one control surface for the three FM160 dial paths.
 *
 *   ECM   native    - fm160d drives the dial over AT, the module hands out
 *                     an address on usb0 (proto dhcp in netifd)
 *   QMAP  vendor    - fibocom-dial + qmi_wwan_f on wwan0, QMAP aggregation,
 *                     optional multi-channel
 *   MBIM  standard  - mbimcli on wwan0, best measured single-flow throughput
 *
 * The page never dials by itself.  Every button is a request to
 * /usr/sbin/fm160-dial-ctl, which validates, persists to /etc/config/fm160,
 * picks the right backend and creates the `fm160` interface section in
 * /etc/config/network so the link shows up under Network -> Interfaces.
 * The protocol handlers for qmi/mbim were deliberately removed from the
 * image: netifd does not dial this modem, the vendor tools do.
 *
 * The settings form is built ONCE (text inputs would be destroyed by a
 * repaint timer); everything else refreshes on a 3 s poll.
 */

var USB_MODES = { ecm: 33, qmap: 32, mbim: 30 };

var MODE_INFO = {
	ecm: {
		name: _('ECM (native)'),
		desc: _('fm160d dials over AT; the module serves DHCP on usb0. Most mature path.')
	},
	qmap: {
		name: _('QMAP (vendor)'),
		desc: _('fibocom-dial over qmi_wwan_f with QMAP aggregation. Multi-channel capable; single-flow throughput varies (modem firmware).')
	},
	mbim: {
		name: _('MBIM (standard)'),
		desc: _('mbimcli over cdc_mbim. Best measured single-flow speed (15-20 MB/s on this module).')
	}
};

/* --- small helpers, house style ----------------------------------------- */

function info(text)  { ui.addNotification(null, E('p', {}, text), 'info'); }
function warn(text)  { ui.addNotification(null, E('p', {}, text), 'warning'); }
function fail(text)  { ui.addNotification(null, E('p', {}, text), 'danger'); }

function row(label, value) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td', 'style': 'width:34%' }, label),
		E('td', { 'class': 'td' }, value)
	]);
}

function fmtBytes(n) {
	if (n >= 1073741824) return (n / 1073741824).toFixed(2) + ' GB';
	if (n >= 1048576) return (n / 1048576).toFixed(1) + ' MB';
	if (n >= 1024) return (n / 1024).toFixed(0) + ' KB';
	return n + ' B';
}

/* Keepalive status rows for the connection table. */
var KA_STATE = {
	idle:    _('idle'),
	running: _('running'),
	stopped: _('stopped (given up)'),
	disabled: _('disabled', 'fm160 keepalive state'),
	dead:    _('died unexpectedly')
};

function kaRows(ka) {
	if (!ka || ka.enable === undefined)
		return [ row(_('Keepalive'), _('not installed')) ];
	var stateTxt = KA_STATE[ka.state] || ka.state || _('idle');
	var lastTxt = ka.last_check || '—';
	if (ka.last_targets)
		lastTxt = '%s (%s)'.format(lastTxt, ka.last_targets);
	return [
		row(_('Keepalive'), ka.enable ? stateTxt : _('disabled', 'fm160 keepalive state')),
		row(_('Last check'), lastTxt),
		row(_('Failed rounds'),
			_('%d consecutive / %d in 24 h / %d total').format(
				ka.consec_fail || 0, ka.window_rounds || 0, ka.total_rounds || 0)),
		row(_('Module reloads'), String(ka.reloads || 0))
	];
}

/* --- backend ------------------------------------------------------------ */

function ctlStatus() {
	return fs.exec('/usr/sbin/fm160-dial-ctl', [ 'status' ])
		.then(function(res) {
			try { return JSON.parse((res && res.stdout) || '{}'); }
			catch (e) { return {}; }
		})
		.catch(function() { return {}; });
}

function ctl(args) {
	return fs.exec('/usr/sbin/fm160-dial-ctl', args)
		.then(function(res) {
			var out = (res && res.stdout) || '';
			var ok = false, err = '';
			try {
				var j = JSON.parse(out);
				ok = !!j.ok;
				err = j.error || '';
			} catch (e) { err = out.slice(0, 120); }
			return { ok: ok, error: err };
		})
		.catch(function(e) { return { ok: false, error: String(e) }; });
}

function reloadPrefixService() {
	return fs.exec('/etc/init.d/fm160-prefix', [ 'reload' ]).catch(function() {});
}

/* --- the page ------------------------------------------------------------ */

return view.extend({
	load: function() {
		return Promise.all([ ctlStatus(), api.status() ]);
	},

	render: function(data) {
		var self = this;
		this.dial = data[0] || {};
		this.modem = data[1] || {};

		this.container = E('div', {});
		this.settings = this.renderSettings();
		this.statusBox = E('div', {});
		this.busy = false;

		this.container.appendChild(this.statusBox);
		this.container.appendChild(this.settings);
		this.paint();

		poll.add(function() {
			if (!document.body.contains(self.container)) {
				poll.stop();
				return Promise.resolve();
			}
			return Promise.all([ ctlStatus(), api.status() ]).then(function(r) {
				self.dial = r[0] || {};
				self.modem = r[1] || {};
				self.paint();
			});
		});

		return this.container;
	},

	/* Repainted every poll tick: everything except the form. */
	paint: function() {
		var d = this.dial, m = this.modem;
		var mode = d.mode || 'ecm';
		var want = USB_MODES[mode];
		var usbMode = parseInt(d.usb_mode, 10);
		var switching = (usbMode !== want);
		var linkUp = !!(d.link && d.link.up);
		var reg = (m.c5greg === 1 || m.cereg === 1);

		var btnClass = 'btn cbi-button cbi-button-apply';
		var btn;

		if (this.busy) {
			btn = E('button', { 'class': 'btn cbi-button', 'disabled': 'disabled' },
				switching ? _('Switching USB profile...') : _('Connecting...'));
		} else if (!switching && linkUp) {
			btn = E('button', {
				'class': 'btn cbi-button cbi-button-reset',
				'click': ui.createHandlerFn(this, 'onDown')
			}, _('Disconnect'));
		} else {
			btn = E('button', {
				'class': btnClass,
				'click': ui.createHandlerFn(this, 'onUp')
			}, switching ? _('Connect (switches USB profile first)') : _('Connect'));
		}

		this.statusBox.replaceChildren(
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Connection')),
				E('table', { 'class': 'table' }, [
					row(_('Dial mode'), MODE_INFO[mode] ? MODE_INFO[mode].name : mode),
					row(_('Module profile'), usbMode ? String(usbMode) : _('unknown')),
					row(_('Registered'), reg ? _('yes') : _('no')),
					row(_('Link'), linkUp ?
						_('up on %s').format(d.link.ifname) :
						_('down', 'fm160 link state')),
					row(_('IPv4 address'), (d.link && d.link.v4) || _('none')),
					row(_('IPv6 address'), (d.link && d.link.v6) || _('none')),
					row(_('Traffic'), d.link ?
						_('RX %s / TX %s').format(fmtBytes(d.link.rx), fmtBytes(d.link.tx)) :
						''),
					row(_('In Network -> Interfaces'),
						d.netif ?
							_('yes (proto %s)').format(d.netif_proto || '?') :
							_('not yet (connect once to create it)'))
					].concat(kaRows(d.keepalive || {}))),
				E('div', { 'class': 'right' }, [ btn ])
			]));
	},

	renderSettings: function() {
		var self = this;
		var d = this.dial;

		/* --- mode selector ------------------------------------------------ */
		var modeCards = Object.keys(MODE_INFO).map(function(k) {
			var info_ = MODE_INFO[k];
			return E('div', { 'class': 'cbi-section', 'style': 'margin-bottom:6px' }, [
				E('label', { 'style': 'display:flex; gap:8px; align-items:baseline' }, [
					E('input', {
						'type': 'radio', 'name': 'dial_mode', 'value': k,
						'checked': (d.mode || 'ecm') === k ? 'checked' : null
					}),
					E('span', {}, [
						E('b', {}, info_.name),
						E('br'),
						E('span', { 'class': 'hint' }, info_.desc)
					])
				])
			]);
		});

		var modeBox = E('div', {}, modeCards);

		/* --- fields -------------------------------------------------------- */
		var apnInput = E('input', {
			'type': 'text', 'class': 'cbi-input-text',
			'value': d.apn || '', 'placeholder': 'ctnet',
			'style': 'width:20em'
		});

		var v4Chk = E('input', { 'type': 'checkbox', 'name': 'dial_v4' });
		if ((d.v4 === undefined ? 1 : d.v4) === 1) v4Chk.checked = true;

		var v6Chk = E('input', { 'type': 'checkbox', 'name': 'dial_v6' });
		if ((d.v6 === undefined ? 1 : d.v6) === 1) v6Chk.checked = true;

		var chanSelect = E('select', { 'name': 'qmap_channels', 'class': 'cbi-input-select' },
			[ 1, 2, 3, 4 ].map(function(n) {
				return E('option', { 'value': String(n),
					'selected': String(d.channels || 1) === String(n) ? 'selected' : null },
					String(n));
			}));

		var prefixChk = E('input', { 'type': 'checkbox', 'name': 'lan_ipv6' });
		if (String(d.lan_ipv6) === '1') prefixChk.checked = true;

		var autoChk = E('input', { 'type': 'checkbox', 'name': 'dial_autostart' });
		if ((d.autostart === undefined ? 0 : d.autostart) === 1) autoChk.checked = true;

		/* --- keepalive fields ---------------------------------------------- */
		var ka = d.keepalive || {};
		var kaChk = E('input', { 'type': 'checkbox', 'name': 'ka_enable' });
		if ((ka.enable === undefined ? 1 : ka.enable) === 1) kaChk.checked = true;

		var kaInterval = E('input', {
			'type': 'text', 'class': 'cbi-input-text', 'style': 'width:6em',
			'value': ka.interval || '30'
		});
		var kaReload = E('input', {
			'type': 'text', 'class': 'cbi-input-text', 'style': 'width:6em',
			'value': ka.reload_rounds || '5'
		});
		var kaStop = E('input', {
			'type': 'text', 'class': 'cbi-input-text', 'style': 'width:6em',
			'value': ka.stop_rounds || '10'
		});
		var kaDns4 = E('input', {
			'type': 'text', 'class': 'cbi-input-text', 'style': 'width:16em',
			'value': ka.dns4 || '', 'placeholder': _('auto (module DNS)')
		});
		var kaDns6 = E('input', {
			'type': 'text', 'class': 'cbi-input-text', 'style': 'width:16em',
			'value': ka.dns6 || '', 'placeholder': _('auto (module DNS)')
		});

		function modeIs(m) {
			var r = modeBox.querySelector('input[name=dial_mode]:checked');
			return r && r.value === m;
		}

		function refreshModeFields() {
			chanRow.style.display = modeIs('qmap') ? '' : 'none';
		}

		var chanRow = E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td', 'style': 'width:34%' }, _('QMAP channels')),
			E('td', { 'class': 'td' }, chanSelect)
		]);
		modeBox.addEventListener('change', refreshModeFields);

		var saveBtn = E('button', {
			'class': 'btn cbi-button cbi-button-apply',
			'click': ui.createHandlerFn(this, 'onSave', {
				modeBox: modeBox, apnInput: apnInput,
				v4Chk: v4Chk, v6Chk: v6Chk, chanSelect: chanSelect,
				prefixChk: prefixChk, autoChk: autoChk,
				kaChk: kaChk, kaInterval: kaInterval, kaReload: kaReload,
				kaStop: kaStop, kaDns4: kaDns4, kaDns6: kaDns6
			})
		}, _('Save settings'));

		var kaForm = E('div', { 'class': 'cbi-section', 'style': 'margin-top:10px' }, [
			E('h3', {}, _('Keepalive (link watchdog)')),
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td', 'style': 'width:34%' }, _('Enable keepalive')),
					E('td', { 'class': 'td' }, kaChk)
				]),
				E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, _('Check interval (seconds)')),
					E('td', { 'class': 'td' }, kaInterval)
				]),
				E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, _('Reload module after N consecutive failed rounds')),
					E('td', { 'class': 'td' }, kaReload)
				]),
				E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, _('Give up after N rounds within 24 h')),
					E('td', { 'class': 'td' }, kaStop)
				]),
				E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, _('Ping target IPv4 (empty = module DNS)')),
					E('td', { 'class': 'td' }, kaDns4)
				]),
				E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, _('Ping target IPv6 (empty = module DNS)')),
					E('td', { 'class': 'td' }, kaDns6)
				])
			]),
			E('p', { 'class': 'hint' }, [
				_('Every interval the module-reported DNS is pinged over the carrier path (IPv4/IPv6 honouring the stack toggles).'),
				' ',
				_('A failed round triggers a redial; after the consecutive-failure threshold the module USB profile is bounced and reloaded; after the 24 h budget is exhausted the watchdog stops by itself.')
			])
		]);

		var form = E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Dial settings')),
			modeBox,
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td', 'style': 'width:34%' }, _('APN')),
					E('td', { 'class': 'td' }, apnInput)
				]),
				E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, _('IPv4')),
					E('td', { 'class': 'td' }, v4Chk)
				]),
				E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, _('IPv6')),
					E('td', { 'class': 'td' }, v6Chk)
				]),
				chanRow,
				E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, _('Delegate IPv6 /64 to LAN')),
					E('td', { 'class': 'td' }, prefixChk)
				]),
				E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, _('Connect automatically at boot')),
					E('td', { 'class': 'td' }, autoChk)
				])
			]),
			E('div', { 'class': 'right' }, [ saveBtn ]),
			E('p', { 'class': 'hint' }, [
				_('IPv4/IPv6 map to the PDP type on ECM (-4 -6 = IPV4V6) and to -4/-6 flags on QMAP / ip-type on MBIM.'),
				' ',
				_('Saving does not dial — press Connect. Changing the mode may restart the USB profile (30-90 s).')
			])
		]);

		/* initial visibility */
		setTimeout(refreshModeFields, 0);

		return E('div', {}, [ form, kaForm ]);
	},

	onSave: function(fields, ev) {
		var self = this;
		var modeRadio = fields.modeBox.querySelector('input[name=dial_mode]:checked');
		var mode = modeRadio ? modeRadio.value : 'ecm';
		var apn = fields.apnInput.value.trim();

		if (!apn) { fail(_('APN is required.')); return; }
		if (!/^[A-Za-z0-9._-]+$/.test(apn)) {
			fail(_('APN may only contain letters, digits, dot, dash and underscore.'));
			return;
		}
		if (!fields.v4Chk.checked && !fields.v6Chk.checked) {
			fail(_('At least one of IPv4 / IPv6 must be enabled.'));
			return;
		}

		var kaInt = parseInt(fields.kaInterval.value, 10);
		var kaRel = parseInt(fields.kaReload.value, 10);
		var kaStp = parseInt(fields.kaStop.value, 10);
		var ka4 = fields.kaDns4.value.trim();
		var ka6 = fields.kaDns6.value.trim();
		if (isNaN(kaInt) || kaInt < 10 || kaInt > 600) {
			fail(_('Check interval must be 10-600 seconds.')); return;
		}
		if (isNaN(kaRel) || kaRel < 2 || kaRel > 50) {
			fail(_('Module reload threshold must be 2-50 rounds.')); return;
		}
		if (isNaN(kaStp) || kaStp < 2 || kaStp > 100) {
			fail(_('Give-up threshold must be 2-100 rounds.')); return;
		}
		if (ka4 && !/^[0-9.]+$/.test(ka4)) {
			fail(_('IPv4 ping target may only contain digits and dots (or empty).')); return;
		}
		if (ka6 && ka6.indexOf(':') < 0) {
			fail(_('IPv6 ping target must be an IPv6 address (or empty).')); return;
		}

		var ops = [
			[ 'set', 'dial_mode', mode ],
			[ 'set', 'dial_apn', apn ],
			[ 'set', 'dial_v4', fields.v4Chk.checked ? '1' : '0' ],
			[ 'set', 'dial_v6', fields.v6Chk.checked ? '1' : '0' ],
			[ 'set', 'qmap_channels', fields.chanSelect.value ],
			[ 'set', 'dial_autostart', fields.autoChk.checked ? '1' : '0' ],
			[ 'set', 'lan_ipv6', fields.prefixChk.checked ? '1' : '0' ],
			[ 'set', 'ka_enable', fields.kaChk.checked ? '1' : '0' ],
			[ 'set', 'ka_interval', String(kaInt) ],
			[ 'set', 'ka_reload_rounds', String(kaRel) ],
			[ 'set', 'ka_stop_rounds', String(kaStp) ],
			[ 'set', 'ka_dns4', ka4 ],
			[ 'set', 'ka_dns6', ka6 ]
		];

		var chain = Promise.resolve();
		ops.forEach(function(op) {
			chain = chain.then(function() { return ctl(op); });
		});

		chain.then(function() { return reloadPrefixService(); })
			.then(function() {
				info(_('Settings saved. Press Connect to dial.'));
				return ctlStatus();
			})
			.then(function(st) { self.dial = st; self.paint(); });
	},

	onUp: function(ev) {
		var self = this;
		this.busy = true;
		this.paint();
		/* up 会阻塞到 USB 切换完成（30-90s），超出 LuCI 请求超时，结果以轮询为准 */
		info(_('Dial command sent. The link comes up over the next seconds; a mode switch can take a minute.'));
		ctl([ 'up' ]).catch(function(e) {
			/* 请求超时不代表失败：后端仍在执行，轮询会反映真实状态 */
			console.warn('fm160-dial-ctl up:', e);
		}).then(function() {
			return ctlStatus();
		}).then(function(st) {
			self.busy = false;
			self.dial = st; self.paint();
		});
	},

	onDown: function(ev) {
		var self = this;
		this.busy = true;
		this.paint();
		ctl([ 'down' ]).then(function() {
			return ctlStatus();
		}).then(function(st) {
			self.busy = false;
			self.dial = st; self.paint();
		});
	}
});

function btn(el, label, disabled) {
	el.textContent = label;
	if (disabled) el.setAttribute('disabled', 'disabled');
	else el.removeAttribute('disabled');
}
