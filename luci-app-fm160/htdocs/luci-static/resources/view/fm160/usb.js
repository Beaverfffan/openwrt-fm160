'use strict';
'require view';
'require poll';
'require ui';
'require fm160.api as api';

/*
 * USB mode page: the profile the module enumerates in, and the switch.
 *
 * This used to be the second half of the Dial page, and it is a page of its own
 * because the two halves only look alike.  Dialling is a sequence of AT commands
 * that can fail and be retried; switching the profile restarts the module, takes
 * the management channel away for 30-90 s, and is the only operation in this
 * application that can leave the modem beyond reach from the host.  Keeping them
 * on one page made the dangerous button sit next to the safe one and made the
 * page's own alarm about a stuck module read as if it were about the dial.
 *
 * Three rules shape everything below.
 *
 *   1. THE PAGE IS NOT THE GUARD.  Every button here is a request.  The daemon
 *      decides: fm160_modesw_apply() runs fm160_usbmode_decide() again on the way
 *      in.  The filtering below is a convenience so the user is not offered a
 *      button that can only fail - it is never the reason something is safe.
 *
 *   2. THE MODULE CAN BE GONE.  During a switch the AT port does not exist for a
 *      minute or two.  That is expected, and the page must not report it as a
 *      fault; what it must shout about is the one state where the module did not
 *      come back at all, because then fm160d refuses further switches and the way
 *      out is a hand on the hardware.
 *
 *   3. A REFUSED PROFILE IS REFUSED FOR A REASON.  The FM160 has no
 *      reset-to-default that the host can trigger, so a profile that would leave
 *      no AT interface at all, or whose AT interface the kernel has no driver
 *      for, would be a one-way trip.  Those are listed with their verdict rather
 *      than hidden, because "why can I not pick this one" is the question this
 *      page exists to answer.
 */

/* --- small helpers, in the house style of the cells page --------------- */

function confirmPrompt(title, lines, okLabel, className) {
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
					'class': 'btn ' + (className || 'cbi-button-negative'),
					'click': function(ev) {
						ev.currentTarget.blur();
						ui.hideModal();
						resolve(true);
					}
				}, okLabel || _('Continue'))
			])
		]);
	});
}

function info(text) {
	ui.addNotification(null, E('p', {}, text), 'info');
}

function warn(text) {
	ui.addNotification(null, E('p', {}, text), 'warning');
}

function fail(text) {
	ui.addNotification(null, E('p', {}, text), 'danger');
}

function row(label, value, cls) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td', 'style': 'width:34%' }, label),
		E('td', { 'class': 'td ' + (cls || '') }, value)
	]);
}

function section(title, children) {
	return E('div', { 'class': 'cbi-section' }, [ E('h3', {}, title) ].concat(children));
}

/*
 * The foreground hold makes the daemon shorten its polling intervals and switch
 * on the tiers that only run for an open page.
 *
 * This page wants that while a profile change is in flight - it is the only time
 * the step can change under the user's eyes - and NOT otherwise.  The tiers it
 * turns on include the cell poll (AT+GTCCINFO?), which this page has no use for
 * and which costs the one serial port real time.  Holding the foreground
 * permanently would be the easy thing to do and the wrong one.
 */
function holdForeground(st) {
	return api.modeswPending(st) || api.modeswBusy(st);
}

return view.extend({
	load: function() {
		var self = this;

		this.advanced = false;
		this.profileError = null;

		return api.status().then(function(st) {
			self.state = st;
			self.profileKey = self.keyOf(st);

			/* A failure here must not blank the page: the profile list is one
			 * section and the module's state is the banner above it. */
			return api.profiles().catch(function(e) {
				self.profileError = String((e && e.message) || e);
				return null;
			});
		}).then(function(pf) {
			self.profileData = pf;
			return self.state;
		});
	},

	/*
	 * The profile list is fetched on demand, so it has to be refetched whenever
	 * the daemon's basis for its verdicts moves: the active profile, whether the
	 * capability answer ever arrived, and which profiles the modem reported.
	 * `pending` is in the key too, so the rows are re-asked for when a switch
	 * starts and again when it ends - the second of those is exactly when
	 * `current` changes, and re-asking is the cheapest way to keep the display
	 * and the decision in step.
	 */
	keyOf: function(st) {
		var m = api.modeswOf(st);

		return [ m.current_known ? m.current : '-', m.caps_valid ? 1 : 0,
			 m.pending ? 1 : 0, (m.supported || []).join(',') ].join('|');
	},

	render: function(state) {
		var self = this;

		this.container = E('div', {});
		this.paint(state);

		poll.add(function() {
			if (!document.body.contains(self.container)) {
				poll.stop();
				return Promise.resolve();
			}

			var hold = holdForeground(self.state);

			return (hold ? api.profile(true) : Promise.resolve())
				.then(api.status)
				.then(function(s) {
					var fetch = null, key = self.keyOf(s);

					self.state = s;
					if (key !== self.profileKey) {
						self.profileKey = key;
						fetch = self.loadProfiles();
					}
					self.paint(s);
					return fetch;
				});
		}, 2);

		return this.container;
	},

	loadProfiles: function() {
		var self = this;

		return api.profiles().then(function(pf) {
			self.profileData = pf;
			self.profileError = null;
			self.paint(self.state);
		}).catch(function(e) {
			self.profileError = String((e && e.message) || e);
			self.paint(self.state);
		});
	},

	paint: function(state) {
		this.state = state;
		this.container.innerHTML = '';

		this.container.appendChild(this.renderBanner(state));
		this.container.appendChild(this.renderProfiles(state));
		this.container.appendChild(this.renderNotes(state));
	},

	/* --- what is wrong, if anything --------------------------------- */

	renderBanner: function(state) {
		var box = E('div', {}), m = api.modeswOf(state);

		/* Ordered by severity.  A stuck module is the one state this page cannot
		 * get out of, so it is the one that gets the loudest box. */
		if (api.modeswStuck(state)) {
			box.appendChild(E('div', { 'class': 'alert-message error' }, [
				E('strong', {}, _('A profile change left the module unreachable.')),
				' ',
				_('It has not answered since the switch, so fm160d is not trying again and will refuse further profile changes. The module needs a manual power cycle; nothing on this page can recover it.')
			]));
		}
		else if (m.pending) {
			box.appendChild(E('div', { 'class': 'alert-message warning' }, [
				_('The module is restarting in profile') + ' ',
				E('strong', {}, String(api.modeswTarget(state))),
				'. ',
				_('It re-enumerates on the USB bus, which takes 30-90 s, and the AT port is absent until it finishes.'),
				api.reported(m.pending_for_ms) ?
					' (' + api.fmtAge(m.pending_for_ms) + ')' : ''
			]));
		}

		if (!state.port_found && !m.pending)
			box.appendChild(E('div', { 'class': 'alert-message warning' },
				_('No AT port. The modem is either absent, or has not finished re-enumerating after a profile change.')));

		if (api.dialKindKnown(state) && !api.dialIsDaemons(api.dialKind(state)))
			box.appendChild(E('div', { 'class': 'alert-message notice' }, [
				_('This USB profile is not an ECM profile, so fm160d does not dial it.'),
				' ',
				_('QMI and MBIM links are brought up by the kernel stack through netifd - use the network configuration for that, and leave the button below alone.')
			]));

		return box;
	},

	/* --- the USB profile switch ------------------------------------- */

	renderProfiles: function(state) {
		var self = this;
		var m = api.modeswOf(state);
		var cur = api.modeswCurrent(state);
		var table = E('div', {});

		var box = section(_('USB profile'), [
			E('table', { 'class': 'table' }, [
				row(_('Active profile'), cur === null ?
					E('span', {}, _('not read yet - 17 and 32 share a USB ID, and so do 18 and 33, so the number has to be read back with AT+GTUSBMODE?')) :
					(cur + ' - ' + (this.labelOf(cur) || ''))),
				row(_('State'), api.modeswStateText(state)),
				row(_('Rollback point'), api.modeswRollback(state) === null ? '-' :
					String(api.modeswRollback(state))),
				row(_('Profile list'), m.caps_valid ?
					_('answered by the modem') + ': ' + (m.supported || []).join(', ') :
					E('span', {}, _('the modem has not answered AT+GTUSBMODE=? - no profile change is allowed until it does, which happens at boot and on a rescan')))
			]),
			this.optinRow(state),
			table,
			E('p', { 'class': 'hint' }, [
				E('strong', {}, _('Switching a profile restarts the module.')),
				' ',
				_('The modem disappears from the USB bus and comes back as a different device, so the network drops for roughly 30-90 s. fm160d reads the profile back afterwards: if it did not change it rolls back once, and if the module never answers it stops and says so above.'),
				' ',
				_('Profiles that would leave no AT interface at all, or whose AT interface the kernel has no driver for, are listed as refused and cannot be chosen - the FM160 has no reset-to-default that the host can trigger, so that would be a one-way trip.')
			])
		]);

		this.tableHost = table;
		this.paintProfiles(state);

		return box;
	},

	/* A mode number as the daemon's own table names it.  The list comes from
	 * fm160.profiles, not from api.js's copy of the table, so the text shown is
	 * the text the daemon's decision was made about. */
	labelOf: function(mode) {
		var rows = api.profileRows(this.profileData);

		for (var i = 0; i < rows.length; i++)
			if (rows[i].mode === mode)
				return rows[i].layout;
		return null;
	},

	optinRow: function(state) {
		var self = this;

		if (!this.optin) {
			this.optin = E('input', {
				'type': 'checkbox',
				'change': function(ev) {
					self.advanced = !!ev.currentTarget.checked;
					self.paintProfiles(self.state);
				}
			});
		}
		/* A DOM property, not the attribute map E() takes: '' and null are both
		 * falsy here, so `? '' : null` would clear the box on every repaint. */
		this.optin.checked = !!this.advanced;

		return E('div', { 'style': 'margin:6px 0' }, [
			this.optin, ' ',
			_('Also show profiles that are not in the FM160 AT manual'),
			E('div', { 'class': 'hint' }, _('These are device dependent: the vendor lists them in its port table and says nothing else about them. They are refused unless this is ticked.'))
		]);
	},

	paintProfiles: function(state) {
		var self = this, host = this.tableHost;

		if (!host)
			return;
		host.innerHTML = '';

		if (this.profileError) {
			host.appendChild(E('p', { 'class': 'alert-message warning' },
				_('The profile list could not be read') + ': ' + this.profileError));
			return;
		}
		if (!this.profileData) {
			host.appendChild(E('p', { 'class': 'hint' }, _('Reading the profile list...')));
			return;
		}

		var rows = api.profileRows(this.profileData);
		var choices = api.profileChoices(rows, this.advanced);
		var refused = api.profileRefused(rows, this.advanced);

		if (!rows.length) {
			host.appendChild(E('p', { 'class': 'hint' },
				_('This unit reports no profile this firmware knows how to name. Nothing is offered, because on any other platform the same mode numbers mean something else.')));
			return;
		}

		host.appendChild(E('h4', {}, _('Available', 'fm160 profile table heading')));

		if (!choices.length) {
			host.appendChild(E('p', { 'class': 'hint' },
				api.modeswOf(state).caps_valid ?
					_('No profile may be chosen right now.') :
					_('The modem has not answered AT+GTUSBMODE=?, so nothing may be chosen.')));
		}
		else {
			host.appendChild(E('table', { 'class': 'table' },
				[ E('tr', { 'class': 'tr' }, [
					E('th', { 'class': 'th' }, _('Mode')),
					E('th', { 'class': 'th' }, _('USB ID')),
					E('th', { 'class': 'th' }, _('Interfaces')),
					E('th', { 'class': 'th' }, _('Dialled by')),
					E('th', { 'class': 'th' }, '')
				]) ].concat(choices.map(function(p) {
					return E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td' }, [
							String(p.mode),
							p.needsOptin ? E('span', { 'class': 'hint' }, ' (' + _('advanced') + ')') : ''
						]),
						E('td', { 'class': 'td' }, p.pid ? ('0x' + p.pid) : '-'),
						E('td', { 'class': 'td' }, p.layout),
						E('td', { 'class': 'td' }, api.dialIsDaemons(p.kind) ?
							_('fm160d') : _('the kernel stack')),
						E('td', { 'class': 'td' }, E('button', {
							'class': 'btn cbi-button cbi-button-action',
							'click': ui.createHandlerFn(self, function() {
								return self.requestSwitch(p);
							})
						}, _('Switch', 'fm160 profile action')))
					]);
				}))));
		}

		if (refused.length) {
			host.appendChild(E('h4', {}, _('Not offered')));
			host.appendChild(E('table', { 'class': 'table' },
				refused.map(function(p) {
					return E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td', 'style': 'width:6%' }, String(p.mode)),
						E('td', { 'class': 'td', 'style': 'width:34%' }, p.layout),
						E('td', { 'class': 'td' }, _(p.verdictText))
					]);
				})));
		}
	},

	requestSwitch: function(p) {
		var self = this;
		var cur = api.modeswCurrent(this.state);
		var lines = [
			_('The module will restart in profile') + ' ' + p.mode + ' (' + p.layout + ').',
			_('Every connection through it drops. The modem leaves the USB bus and comes back as a different device, so it is unreachable for roughly 30-90 s - including from this page.'),
			cur === null ? _('The profile it is in now is not known, so there is nothing to go back to.') :
				_('If it does not come back in the new profile, fm160d rolls back to') + ' ' + cur + ' ' + _('once.'),
			_('If that fails too, the module needs a manual power cycle. Nothing on this page can recover it.')
		];

		if (p.needsOptin)
			lines.push(_('This profile is not in the FM160 AT manual. It appears here only because you asked for the advanced list; nothing documents how it behaves.'));

		return confirmPrompt(_('Switch USB profile?'), lines,
				     _('Restart the module in profile') + ' ' + p.mode).then(function(yes) {
			if (!yes)
				return;

			return api.setUsbMode(p.mode, p.needsOptin).then(function(res) {
				self.reportSwitch(res, p);
				/* The snapshot's modesw table carries the whole story from
				 * here; the profile list is re-asked when `current` moves. */
				self.profileKey = null;
				return api.status().then(function(s) {
					self.state = s;
					self.profileKey = self.keyOf(s);
					return self.loadProfiles();
				});
			}).catch(function(e) {
				fail(_('The switch request failed') + ': ' + String((e && e.message) || e));
			});
		});
	},

	reportSwitch: function(res, p) {
		var result = (res && res.result) || 'unknown';
		var detail = (res && res.detail) || '';

		if (result === 'applying')
			info(_('The switch has been sent. The module is restarting; expect it to be gone for 30-90 s. This page will follow it.'));
		else if (result === 'already')
			info(_('The module is already in profile') + ' ' + p.mode + '.');
		else if (result === 'refused')
			fail(_('The daemon refused the switch') + (detail ? ': ' + detail : '.'));
		else if (result === 'busy')
			warn(_('A profile change is already in progress.'));
		else if (result === 'not-ready')
			warn(_('Refused for now: no AT port, or the modem has not been asked which profile it is in.'));
		else
			fail(_('The switch request failed') + (detail ? ': ' + detail : ''));
	},

	/* --- what this page deliberately does not do -------------------- */

	renderNotes: function(state) {
		return section(_('Notes', 'fm160 usb notes heading'), [
			E('ul', { 'class': 'hint' }, [
				E('li', {}, [
					_('This page is a convenience, not the guard. fm160d re-checks every request: the profile decision runs inside the same function that would send the command, so nothing here can widen what is allowed.')
				]),
				E('li', {}, [
					_('The ECM profile is dialled by fm160d itself, and a'), ' ',
					E('code', {}, 'proto fm160'), ' ',
					_('interface exists so that netifd can own the address side of it. QMI and MBIM profiles are dialled by the kernel (uqmi / umbim) instead.')
				]),
				E('li', {}, [
					_('The profile the module is in is read back with AT+GTUSBMODE? rather than remembered, because 17 and 32 share a USB ID and so do 18 and 33 - the USB descriptors alone cannot tell which one is active.')
				])
			])
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
