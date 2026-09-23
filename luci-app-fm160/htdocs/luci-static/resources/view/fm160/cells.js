'use strict';
'require view';
'require poll';
'require ui';
'require fm160.api as api';

/*
 * Cells / band lock page (M4).
 *
 * Three settings live here and they are NOT equally safe:
 *
 *   band lock    AT+GTACT      persistent (EFS).  Changing it drops the current
 *                              cell, so the daemon opens a quiet window and the
 *                              poll tiers stay off the modem while it
 *                              re-registers.
 *   cell lock    AT+GTCELLLOCK persistent (EFS) AND only takes effect after a
 *                              UE reset, which fm160d deliberately never
 *                              performs - see the note in the page.  Writing it
 *                              without a reset changes nothing on the air, so
 *                              the UI says so instead of pretending success.
 *   CA view      AT+GTCAINFO?  read-only.
 *
 * Two rules shape the whole page:
 *
 *   1. The capability enumerations are the licence to write.  fm160d refuses
 *      both writes when AT+GTACT=? / AT+GTCELLLOCK=? never answered, so the
 *      controls are disabled here rather than left to produce a command that
 *      is guaranteed to be rejected.
 *
 *   2. Never re-implement the band encoding in JavaScript.  AT+GTACT writes the
 *      RAT-prefixed raw token (3 is ambiguous, 103 is not), and the modem's own
 *      AT+GTACT=? answer maps every token it accepts to a band number.  The
 *      serving cell only reports the decoded band, so "lock to this band" is a
 *      LOOKUP in that answer - and when the lookup fails, the button is not
 *      offered instead of a token being guessed.
 */

function confirmPrompt(title, lines, okLabel) {
	return new Promise(function(resolve) {
		ui.showModal(title, [
			E('div', {}, lines.map(function(l) { return E('p', {}, l); })),
			E('div', { 'class': 'right' }, [
				E('button', {
					'class': 'btn',
					'click': function(ev) {
						ev.currentTarget.blur();
						ui.hideModal();
						resolve(false);
					}
				}, _('Cancel')),
				' ',
				E('button', {
					'class': 'btn cbi-button-negative',
					'click': function(ev) {
						ev.currentTarget.blur();
						ui.hideModal();
						resolve(true);
					}
				}, okLabel || _('Write to modem'))
			])
		]);
	});
}

/* Turn the deferred reply of a write into a notification.  "ok" here means the
 * write AND the read-back both completed - the daemon only reports ok once it
 * has seen the setting come back as the value it just wrote. */
function reportWrite(res, what) {
	var status = (res && res.status) || 'unknown';

	if (status === 'ok')
		ui.addNotification(null, E('p', {}, what + ' ' + _('confirmed by read-back.')), 'info');
	else
		ui.addNotification(null, E('p', {},
			what + ' ' + _('failed') + ': ' + status + ' - ' +
			((res && res.response) || _('(no response)'))), 'danger');
}

/* A small chip for one band token. */
function bandChip(entry, onRemove) {
	var label = api.bandLabel(entry);

	return E('span', {
		'class': 'ifacebox',
		'style': 'display:inline-block;margin:0 4px 4px 0;padding:2px 8px;border-radius:10px;' +
			 'border:1px solid rgba(0,0,0,0.2)',
		'title': _('raw token') + ' ' + api.bandRaw(entry)
	}, [
		label,
		onRemove ? E('span', {
			'style': 'margin-left:6px;cursor:pointer;opacity:0.7',
			'click': onRemove
		}, '\u00d7') : ''
	]);
}

return view.extend({
	load: function() {
		return api.status();
	},

	render: function(state) {
		var self = this;

		this.container = E('div', {});
		this.scanData = null;
		this.scanStamp = null;
		this.scanBusy = false;
		this.paint(state);

		/* Cell data (neighbour list) is only polled while a page that needs it
		 * is open, so this page has to hold the foreground too - it is the page
		 * that offers to lock onto those cells. */
		poll.add(function() {
			if (!document.body.contains(self.container)) {
				poll.stop();
				return Promise.resolve();
			}
			return api.profile(true).then(api.status).then(function(s) {
				self.paint(s);
			});
		}, 2);

		/* Live AT scan: serving cell + neighbours + CA, every 30 s while
		 * this page is open.  The manual button calls the same capture. */
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
			/* a failed scan keeps the previous table; the stamp line says
			 * nothing new arrived, which is the honest state */
		}).then(function() {
			self.scanBusy = false;
		});
	},

	paint: function(state) {
		this.state = state;
		this.container.innerHTML = '';

		this.container.appendChild(this.renderBanner(state));
		this.container.appendChild(this.renderScan());
		this.container.appendChild(this.renderBands(state));
		this.container.appendChild(this.renderBandLock(state));
		this.container.appendChild(this.renderLockTargets(state));
		this.container.appendChild(this.renderCellLock(state));
		this.container.appendChild(this.renderCa(state));
	},

	/* --- live cell scan (manual + auto) ------------------------------ */

	renderScan: function() {
		var self = this;

		this.scanStatus = E('p', { 'class': 'hint' },
			_('Auto-capturing every 30 s while this page is open…'));
		this.scanBody = E('div', {});

		this.scanBtn = E('button', {
			'class': 'btn cbi-button-action',
			'click': ui.createHandlerFn(this, function() {
				api.opLog(_('cell scan'), _('manual trigger from the cells page'), _('initiated'));
				return this.scan();
			})
		}, _('Search cells now'));

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Cell scan (live AT)')),
			E('p', { 'class': 'hint' },
			  _('Sends AT+GTCCINFO? (serving cell and up to ten LTE / NR neighbours) and AT+GTCAINFO? (carrier aggregation) live, instead of relying on the daemon snapshot. AT+GTCELLSCAN is not used: on this firmware it blocks the AT channel for ~50 s and returns nothing.')),
			E('div', {}, [ this.scanBtn, ' ', this.scanStatus ]),
			this.scanBody
		]);
	},

	paintScan: function() {
		var d = this.scanData;

		if (!d) {
			this.scanStatus.textContent = _('No scan result yet.');
			return;
		}

		this.scanStatus.textContent = _('Last capture:') + ' ' +
			this.scanStamp.toLocaleString() + ' — ' +
			(d.neighbors.length + ' ' + _('neighbour cells'));

		this.scanBody.innerHTML = '';

		if (d.neighbors.length) {
			var rows = d.neighbors.map(function(n) {
				return E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, n.nr ? 'NR' : 'LTE'),
					E('td', { 'class': 'td' }, String(n.arfcn)),
					E('td', { 'class': 'td' }, String(n.pci)),
					E('td', { 'class': 'td' }, n.bwCode ? (n.bwCode + ' ' + _('raw bw code')) : '-'),
					E('td', { 'class': 'td' }, n.rsrpRaw !== null ? (n.rsrpRaw + ' ' + _('dBm (raw)')) : '-'),
					E('td', { 'class': 'td' }, n.rsrqRaw !== null ? (n.rsrqRaw + ' ' + _('dB (raw)')) : '-')
				]);
			});

			this.scanBody.appendChild(E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, _('RAT')),
					E('th', { 'class': 'th' }, _('EARFCN / NRARFN')),
					E('th', { 'class': 'th' }, _('PCI')),
					E('th', { 'class': 'th' }, _('Bandwidth')),
					E('th', { 'class': 'th' }, _('RSRP')),
					E('th', { 'class': 'th' }, _('RSRQ'))
				]),
				rows
			]));
		} else {
			this.scanBody.appendChild(E('p', { 'class': 'hint' },
				_('No neighbour cells reported this capture.')));
		}

		if (d.cainfo && d.cainfo.pcc) {
			var ca = [ E('p', { 'class': 'hint' }, [
				_('PCC:'), ' ', d.cainfo.pcc.band,
				' / ' + (d.cainfo.pcc.bw || '-') + ' MHz',
				' / PCI ' + d.cainfo.pcc.pci,
				d.cainfo.scc.length
					? ' / ' + d.cainfo.scc.length + ' ' + _('SCC')
					: ' / ' + _('no carrier aggregation')
			]) ];
			this.scanBody.appendChild(E('div', {}, ca));
		}
	},

	/* --- banner ----------------------------------------------------- */

	renderBanner: function(state) {
		var health = api.atHealth(state);
		var caps = api.bandCapsOf(state), clcaps = api.celllockCapsOf(state);
		var out = E('div', {});

		out.appendChild(E('div', { 'class': 'alert-message ' + (health.level === 'ok' ? 'success' : 'warning') },
			health.text));

		if (state.m4 === undefined)
			out.appendChild(E('div', { 'class': 'alert-message warning' },
				_('fm160d does not publish the locking data - the installed daemon is older than this page.')));
		else if (!caps.valid || !clcaps.valid)
			out.appendChild(E('div', { 'class': 'alert-message warning' },
				_('The modem has not answered the capability queries (AT+GTACT=? / AT+GTCELLLOCK=?) yet, so writing is disabled. Both settings are persistent, and a value the modem does not accept cannot be taken back by guessing.')));

		return out;
	},

	/* --- current restriction ---------------------------------------- */

	renderBands: function(state) {
		var b = api.bandsOf(state);
		var all = api.bandListOf(b);
		var self = this;
		var body = E('div', {});

		if (!b.valid) {
			body.appendChild(E('p', { 'class': 'hint' },
				_('No band data yet. It is polled in the background, so give it a few seconds.')));
		}
		else if (all.length === 0) {
			body.appendChild(E('p', {},
				E('strong', {}, b.auto_seen
					? _('No band restriction: automatic band selection is active.')
					: _('The modem reported no bands at all.'))));
		}
		else {
			body.appendChild(E('p', {},
				_('The modem is currently restricted to %d bands.').format(all.length)));
			body.appendChild(E('div', {}, all.map(function(e) { return bandChip(e, null); })));
		}

		if (b.auto_seen && all.length)
			body.appendChild(E('p', { 'class': 'hint' },
				_('The answer also contained the "automatic band selection" marker, which normally means "unrestricted".')));

		/* A non-zero count here means the firmware used a token that matches no
		 * documented encoding.  Showing that is the whole point of counting it
		 * instead of dropping it. */
		if (b.unknown > 0)
			body.appendChild(E('p', { 'class': 'alert-message warning' },
				_('%d band token(s) could not be decoded and are shown elsewhere or omitted. The daemon is talking to a firmware whose encoding differs from the documented one, so locking is best avoided until that is understood.').format(b.unknown)));

		if (b.rat !== undefined)
			body.appendChild(E('p', { 'class': 'hint' },
				_('RAT preference: %s (fallback %s, then %s)')
					.format(api.ratName(b.rat), api.ratName(b.pref1), api.ratName(b.pref2))));

		/* Turning the restriction off is a write like any other, and it is the
		 * one write that is always meaningful: the "automatic band selection"
		 * token is 0. */
		if (b.valid && all.length) {
			body.appendChild(E('button', {
				'class': 'btn cbi-button',
				'disabled': api.bandCapsOf(state).valid ? null : 'disabled',
				'click': ui.createHandlerFn(this, function() {
					return confirmPrompt(_('Remove the band restriction?'), [
						_('This writes %s, i.e. automatic band selection for every supported RAT.').format('AT+GTACT=,,,0'),
						_('The modem will re-scan the whole band set and the current connection will drop for a few seconds.')
					]).then(function(ok) {
						if (!ok)
							return;
						return self.writeBands('0', _('Remove band restriction'));
					});
				})
			}, _('Restore automatic band selection')));
		}

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Current band restriction')),
			body
		]);
	},

	/* --- band lock -------------------------------------------------- */

	renderBandLock: function(state) {
		var caps = api.bandCapsOf(state);
		var self = this;
		var body = E('div', {});

		if (!caps.valid) {
			body.appendChild(E('p', { 'class': 'hint' },
				_('Locking is disabled until the modem answers AT+GTACT=?.')));
			return E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Band lock')),
				body
			]);
		}

		/* The nine groups come back in a fixed order; three of them are
		 * routinely empty on this hardware, and an empty group is shown as
		 * "not supported" rather than hidden - the manual lists bands for all
		 * of them, so silence would read as a bug. */
		var groups = [
			{ key: 'lte',  title: 'LTE', rat: 4 },
			{ key: 'nr',   title: 'NR / 5G', rat: 9 },
			{ key: 'umts', title: 'UMTS', rat: 2 }
		];

		/* The page is repainted on every poll (2 s), which rebuilds these
		 * controls from scratch.  The selection therefore lives on the view and
		 * the checkboxes are seeded from it, rather than the other way round -
		 * otherwise a poll would silently clear whatever the user had ticked
		 * between two renders. */
		if (!this.selected)
			this.selected = {};

		groups.forEach(function(g) {
			var list = caps[g.key] || [];

			body.appendChild(E('h4', {}, g.title + ' (' + list.length + ')'));

			if (!list.length) {
				body.appendChild(E('p', { 'class': 'hint' },
					_('This modem reports no bands for this RAT.')));
				return;
			}

			body.appendChild(E('div', {}, list.map(function(e) {
				if (!api.bandUsable(e))
					return E('span', { 'class': 'hint', 'style': 'margin-right:8px' },
						_('raw', 'fm160 raw marker') + ' ' + e.raw + ' (' + _('not decodable') + ')');

				var id = 'fm160-band-' + e.rat + '-' + e.raw;

				return E('label', {
					'style': 'display:inline-block;margin:0 10px 4px 0;white-space:nowrap',
					'title': _('raw token') + ' ' + e.raw
				}, [
					E('input', {
						'type': 'checkbox',
						'id': id,
						'class': 'fm160-band-cb',
						'data-raw': String(e.raw),
						/* setAttribute('checked','') is what marks a freshly
						 * built box as ticked; null means "leave it alone". */
						'checked': self.selected[e.raw] ? '' : null,
						'change': function(ev) {
							if (ev.currentTarget.checked)
								self.selected[e.raw] = e;
							else
								delete self.selected[e.raw];
						}
					}),
					' ',
					api.bandLabel(e)
				]);
			})));
		});

		body.appendChild(E('p', { 'class': 'hint' },
			_('Only bands this modem reports are listed. That is deliberate: AT+GTACT writes the raw token, and a token the modem did not advertise is a guess. LTE and NR token numbers are not band numbers - the raw value is shown as a tooltip.')));

		body.appendChild(E('div', { 'style': 'margin-top:8px' }, [
			E('button', {
				'class': 'btn cbi-button-action',
				'click': ui.createHandlerFn(this, function() {
					var keys = Object.keys(self.selected);

					if (!keys.length) {
						ui.addNotification(null, E('p', {}, _('Select at least one band first.')), 'warning');
						return Promise.resolve();
					}

					var entries = keys.map(function(k) { return self.selected[k]; });
					var csv = api.bandCsv(entries);

					return confirmPrompt(_('Lock to the selected bands?'), [
						_('This writes %s and is stored in the modem.').format('AT+GTACT=,,,' + csv),
						_('Selected: %s').format(entries.map(api.bandLabel).join(', ')),
						_('The RAT preference is left untouched - only the band list changes.'),
						_('The current connection drops while the modem re-scans.')
					]).then(function(ok) {
						if (!ok)
							return;
						return self.writeBands(csv, _('Band lock'));
					});
				})
			}, _('Lock to the selected bands')),
			' ',
			E('button', {
				'class': 'btn cbi-button',
				'click': ui.createHandlerFn(this, function() {
					/* Repaint from the view's own state: the checkboxes are
					 * derived from self.selected, so clearing it and
					 * repainting is the whole operation. */
					self.selected = {};
					self.paint(self.state);
					return Promise.resolve();
				})
			}, _('Clear selection'))
		]));

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Band lock')),
			body
		]);
	},

	/* --- cell lock -------------------------------------------------- */

	renderCellLock: function(state) {
		var l = api.celllockOf(state);
		var caps = api.celllockCapsOf(state);
		var body = E('div', {});

		if (l.valid) {
			if (!l.enabled) {
				body.appendChild(E('p', {}, _('Cell lock is off.')));
			}
			else {
				var rows = [
					E('tr', { 'class': 'tr' }, [
						E('th', { 'class': 'th' }, _('RAT')),
						E('th', { 'class': 'th' }, _('Lock on')),
						E('th', { 'class': 'th' }, 'EARFCN / NARFCN'),
						E('th', { 'class': 'th' }, 'PCI'),
						E('th', { 'class': 'th' }, 'SCS'),
						E('th', { 'class': 'th' }, _('NR band'))
					]),
					E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td' }, api.celllockRatName(l.rat)),
						E('td', { 'class': 'td' }, api.celllockTypeName(l.type)),
						E('td', { 'class': 'td' }, api.fmtNum(api.celllockEarfcn(l))),
						E('td', { 'class': 'td' }, api.fmtNum(api.celllockPci(l))),
						E('td', { 'class': 'td' }, api.fmtNum(api.celllockScs(l))),
						E('td', { 'class': 'td' }, api.fmtBand(api.celllockNrband(l)))
					])
				];

				body.appendChild(E('table', { 'class': 'table' }, rows));
				/* The pending-reset state is not a cosmetic detail: the lock has
				 * no effect on the air until the UE is reset, and this project
				 * never resets it.  Say it plainly. */
				body.appendChild(E('p', { 'class': 'alert-message warning' },
					_('The modem stores this in its own flash and only applies it after a UE reset. This page never resets the modem - do not switch it to a mode without an AT port to "force" it. The value is in place and will take effect on the next power cycle.')));
			}
		}
		else {
			body.appendChild(E('p', { 'class': 'hint' },
				_('Cell lock state has not been read yet.')));
		}

		body.appendChild(E('p', { 'class': 'hint' },
			_('Available locks are listed below, one row per cell the modem can currently see.')));

		var self = this;

		var disable = E('button', {
			'class': 'btn cbi-button',
			'disabled': (caps.valid && l.valid && l.enabled) ? null : 'disabled',
			'click': ui.createHandlerFn(this, function() {
				return confirmPrompt(_('Disable cell lock?'), [
					_('This writes AT+GTCELLLOCK=0.'),
					_('Like the lock itself, the change is stored immediately but only takes effect after a UE reset.')
				]).then(function(ok) {
					if (!ok)
						return;
					return self.writeCellLock({ mode: 0 }, _('Disable cell lock'));
				});
			})
		}, _('Disable cell lock'));

		var manual = E('div', { 'style': 'margin-top:8px' }, [
			E('p', { 'class': 'hint' },
				_('Manual entry. The modem accepts %s and %s for the mode; its own capability answer lists a third value that the manual does not define, and this page will not write it.')
					.format('0 (' + _('off', 'fm160 off state') + ')', '1 (' + _('on', 'fm160 on state') + ')')),
			disable
		]);

		if (api.celllockModeUndocumented(state))
			manual.insertBefore(E('p', { 'class': 'alert-message warning' },
				_('AT+GTCELLLOCK=? reports a mode value beyond the documented 0 and 1. It is recorded in the status blob as evidence and deliberately not usable here.')), manual.firstChild);

		body.appendChild(manual);

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Cell lock')),
			body
		]);
	},

	/* --- lock targets ----------------------------------------------- */

	/*
	 * The serving cell and every neighbour that reports an EARFCN, each with a
	 * "lock to this cell" action.
	 *
	 * Cells without an EARFCN are skipped rather than offered with a filler
	 * value: AT+GTCELLLOCK needs the frequency, and inventing one would lock
	 * the modem to something that was never measured.
	 *
	 * The prompt lists the fields that will be written instead of assembling an
	 * AT+GTCELLLOCK string.  The daemon owns that grammar (nesting, "-1 means
	 * absent", NR-only trailing fields); a second copy here could only drift.
	 */
	renderLockTargets: function(state) {
		var self = this;
		var rows = [ E('tr', { 'class': 'tr' }, [
			E('th', { 'class': 'th' }, _('Cell')),
			E('th', { 'class': 'th' }, _('RAT')),
			E('th', { 'class': 'th' }, 'EARFCN'),
			E('th', { 'class': 'th' }, 'PCI'),
			E('th', { 'class': 'th' }, _('Band', 'fm160 band column')),
			E('th', { 'class': 'th' }, 'RSRP'),
			E('th', { 'class': 'th' }, _('Action'))
		]) ];

		var targets = [];

		if (state.cell_valid)
			targets.push({ cell: state.serving, name: _('Serving cell') });
		(state.neighbours || []).forEach(function(n, i) {
			targets.push({ cell: n, name: _('Neighbour') + ' ' + (i + 1) });
		});

		targets.filter(function(t) { return !!api.cellEarfcn(t.cell); })
		       .forEach(function(t) {
			var c = t.cell;
			var pci = api.cellPci(c);
			var scsEl = null;

			/* NR needs the subcarrier spacing to identify a cell on some
			 * frequencies; it is optional in the grammar, so "unspecified" is
			 * a real choice and the default. */
			if (c.rat === 9) {
				scsEl = E('select', { 'class': 'cbi-input-select' }, [
					E('option', { 'value': '' }, _('unspecified')),
					E('option', { 'value': '0' }, '15 kHz'),
					E('option', { 'value': '1' }, '30 kHz')
				]);
			}

			rows.push(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, t.name),
				E('td', { 'class': 'td' }, api.ratName(c.rat)),
				E('td', { 'class': 'td' }, api.fmtNum(api.cellEarfcn(c))),
				E('td', { 'class': 'td' }, pci === null ? '-' : String(pci)),
				E('td', { 'class': 'td' }, api.fmtBand(api.cellBand(c))),
				E('td', { 'class': 'td' }, api.fmtDbm(api.cellRsrp(c))),
				E('td', { 'class': 'td' }, [
					scsEl, ' ',
					E('button', {
						'class': 'btn cbi-button-action',
						'click': ui.createHandlerFn(self, function() {
							var params = {
								mode: 1,
								rat: api.celllockRatOf(c.rat),
								/* "by PCI" only when the modem actually
								 * reported a PCI for this cell. */
								type: pci === null ? 1 : 0,
								earfcn: api.cellEarfcn(c),
								pci: pci === null ? undefined : pci,
								scs: (scsEl && scsEl.value !== '') ?
									Number(scsEl.value) : undefined,
								nrband: undefined
							};

							return self.lockToCell(t.name, params);
						})
					}, _('Lock'))
				])
			]));
		});

		if (rows.length < 2)
			return E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Cells available for locking')),
				E('p', { 'class': 'hint' },
					_('No cell with a frequency has been measured yet. Cell information is only polled while a signal page is open - open the signal page, or wait for the next poll on this one.'))
			]);

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Cells available for locking')),
			E('p', { 'class': 'hint' },
				_('Locking pins the modem to one cell. It is stored in the modem and only takes effect after a UE reset. Use the band lock instead when the goal is simply to stop the modem from using a particular band.')),
			E('table', { 'class': 'table' }, rows)
		]);
	},

	lockToCell: function(name, params) {
		var self = this;
		var caps = api.celllockCapsOf(this.state);

		if (!caps.valid)
			return Promise.resolve();

		var lines = [
			_('The following values will be written to the modem:'),
			E('div', { 'style': 'font-family:monospace;margin:6px 0' }, [
				E('div', {}, 'mode   = 1 (' + _('on', 'fm160 on state') + ')'),
				E('div', {}, 'rat    = ' + params.rat + ' (' + api.celllockRatName(params.rat) + ')'),
				E('div', {}, 'type   = ' + params.type + ' (' + api.celllockTypeName(params.type) + ')'),
				E('div', {}, 'earfcn = ' + params.earfcn),
				E('div', {}, 'pci    = ' + (params.pci === undefined ? _('unspecified') : params.pci)),
				E('div', {}, 'scs    = ' + (params.scs === undefined ? _('unspecified') : params.scs)),
				E('div', {}, 'nrband = ' + _('unspecified'))
			]),
			_('This is stored in the modem\'s own flash and only applied after a UE reset, which this page never performs.')
		];

		return confirmPrompt(_('Lock to %s?').format(name), lines).then(function(ok) {
			if (!ok)
				return;
			return self.writeCellLock(params, _('Lock to %s').format(name));
		});
	},

	/* --- carrier aggregation ---------------------------------------- */

	renderCa: function(state) {
		var ca = api.caOf(state);
		var body = E('div', {});

		if (!ca.valid) {
			/* A bare OK with no data line is what the modem answers when it is
			 * not aggregated - which is also what it answers with no SIM.  So
			 * this is the normal empty state, not an error. */
			body.appendChild(E('p', { 'class': 'hint' },
				_('The modem reports no carrier aggregation. Note that a modem with no active data connection answers this query with a bare OK, so an empty table here is normal.')));
		}
		else {
			var rows = [
				E('tr', { 'class': 'tr' }, [
					E('th', { 'class': 'th' }, _('Cell')),
					E('th', { 'class': 'th' }, _('Band', 'fm160 band column')),
					E('th', { 'class': 'th' }, 'PCI'),
					E('th', { 'class': 'th' }, _('Frequency')),
					E('th', { 'class': 'th' }, _('State')),
					E('th', { 'class': 'th' }, _('DL width')),
					E('th', { 'class': 'th' }, _('DL MIMO')),
					E('th', { 'class': 'th' }, _('DL modulation')),
					E('th', { 'class': 'th' }, 'RSRP')
				])
			];

			var cc = [ Object.assign({ is_pcc: true }, ca.pcc || {}) ]
				.concat(ca.scc || []);

			cc.forEach(function(c, i) {
				rows.push(E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, c.is_pcc ? 'PCC' : ('SCC' + i)),
					E('td', { 'class': 'td' }, api.fmtBand(c.band)),
					E('td', { 'class': 'td' }, api.fmtNum(c.pci)),
					E('td', { 'class': 'td' }, api.fmtNum(c.freq)),
					E('td', { 'class': 'td' }, api.caStateName(c.state)),
					E('td', { 'class': 'td' }, c.dl_bw_mhz ? c.dl_bw_mhz + ' MHz' : '-'),
					E('td', { 'class': 'td' }, c.dl_mimo ? c.dl_mimo + 'x' + c.dl_mimo : '-'),
					E('td', { 'class': 'td' }, api.caModName(c.dl_mod)),
					E('td', { 'class': 'td' }, api.fmtDbm(c.rsrp_dbm))
				]));
			});

			body.appendChild(E('p', {},
				api.ratName(ca.rat) + (ca.has_nr && ca.rat !== 9 ? ' + NR (EN-DC)' : '')));
			body.appendChild(E('table', { 'class': 'table' }, rows));

			if (ca.has_nr)
				body.appendChild(E('p', { 'class': 'hint' },
					_('An NR leg is present, so this is EN-DC. Only the first block is expanded into the table above.')));
		}

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Carrier aggregation')),
			body
		]);
	},

	/* --- writes ----------------------------------------------------- */

	writeBands: function(csv, what) {
		var self = this;

		return api.setBands(csv).then(function(res) {
			reportWrite(res, what);
			return api.status().then(function(s) { self.paint(s); });
		}).catch(function(e) {
			ui.addNotification(null, E('p', {}, what + ' ' + _('failed') + ': ' + e), 'danger');
		});
	},

	writeCellLock: function(params, what) {
		var self = this;

		return api.setCellLock(params.mode, params.rat, params.type,
				       params.earfcn, params.pci, params.scs, params.nrband)
			.then(function(res) {
				reportWrite(res, what);
				return api.status().then(function(s) { self.paint(s); });
			}).catch(function(e) {
				ui.addNotification(null, E('p', {}, what + ' ' + _('failed') + ': ' + e), 'danger');
			});
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
