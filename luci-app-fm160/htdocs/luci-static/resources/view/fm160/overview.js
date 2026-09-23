'use strict';
'require view';
'require poll';
'require ui';
'require fm160.api as api';

/*
 * Overview: identity, SIM and registration state, link health and traffic.
 *
 * Everything on this page comes from fm160d's cache.  The only thing this page
 * does that touches the daemon's behaviour is the periodic profile() ping, which
 * tells it a UI is open so the faster polling tier becomes active.
 */

function kv(rows) {
	return E('table', { 'class': 'table' }, rows.map(function(r) {
		return E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'style': 'width:34%' }, r[0]),
			E('td', { 'class': 'td left' }, r[1])
		]);
	}));
}

function section(title, body) {
	return E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, title),
		body
	]);
}

var BADGE = {
	ok:   { cls: 'label success', text: null },
	warn: { cls: 'label warning', text: null },
	down: { cls: 'label danger',  text: null }
};

return view.extend({
	load: function() {
		this.radio = null;
		this.radioAt = 0;
		this.radioBusy = false;
		return api.status().then(function(s) {
			return api.radio().then(function(r) {
				this.radio = r;
				this.radioAt = Date.now();
				return s;
			}.bind(this));
		}.bind(this));
	},

	render: function(state) {
		var self = this;

		this.banner = E('div', {});
		this.body   = E('div', {});
		this.radioBox = E('div', {});
		this.footer = E('div', {});

		var actions = E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Actions')),
			E('div', { 'class': 'cbi-page-actions', 'style': 'text-align:left' }, [
				E('button', {
					'class': 'btn cbi-button-action',
					'click': ui.createHandlerFn(this, function() {
						ui.showModal(_('Re-probe the AT port'),
							[ E('p', {}, _('fm160d will drop the current port and scan /dev/ttyUSB* again. Safe to run at any time.')),
							  E('div', { 'class': 'right' }, [
								E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')),
								' ',
								E('button', { 'class': 'btn cbi-button-action', 'click': function() {
									ui.hideModal();
									api.rescan().then(self.refresh.bind(self));
								} }, _('Re-probe'))
							  ]) ]);
					})
				}, _('Re-probe AT port')),
				' ',
				E('button', {
					'class': 'btn cbi-button-action',
					'click': ui.createHandlerFn(this, function() {
						return api.ident().then(self.refresh.bind(self));
					})
				}, _('Re-read module identity')),
				' ',
				E('button', {
					'class': 'btn cbi-button-' + (state.enabled ? 'reset' : 'apply'),
					'click': ui.createHandlerFn(this, function() {
						var next = !state.enabled;
						if (next === false) {
							return ui.showModal(_('Pause FM160 management'),
								[ E('p', {}, _('This stops all automatic polling. The modem keeps working; only this manager goes quiet until you enable it again.')),
								  E('div', { 'class': 'right' }, [
									E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')),
									' ',
									E('button', { 'class': 'btn cbi-button-reset', 'click': function() {
										ui.hideModal();
										api.setEnabled(false).then(self.refresh.bind(self));
									} }, _('Pause'))
								  ]) ]);
						}
						return api.setEnabled(true).then(self.refresh.bind(self));
					})
				}, state.enabled ? _('Pause management') : _('Resume management'))
			])
		]);

		this.container = E('div', {}, [ this.banner, this.body, this.radioBox, actions, this.footer ]);
		this.paint(state);

		poll.add(function() {
			/* Stop as soon as this view is gone, otherwise the poller would
			 * keep the daemon in the fast tier forever. */
			if (!document.body.contains(self.container)) {
				poll.stop();
				return Promise.resolve();
			}
			var chain = api.profile(true).then(api.status);
			/* The radio cross-check costs 5 AT commands per round, so it
			 * refreshes on a 30 s cadence; the daemon snapshot underneath
			 * keeps its 2 s rhythm. */
			if (Date.now() - self.radioAt > 30000)
				chain = chain.then(function(s) {
					return self.captureRadio().then(function() { return s; });
				});
			return chain.then(function(s) {
				self.state = s;
				self.paint(s);
			});
		}, 2);

		return this.container;
	},

	/* One radio capture.  The manual button and the 30 s auto-cadence call
	 * the same function; a failed capture keeps the last good data and the
	 * stamp below it says how old that data is. */
	captureRadio: function() {
		var self = this;

		if (self.radioBusy)
			return Promise.resolve();
		self.radioBusy = true;

		return api.radio().then(function(r) {
			self.radio = r;
			self.radioAt = Date.now();
		}).catch(function() {
		}).then(function() {
			self.radioBusy = false;
		});
	},

	paint: function(state) {
		var health = api.atHealth(state);

		this.banner.innerHTML = '';
		this.banner.appendChild(E('div', {
			'class': 'alert-message ' + (health.level === 'ok' ? 'success' :
						     health.level === 'warn' ? 'warning' : 'danger')
		}, [
			E('strong', {}, health.text),
			state.port ? ' \u2014 ' + _('port') + ': ' + state.port : '',
			state.quiet ? E('span', {}, ' \u2014 ' + _('quiet window') + ': ' + (state.quiet_reason || '') +
				' (' + Math.ceil((state.quiet_left_ms || 0) / 1000) + ' s)') : ''
		]));

		if (!state.enabled)
			this.banner.appendChild(E('div', { 'class': 'alert-message warning' },
				_('Management is paused. No polling is running.')));

		var identity = [
			[ _('Manufacturer', 'fm160 identity field'),   state.manufacturer || '-' ],
			[ _('Model'),          state.model || '-' ],
			[ _('Firmware'),       state.revision || '-' ],
			[ _('IMEI'),           state.imei || '-' ],
			[ _('Serial number'),  state.sn || '-' ],
			[ _('ICCID'),          state.iccid || '-' ],
			[ _('SIM status'),     state.pin_status || '-' ],
			[ _('USB profile'),    state.usb_mode >= 0 ? api.usbModeLabel(state.usb_mode) :
						E('em', {}, _('unknown - not yet read')) ],
			[ _('Queued AT'),      String(state.queue_depth || 0) ],
			[ _('Slowest reply'),  api.fmtAge(state.worst_response_ms) ]
		];

		var csqRaw = api.csqRawRssi(state);
		var rsrc = api.rsrpDbm(state);
		var rsrq = api.rsrqDb(state);
		var ssRsrp = api.isSsRsrp(state);
		var rssi = api.rssiDbm(state);

		var reg = [
			[ _('Operator'),            state.operator || '-' ],
			[ _('LTE registration'),    api.regName(state.cereg) ],
			[ _('NR registration'),     api.regName(state.c5greg) ],
			[ _('Serving RAT'),         state.cell_valid ? api.ratName(state.serving.rat) : '-' ],
			[ _('Band', 'fm160 band column'),                state.cell_valid && api.cellBand(state.serving) ?
						    api.fmtBand(api.cellBand(state.serving)) : '-' ],
			/* 99 is AT+CSQ's "not measurable"; on the 5G path the same field
			 * carries SS-RSRP, so label it as what it actually is. */
			[ ssRsrp ? _('SS-RSRP (AT+CSQ)') : _('RSSI (AT+CSQ)'),
			  csqRaw === 99 ? _('unknown') :
			  ssRsrp       ? api.fmtDbm(rsrc)
				       : (api.fmtNum(csqRaw) + ' (' + api.fmtDbm(rssi) + ')') ],
			[ _('RSRP'),                api.fmtDbm(rsrc) ],
			[ _('RSRQ'),                rsrq === null ? '-' : (rsrq.toFixed(1) + ' dB') ],
			[ _('Last successful AT'),  api.fmtAge(state.last_ok_age_ms) ]
		];

		var traffic = [
			[ _('Interface'), api.trafficOf(state).netdev || _('not detected') ],
			[ _('Received'),  api.fmtBytes(api.trafficOf(state).rx_bytes) ],
			[ _('Sent'),      api.fmtBytes(api.trafficOf(state).tx_bytes) ],
			[ _('Down', 'fm160 traffic direction'),      api.fmtRate(api.trafficOf(state).rx_bps) ],
			[ _('Up', 'fm160 traffic direction'),        api.fmtRate(api.trafficOf(state).tx_bps) ]
		];

		this.body.innerHTML = '';
		this.body.appendChild(section(_('Module'), kv(identity)));
		this.body.appendChild(section(_('Network'), kv(reg)));

		if (state.cell_valid) {
			var c = state.serving;

			this.body.appendChild(section(_('Serving cell'), kv([
				[ _('PLMN'),     api.cellPlmn(c) || '-' ],
				[ _('TAC'),      api.fmtNum(api.cellTac(c)) ],
				[ _('Cell ID', 'fm160 cell field'),  api.fmtNum(api.cellCellId(c)) ],
				[ _('EARFCN'),   api.fmtNum(api.cellEarfcn(c)) ],
				[ _('PCI'),      api.cellPci(c) === null ? '-' : String(api.cellPci(c)) ],
				/* fm160d decodes the bandwidth field to MHz; the raw value is
				 * a 3GPP code point and stays in the status blob for
				 * diagnostics. */
				[ _('Bandwidth'), api.fmtBandwidth(c) ]
			])));
		}

		this.body.appendChild(section(_('Traffic'), kv(traffic)));

		this.paintRadio();

		this.footer.innerHTML = '';
		if (state.port_found && state.at_state === 0)
			this.footer.appendChild(E('div', { 'class': 'cbi-section' }, [
				E('p', { 'class': 'hint' },
				  _('Polling is tiered: registration every 5 s while a page is open, cells every 10 s, and byte counters are read from sysfs so they cost no AT commands at all.'))
			]));
	},

	/* Radio cross-check: operator act, serving band + CA, rates, MCS/QCI.
	 * All values come straight from the module via the AT manual's
	 * queries; "-" means the module gave us nothing to parse, which on
	 * this firmware includes the optional QCI fields. */
	paintRadio: function() {
		this.radioBox.innerHTML = '';

		var head = [
			E('button', {
				'class': 'btn cbi-button',
				'click': ui.createHandlerFn(this, function() {
					api.opLog(_('overview refresh'), _('manual radio cross-check refresh'), _('initiated'));
					return this.captureRadio().then(this.paintRadio.bind(this));
				})
			}, _('Refresh')),
			' '
		];

		if (this.radioAt) {
			head.push(E('span', { 'class': 'hint' },
				_('Last capture:') + ' ' + new Date(this.radioAt).toLocaleString()));
		}

		if (!this.radio) {
			head.push(E('p', { 'class': 'hint' },
				_('No radio capture yet - press Refresh to take one.')));
			this.radioBox.appendChild(section(_('Radio (AT cross-check)'),
				E('div', {}, head)));
			return;
		}

		var state = this.state || {};
		var r = this.radio || {};
		var ca = r.cainfo || {};
		var st = r.statis || {};
		var ci = r.cellinfo || {};
		var cops = r.cops || {};
		var pcc = ca.pcc;
		var rows = [
			[ _('Operator (AT+COPS)'),
				(cops.operator || state.operator || '-') +
				(cops.act !== null && cops.act !== undefined ?
					' (' + (api.copsActName(cops.act) || cops.act) + ')' : '') ],
			[ _('PCC band'), pcc ? '%s, %s MHz, PCI %d'.format(
					pcc.band, pcc.bw !== null ? pcc.bw : '?', pcc.pci) : '-' ],
			[ _('MIMO / modulation'), pcc ?
				_('DL %sx %s / UL %sx %s').format(pcc.dlMimo, pcc.dlMod, pcc.ulMimo, pcc.ulMod) : '-' ],
			[ _('Carrier aggregation'),
				ca.scc && ca.scc.length ? (ca.scc.length + ' SCC') :
				(pcc ? _('none (PCC only)') : '-') ],
			[ _('Module rate (AT+GTSTATIS)'),
				st.rxRate !== undefined && st.rxRate !== null ?
					_('down %s/s / up %s/s').format(api.fmtBytes(st.rxRate), api.fmtBytes(st.txRate)) : '-' ],
			[ _('Session total'),
				st.rxBytes !== undefined && st.rxBytes !== null ?
					api.fmtBytes(st.rxBytes) + ' / ' + api.fmtBytes(st.txBytes) : '-' ]
		];
		if (ca.scc && ca.scc.length) {
			ca.scc.forEach(function(sc) {
				rows.push([ _(sc.id + ' (CA)'),
					_('%s, %s MHz, PCI %d, %s').format(
						sc.band, sc.dlBw !== null ? sc.dlBw : '?', sc.pci,
						sc.state === 'active' ? _('active', 'fm160 ca state') : _('configured')) ]);
			});
		}
		var lci = ci.lte || ci.nr;
		rows.push([ _('CQI / RANK / MCS'), lci ?
			_('CQI %d, %s, DL MCS %d / UL MCS %d').format(lci.cqi, lci.rank, lci.dlmcs, lci.ulmcs) : '-' ]);
		rows.push([ _('QCI'), lci ?
			(lci.txQci !== null || lci.rxQci !== null ?
				_('TX %s / RX %s').format(lci.txQci || '-', lci.rxQci || '-') :
				_('not reported by this firmware')) : '-' ]);

		this.radioBox.appendChild(section(_('Radio (AT cross-check)'),
			E('div', {}, [ E('div', {}, head), kv(rows) ])));
	},

	refresh: function() {
		var self = this;

		return api.status().then(function(s) {
			self.state = s;
			self.paint(s);
		});
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
