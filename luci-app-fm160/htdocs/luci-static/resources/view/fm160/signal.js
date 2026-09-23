'use strict';
'require view';
'require poll';
'require ui';
'require fm160.api as api';

/*
 * Signal page: quality bars, the serving cell and the neighbour list.
 *
 * The cell data comes from a single AT+GTCCINFO? per poll, which returns the
 * serving cell plus up to ten neighbours with RSRP/RSRQ/SINR.  The page only
 * pulls it while it is open - the daemon drops the cell tier back to disabled as
 * soon as the foreground hold expires.
 */

function scale(value, min, max) {
	if (value === undefined || value === null)
		return null;
	var pct = (value - min) / (max - min) * 100;
	return Math.max(0, Math.min(100, pct));
}

function bar(pct, level) {
	return E('div', { 'class': 'cbi-progressbar', 'style': 'height:10px' },
		E('div', { 'style': 'width:' + (pct === null ? 0 : pct).toFixed(0) + '%;height:100%;background:' +
			(level === 'bad' ? '#e24b4a' : level === 'fair' ? '#ef9f27' : '#639922') }));
}

function metric(title, value, sub, pct) {
	var level = pct === null ? 'bad' : pct >= 50 ? 'good' : pct >= 20 ? 'fair' : 'bad';

	return E('div', { 'class': 'cbi-section', 'style': 'display:inline-block;width:23%;min-width:140px;vertical-align:top;margin-right:1%' }, [
		E('h3', {}, title),
		E('div', { 'style': 'font-size:20px;margin-bottom:4px' }, value),
		bar(pct, level),
		E('div', { 'class': 'hint' }, sub || '')
	]);
}

return view.extend({
	load: function() {
		this.radio = null;
		this.radioAt = 0;
		return Promise.all([ api.status(), api.radio() ]).then(function(r) {
			this.radio = r[1];
			return r[0];
		}.bind(this));
	},

	render: function(state) {
		var self = this;

		this.banner = E('div', {});
		this.metrics = E('div', {});
		this.tables = E('div', {});
		this.scanBox = E('div', {});
		this.container = E('div', {}, [ this.banner, this.metrics, this.tables, this.scanBox ]);
		this.scanData = null;
		this.scanStamp = null;
		this.scanBusy = false;
		this.paint(state);
		this.paintScan();

		poll.add(function() {
			if (!document.body.contains(self.container)) {
				poll.stop();
				return Promise.resolve();
			}
			/* Link adaptation (AT+GTCELLINFO) costs 5 AT commands per round,
			 * so it refreshes on a 30 s cadence while the fast metrics keep
			 * their 2 s rhythm. */
			var wantRadio = (Date.now() - self.radioAt) > 30000;
			var chain = api.profile(true).then(api.status);
			if (wantRadio) {
				chain = chain.then(function(s) {
					return api.radio().then(function(r) {
						self.radio = r;
						self.radioAt = Date.now();
						return s;
					});
				});
			}
			return chain.then(function(s) {
				self.paint(s);
			});
		}, 2);

		/* Neighbour cell scan shares the 30 s cadence: one cellscan per
		 * radio round, not one per 2 s metrics round. */
		poll.add(function() {
			if (!document.body.contains(self.container)) {
				poll.stop();
				return Promise.resolve();
			}
			return self.scan();
		}, 30);

		return this.container;
	},

	scan: function() {
		var self = this;

		if (self.scanBusy)
			return Promise.resolve();
		self.scanBusy = true;

		return api.cellscan().then(function(res) {
			self.scanData = res;
			self.scanStamp = new Date();
			self.paintScan();
		}).catch(function() {
		}).then(function() {
			self.scanBusy = false;
		});
	},

	paintScan: function() {
		var self = this;

		this.scanBox.innerHTML = '';

		var status = this.scanStamp
			? (_('Last capture:') + ' ' + this.scanStamp.toLocaleString() + ' — ' +
			   (this.scanData ? this.scanData.neighbors.length + ' ' + _('neighbour cells') : '-'))
			: _('Auto-capturing every 30 s while this page is open…');

		var box = E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Neighbour cells (live AT)')),
			E('p', { 'class': 'hint' },
			  _('Live AT+GTCCINFO? neighbour list and AT+GTCAINFO? carrier aggregation, captured automatically; press the button for an immediate pass.')),
			E('div', {}, [
				E('button', {
					'class': 'btn cbi-button-action',
					'click': ui.createHandlerFn(this, function() {
						api.opLog(_('cell scan'), _('manual trigger from the signal page'), _('initiated'));
						return this.scan();
					})
				}, _('Search cells now')),
				' ',
				E('span', { 'class': 'hint' }, status)
			])
		]);

		if (this.scanData && this.scanData.neighbors.length) {
			box.appendChild(E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, _('RAT')),
					E('th', { 'class': 'th' }, _('EARFCN / NRARFN')),
					E('th', { 'class': 'th' }, _('PCI')),
					E('th', { 'class': 'th' }, _('RSRP')),
					E('th', { 'class': 'th' }, _('RSRQ'))
				])
			].concat(this.scanData.neighbors.map(function(n) {
				return E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, n.nr ? 'NR' : 'LTE'),
					E('td', { 'class': 'td' }, String(n.arfcn)),
					E('td', { 'class': 'td' }, String(n.pci)),
					E('td', { 'class': 'td' }, n.rsrpRaw !== null ? (n.rsrpRaw + ' ' + _('dBm (raw)')) : '-'),
					E('td', { 'class': 'td' }, n.rsrqRaw !== null ? (n.rsrqRaw + ' ' + _('dB (raw)')) : '-')
				]);
			}))));
		} else if (this.scanData) {
			box.appendChild(E('p', { 'class': 'hint' },
				_('No neighbour cells reported this capture.')));
		}

		if (this.scanData && this.scanData.cainfo && this.scanData.cainfo.pcc) {
			var pcc = this.scanData.cainfo.pcc;
			box.appendChild(E('p', { 'class': 'hint' }, [
				_('PCC:'), ' ', pcc.band, ' / ' + (pcc.bw || '-') + ' MHz / PCI ' + pcc.pci,
				this.scanData.cainfo.scc.length
					? ' / ' + this.scanData.cainfo.scc.length + ' ' + _('SCC')
					: ' / ' + _('no carrier aggregation')
			]));
		}

		this.scanBox.appendChild(box);
	},

	paint: function(state) {
		/* Field names live in api.js, not here: the daemon casts its "not
		 * reported" sentinel to uint32, so which test rejects it depends on the
		 * field, and a view that guesses will print 4293967296 as a signal
		 * strength.  Each accessor returns a number or null. */
		var rssi = api.rssiDbm(state);
		var rsrp = api.rsrpDbm(state);
		var rsrq = api.rsrqDb(state);
		/* The serving cell's SINR is the one worth showing; CESQ's SS-SINR is
		 * only a fallback for before the first cell poll has landed. */
		var sinr = state.cell_valid ? api.cellSinrDb(state.serving) : api.cesqSinrDb(state);

		this.banner.innerHTML = '';
		if (!state.cell_valid)
			this.banner.appendChild(E('div', { 'class': 'alert-message warning' },
				_('No cell information yet. Cell data is only polled while this page is open - give it a few seconds.')));

		this.metrics.innerHTML = '';
		this.metrics.appendChild(metric('RSSI',
			api.fmtDbm(rssi),
			api.isSsRsrp(state) ? _('not applicable - CSQ carries SS-RSRP on this link')
					    : _('from AT+CSQ'),
			scale(rssi, -113, -51)));
		this.metrics.appendChild(metric('RSRP',
			api.fmtDbm(rsrp),
			_('reference signal received power'),
			scale(rsrp, -140, -70)));
		this.metrics.appendChild(metric('RSRQ',
			rsrq === null ? '-' : (rsrq.toFixed(1) + ' dB'),
			_('reference signal received quality'),
			rsrq === null ? null : scale(rsrq, -20, -3)));
		this.metrics.appendChild(metric('SINR',
			sinr === null ? '-' : (sinr.toFixed(1) + ' dB'),
			_('signal to interference plus noise'),
			sinr === null ? null : scale(sinr, -20, 30)));

		this.tables.innerHTML = '';

		if (state.cell_valid) {
			var c = state.serving;
			var crsrp = api.cellRsrp(c), crsrq = api.cellRsrqDb(c), csinr = api.cellSinrDb(c);
			var cpci = api.cellPci(c), cband = api.cellBand(c);

			this.tables.appendChild(E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Serving cell')),
				E('table', { 'class': 'table' }, [
					E('tr', { 'class': 'tr' }, [
						E('th', { 'class': 'th' }, _('RAT')),
						E('th', { 'class': 'th' }, _('PLMN')),
						E('th', { 'class': 'th' }, 'TAC'),
						E('th', { 'class': 'th' }, _('Cell ID', 'fm160 cell field')),
						E('th', { 'class': 'th' }, 'EARFCN'),
						E('th', { 'class': 'th' }, 'PCI'),
						E('th', { 'class': 'th' }, _('Band', 'fm160 band column')),
						E('th', { 'class': 'th' }, 'RSRP'),
						E('th', { 'class': 'th' }, 'RSRQ'),
						E('th', { 'class': 'th' }, 'SINR')
					]),
					E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td' }, api.ratName(c.rat)),
						E('td', { 'class': 'td' }, api.cellPlmn(c) || '-'),
						E('td', { 'class': 'td' }, api.fmtNum(api.cellTac(c))),
						E('td', { 'class': 'td' }, api.fmtNum(api.cellCellId(c))),
						E('td', { 'class': 'td' }, api.fmtNum(api.cellEarfcn(c))),
						E('td', { 'class': 'td' }, cpci === null ? '-' : String(cpci)),
						E('td', { 'class': 'td' }, api.fmtBand(cband)),
						E('td', { 'class': 'td' }, api.fmtDbm(crsrp)),
						E('td', { 'class': 'td' }, crsrq === null ? '-' : (crsrq.toFixed(1) + ' dB')),
						E('td', { 'class': 'td' }, csinr === null ? '-' : (csinr.toFixed(1) + ' dB'))
					])
				])
			]));
		}

		/* Link adaptation / bearer QoS from AT+GTCELLINFO (§5.20).  QCI is an
		 * optional bracketed field and this firmware omits it - show that
		 * honestly instead of guessing. */
		var ci = (this.radio && this.radio.cellinfo) || null;
		var lci = ci && (ci.lte || ci.nr);
		if (lci) {
			this.tables.appendChild(E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Link adaptation (AT+GTCELLINFO)')),
				E('table', { 'class': 'table' }, [
					E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td', 'style': 'width:34%' }, _('CQI')),
						E('td', { 'class': 'td' }, String(lci.cqi))
					]),
					E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td' }, _('Rank')),
						E('td', { 'class': 'td' }, lci.rank)
					]),
					E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td' }, _('MCS DL / UL')),
						E('td', { 'class': 'td' }, '%d / %d'.format(lci.dlmcs, lci.ulmcs))
					]),
					E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td' }, _('QCI TX / RX')),
						E('td', { 'class': 'td' },
							(lci.txQci !== null || lci.rxQci !== null) ?
								'%s / %s'.format(lci.txQci || '-', lci.rxQci || '-') :
								_('not reported by this firmware'))
					])
				])
			]));
		}

		var neigh = state.neighbours || [];

		if (neigh.length) {
			var rows = [ E('tr', { 'class': 'tr' }, [
				E('th', { 'class': 'th' }, '#'),
				E('th', { 'class': 'th' }, _('RAT')),
				E('th', { 'class': 'th' }, 'EARFCN'),
				E('th', { 'class': 'th' }, 'PCI'),
				E('th', { 'class': 'th' }, _('Band', 'fm160 band column')),
				E('th', { 'class': 'th' }, 'RSRP'),
				E('th', { 'class': 'th' }, 'RSRQ'),
				E('th', { 'class': 'th' }, 'SINR')
			]) ];

			neigh.forEach(function(n, i) {
				var npci = api.cellPci(n), nrsrq = api.cellRsrqDb(n), nsinr = api.cellSinrDb(n);

				rows.push(E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, String(i + 1)),
					E('td', { 'class': 'td' }, api.ratName(n.rat)),
					E('td', { 'class': 'td' }, api.fmtNum(api.cellEarfcn(n))),
					E('td', { 'class': 'td' }, npci === null ? '-' : String(npci)),
					E('td', { 'class': 'td' }, api.fmtBand(api.cellBand(n))),
					E('td', { 'class': 'td' }, api.fmtDbm(api.cellRsrp(n))),
					E('td', { 'class': 'td' }, nrsrq === null ? '-' : (nrsrq.toFixed(1) + ' dB')),
					E('td', { 'class': 'td' }, nsinr === null ? '-' : (nsinr.toFixed(1) + ' dB'))
				]));
			});

			this.tables.appendChild(E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Neighbour cells')),
				E('table', { 'class': 'table' }, rows),
				E('p', { 'class': 'hint' }, [
					_('Band locking, cell locking and carrier aggregation live on the'), ' ',
					E('a', { 'href': L.url('admin/fm160/cells') }, _('cells and locking')),
					' ',
					_('page.')
				])
			]));
		}
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
