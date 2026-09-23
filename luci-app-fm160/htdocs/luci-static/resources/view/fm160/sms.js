'use strict';
'require view';
'require ui';
'require fm160.api as api';

/*
 * SMS page (M3).
 *
 * WHY THIS PAGE DOES NOT POLL
 * ---------------------------
 * Every other page in this app refreshes on a timer through poll.add().  This
 * one deliberately does not, and the reason is measured rather than stylistic:
 *
 *   AT+CSQ   24 ms          <- the signal page's poll, every 2 s, is free
 *   AT+CMGF? 5 373 ms       <- +CME ERROR: 10, no card
 *   AT+CPMS? 10 345 ms      <- ERROR, no card
 *
 * The AT port is a single serial line.  A polling tier that asked the SMS
 * questions would put a ten-second command on that line every cycle and starve
 * everything behind it - the signal poll, the registration poll, the identity
 * chain - so the modem would look broken exactly while the page was open.
 *
 * So the split is:
 *
 *   a refresh       a button.  Started by a person, once, when they want it.
 *   the list        read from the daemon's own memory (fm160.sms_list), which
 *                   touches no AT port at all, so the page can show messages
 *                   the moment it opens.
 *   new messages    pushed by the modem itself as +CMTI, which the daemon
 *                   turns into a fetch with no help from this page.
 *
 * The one timer here is the short one started after a send or a sync: it polls
 * fm160.status - the cached snapshot, still no AT traffic - until the daemon
 * reports the operation finished, then refreshes the list.  That is what makes
 * the page feel live without asking the modem anything.
 *
 * WHAT THE PAGE MUST NOT PRETEND
 * ------------------------------
 *   1. "Unusable" is a state to explain, not to hide.  On this hardware the
 *      common cause is that there is no SIM: the modem answers AT+CPMS? with
 *      ERROR after ten seconds and there is nowhere to store a message.  The
 *      banner says so, in the modem's own words, and the send button is
 *      disabled rather than queuing a command that has already been refused.
 *
 *   2. Timestamps are shown as the SMSC wrote them.  Not converted to the
 *      router's clock, not padded from a two-digit year into a four-digit one.
 *      See smsTimestamp() in api.js.
 *
 *   3. A missing index and index 0 are different things, and so are "no time
 *      stamp" and "00:00:00".  Both are carried as separate flags from the
 *      daemon and both are rendered as "-" here when absent.
 */

/* Same shape as the other M4/M5 pages: a confirmation before a change that
 * cannot be taken back. */
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
				}, okLabel || _('Delete'))
			])
		]);
	});
}

function num(v, unit) {
	if (!api.reported(v))
		return '-';
	return (unit ? v + ' ' + unit : String(v));
}

function row(label, value) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td left' }, label),
		E('td', { 'class': 'td' }, value)
	]);
}

function cell(v, opts) {
	return E('td', (opts && opts['class']) ? { 'class': opts['class'] } : {}, v);
}

/* The body of a message, in a block that keeps its line breaks.  An SMS body is
 * text a person typed on a phone keypad; reflowing it into one paragraph would
 * be an edit. */
function textBlock(s, opts) {
	return E('div', {
		'style': 'white-space:pre-wrap;word-break:break-word;' +
			 ((opts && opts['max-height']) ? 'max-height:' + opts['max-height'] + ';overflow:auto;' : '')
	}, s);
}

return view.extend({
	load: function() {
		return api.status();
	},

	render: function(state) {
		var self = this;

		this.state = state;
		this.messages = [];
		this.loading = false;

		/*
		 * The frame is built ONCE and the parts that change are filled in
		 * by paint().  Nothing here is rebuilt, because there is a text
		 * field and a phone-number field in it: a page that redrew itself
		 * every time the daemon reported progress would take the text out
		 * from under whoever was typing it.
		 */
		this.bannerBox  = E('div', {});
		this.storageBox = E('div', { 'class': 'cbi-section' });
		this.composeBox = E('div', { 'class': 'cbi-section' });
		this.listBox    = E('div', { 'class': 'cbi-section' });
		this.diagBox    = E('div', { 'class': 'cbi-section' });

		this.container = E('div', {}, [
			this.bannerBox,
			this.storageBox,
			this.composeBox,
			this.listBox,
			this.diagBox
		]);

		this.buildCompose();
		this.paint(state);
		this.loadList();

		return this.container;
	},

	/*
	 * Only the dynamic parts.  Called on entry, after every write, and by the
	 * idle watcher; never touches the compose fields.
	 */
	paint: function(state) {
		this.state = state;
		this.bannerBox.innerHTML = '';
		this.bannerBox.appendChild(this.renderBanner(state));
		this.storageBox.innerHTML = '';
		this.storageBox.appendChild(this.renderStorage(state));
		this.diagBox.innerHTML = '';
		this.diagBox.appendChild(this.renderDiagnostics(state));
		this.updateSendButton();
	},

	/* --- banner ------------------------------------------------------ */

	renderBanner: function(state) {
		var s = api.smsOf(state);
		var usable = api.smsUsable(state);
		var cls = usable ? 'alert-message success' : 'alert-message warning';
		var body = [];

		/*
		 * The order of these branches is the whole point of this function.
		 *
		 * `probed` is set only when AT+CPMS? is parsed successfully, so on a
		 * modem with no card it stays false forever - while last_error_text
		 * holds the refusal.  Testing `!probed` first, as an earlier version
		 * did, told the reader "the daemon has not finished asking yet" at the
		 * exact moment the daemon had asked and been turned down.  That is the
		 * one situation where the modem's own words matter most, so the refusal
		 * is checked before the silence.
		 */
		if (usable) {
			body.push(E('p', {}, _('Message mode is PDU, storage %s is in use, and new-message indications are armed.')
				.format(api.smsStorageOf(state).name || '?')));
		}
		else if (s.last_error_text) {
			body.push(E('p', {}, _('The modem has not accepted the SMS setup, so sending and refreshing are disabled. Nothing below has been lost: the list still shows messages the daemon kept from earlier.')));
			if (s.cmgf_known)
				body.push(E('p', {}, _('Message mode') + ': ' + (s.cmgf ? 'text' : 'PDU')));
			if (s.storage)
				body.push(E('p', {}, _('Storage') + ': ' + s.storage));
			body.push(E('p', {}, _('The modem said') + ': ' + s.last_error_text.trim()));
			body.push(E('p', {}, _('A modem without a SIM answers AT+CPMS? with ERROR; that is the usual cause here, and it is not a fault in the daemon. The setup is retried a few times and then left alone, because a question whose answer is a ten-second ERROR must not be asked on a timer.')));
		}
		else if (!s.probed) {
			body.push(E('p', {}, _('The daemon has not finished asking the modem whether it can handle SMS.')));
		}
		else {
			body.push(E('p', {}, _('The modem accepted the capability questions but the storage could not be brought into use, and it gave no error text. See the diagnostics below.')));
		}

		return E('div', { 'class': cls }, [
			E('h3', {}, _('SMS')),
			E('div', {}, body)
		]);
	},

	/* --- storage and counters ---------------------------------------- */

	renderStorage: function(state) {
		var st = api.smsStorageOf(state);
		var s = api.smsOf(state);
		var rows = [];

		rows.push(row(_('Storage in use'), st.name || '-'));
		if (st.used !== null || st.total !== null)
			rows.push(row(_('Messages on the modem'),
				num(st.used) + ' / ' + num(st.total)));
		else
			rows.push(row(_('Messages on the modem'), '-'));
		rows.push(row(_('Kept in this daemon'), num(s.kept)));
		rows.push(row(_('Unread'), num(s.unread)));
		rows.push(row(_('Pulled from the modem'), num(s.received)));
		rows.push(row(_('Already held when seen again'), num(s.duplicates)));
		rows.push(row(_('Last successful operation'),
			api.reported(s.age_ms) ? api.smsAgeText({ age_ms: s.age_ms }) + ' ' + _('ago') : '-'));

		return E('div', {}, [
			E('h3', {}, _('Storage')),
			E('table', { 'class': 'table' }, E('tbody', {}, rows)),
			E('div', { 'class': 'cbi-page-actions' }, [
				E('button', {
					'class': 'btn cbi-button cbi-button-action',
					'click': ui.createHandlerFn(this, 'handleSync')
				}, _('Refresh from modem')),
				' ',
				E('button', {
					'class': 'btn cbi-button',
					'click': ui.createHandlerFn(this, 'handleReload')
				}, _('Reload list'))
			]),
			E('p', { 'class': 'cbi-section-descr' },
				_('Refreshing asks the modem to list everything it holds (AT+CMGL=4). It is a button and not a timer because that command takes seconds when the modem has no card. Loading the list reads the daemon\'s memory and costs nothing.'))
		]);
	},

	/* --- compose ------------------------------------------------------ */

	buildCompose: function() {
		var self = this;

		this.inNumber = E('input', {
			'type': 'text',
			'class': 'cbi-input-text',
			'style': 'width:24em',
			'placeholder': '+8613800138000',
			'input': function() { self.updateEstimate(); }
		});

		this.inText = E('textarea', {
			'class': 'cbi-input-text',
			'rows': 4,
			'style': 'width:100%',
			'maxlength': 1024,
			'placeholder': _('Message text'),
			'input': function() { self.updateEstimate(); }
		});

		this.estimateBox = E('p', { 'class': 'cbi-section-descr' });

		this.btnSend = E('button', {
			'class': 'btn cbi-button cbi-button-apply',
			'click': ui.createHandlerFn(this, 'handleSend')
		}, _('Send'));

		this.composeBox.appendChild(E('h3', {}, _('New message')));
		this.composeBox.appendChild(E('table', { 'class': 'table' }, E('tbody', {}, [
			row(_('Number', 'fm160 sms field'), this.inNumber),
			E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left' }, _('Text')),
				E('td', { 'class': 'td' }, this.inText)
			])
		])));
		this.composeBox.appendChild(this.estimateBox);
		this.composeBox.appendChild(E('div', { 'class': 'cbi-page-actions' }, [ this.btnSend ]));
		this.composeBox.appendChild(E('p', { 'class': 'cbi-section-descr' },
			_('A message longer than 160 GSM 7-bit characters is split into numbered segments and sent as several messages; the modem is never asked to do the splitting. The count below is a preview - the daemon decides the real answer when it encodes the text.')));

		this.updateEstimate();
	},

	updateEstimate: function() {
		if (!this.estimateBox)
			return;
		var text = this.inText ? this.inText.value : '';
		var est = api.smsEstimate(text, 4);
		var parts;

		if (!est.parts) {
			this.estimateBox.textContent = _('Empty.');
			return;
		}
		parts = est.gsm7
			? est.septets + ' ' + _('septets') + ', ' + est.parts + ' ' + _('segment(s)')
			: est.chars + ' ' + _('characters') + ', ' + est.parts + ' ' + _('segment(s)');
		this.estimateBox.textContent = est.encoding + ' - ' + parts +
			(est.parts > 4 ? ' - ' + _('more than the 4 segments this daemon will send; the message will be refused') : '');
	},

	updateSendButton: function() {
		if (!this.btnSend)
			return;
		var ok = api.smsUsable(this.state);
		this.btnSend.disabled = !ok;
		this.btnSend.title = ok ? '' :
			_('The modem has not accepted the SMS setup - see the message at the top of this page.');
	},

	/* --- the list ----------------------------------------------------- */

	loadList: function() {
		var self = this;

		this.loading = true;
		this.paintList(this.messages, true);

		return api.smsList().then(function(res) {
			self.messages = (res && res.messages) || [];
			self.loading = false;
			self.paintList(self.messages, false);
		}).catch(function(e) {
			self.loading = false;
			self.listBox.innerHTML = '';
			self.listBox.appendChild(E('h3', {}, _('Messages')));
			self.listBox.appendChild(E('p', { 'class': 'alert-message warning' },
				_('Could not read the message list') + ': ' + e));
		});
	},

	paintList: function(messages, loading) {
		var self = this;

		this.listBox.innerHTML = '';
		this.listBox.appendChild(E('h3', {}, _('Messages') +
			(loading ? '' : ' (' + messages.length + ')')));

		if (loading && !messages.length) {
			this.listBox.appendChild(E('p', { 'class': 'cbi-section-descr' }, _('Reading...')));
			return;
		}
		if (!messages.length) {
			this.listBox.appendChild(E('p', { 'class': 'cbi-section-descr' },
				_('Nothing stored. Messages the modem holds appear here after a refresh; new ones arrive on their own as the modem announces them.')));
			return;
		}

		var head = E('tr', { 'class': 'tr table-titles' }, [
			E('th', { 'class': 'th' }, _('Direction')),
			E('th', { 'class': 'th' }, _('Number', 'fm160 sms field')),
			E('th', { 'class': 'th' }, _('Time (as stamped)')),
			E('th', { 'class': 'th' }, _('Format')),
			E('th', { 'class': 'th' }, _('Text')),
			E('th', { 'class': 'th' }, '')
		]);
		var body = E('tbody', {}, [ head ]);

		messages.forEach(function(m) {
			body.appendChild(self.renderRow(m));
		});

		this.listBox.appendChild(E('table', { 'class': 'table' }, body));
	},

	renderRow: function(m) {
		var self = this;
		var ts = api.smsTimestamp(m);
		var parts = api.smsPartsText(m);
		var fmt = api.smsEncodingName(m.encoding);
		var meta = [];

		if (parts)
			meta.push(_('Part') + ' ' + parts);
		if (m.index_known)
			meta.push('#' + m.index);
		else
			meta.push(_('no modem copy'));

		return E('tr', { 'class': 'tr' }, [
			cell(m.outgoing ? _('Sent') : _('Received'), { 'class': 'td' }),
			cell(m.number || '-', { 'class': 'td' }),
			cell(ts ? [ ts.text, E('br'), E('small', {}, ts.zone) ] : '-', { 'class': 'td' }),
			cell([ fmt, E('br'), E('small', {}, meta.join(' / ')) ], { 'class': 'td' }),
			cell(textBlock(m.text, { 'max-height': '6em' }), { 'class': 'td' }),
			cell([
				E('button', {
					'class': 'btn cbi-button',
					'click': function() { ui.showModal(_('Message'), [
						E('div', {}, [
							E('p', {}, [ E('strong', {}, _('Number', 'fm160 sms field') + ': ' ), m.number || '-' ]),
							E('p', {}, [ E('strong', {}, _('Time') + ': ' ),
								ts ? ts.text + ' (' + ts.zone + ')' : '-' ]),
							E('p', {}, [ E('strong', {}, _('Format') + ': ' ), fmt +
								(parts ? ', ' + _('Part') + ' ' + parts : '') ]),
							E('p', {}, [ E('strong', {}, _('Storage') + ': ' ),
								api.smsStorageOf(self.state).name || '-',
								m.index_known ? ' #' + m.index : ', ' + _('no modem copy') ]),
							textBlock(m.text, { 'max-height': '16em' })
						]),
						E('div', { 'class': 'right' }, [
							E('button', {
								'class': 'btn',
								'click': function(ev) { ev.currentTarget.blur(); ui.hideModal(); }
							}, _('Close'))
						])
					]); }
				}, _('View')),
				' ',
				m.outgoing ? '' : E('button', {
					'class': 'btn cbi-button',
					'click': ui.createHandlerFn(this, 'handleMarkRead', m)
				}, _('Mark read')),
				m.outgoing ? '' : ' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-remove',
					'click': ui.createHandlerFn(this, 'handleDelete', m)
				}, _('Delete'))
			], { 'class': 'td' })
		]);
	},

	/* --- operations --------------------------------------------------- */

	/*
	 * The only timer on this page, and it does not touch the AT port: it reads
	 * the cached snapshot until the daemon says the send or the listing has
	 * finished, then pulls the list.  Bounded, so a modem that never answers
	 * leaves the page quiet instead of leaving a loop running.
	 */
	watchIdle: function(rounds) {
		var self = this;

		return api.status().then(function(s) {
			self.paint(s);
			if (!api.smsOf(s).busy || rounds <= 0)
				return self.loadList();
			return new Promise(function(r) { setTimeout(r, 1500); })
				.then(function() { return self.watchIdle(rounds - 1); });
		});
	},

	handleReload: function() {
		return this.loadList();
	},

	handleSync: function() {
		var self = this;

		return api.smsSync().then(function(res) {
			var result = (res && res.result) || 'error';

			if (result === 'started') {
				ui.addNotification(null, E('p', {},
					_('The modem is listing what it holds. The list refreshes itself when it answers.')), 'info');
				return self.watchIdle(12);
			}
			if (result === 'busy') {
				ui.addNotification(null, E('p', {},
					_('Another SMS operation is already running.')), 'warning');
				return;
			}
			if (result === 'not-ready') {
				ui.addNotification(null, E('p', {},
					_('The modem has not accepted the SMS setup, so it cannot be asked for messages.') +
					(res && res.detail ? ' ' + res.detail : '')), 'warning');
				return api.status().then(function(s) { self.paint(s); });
			}
			ui.addNotification(null, E('p', {}, _('Could not start the refresh.')), 'danger');
		}).catch(function(e) {
			ui.addNotification(null, E('p', {}, _('Refresh failed') + ': ' + e), 'danger');
		});
	},

	handleSend: function() {
		var self = this;
		var number = (this.inNumber.value || '').trim();
		var text = this.inText.value || '';
		var est = api.smsEstimate(text, 4);

		if (!number || !text) {
			ui.addNotification(null, E('p', {}, _('A number and a message are both required.')), 'warning');
			return;
		}
		if (!api.smsUsable(this.state)) {
			ui.addNotification(null, E('p', {}, _('The modem has not accepted the SMS setup.')), 'warning');
			return;
		}

		return confirmPrompt(_('Send this message?'), [
			_('To') + ': ' + number,
			_('This will be sent as') + ' ' + est.encoding + ', ' + est.parts + ' ' + _('segment(s)') + '.',
			_('A sent message cannot be recalled.'),
			_('Nothing is sent until you confirm.')
		], _('Send')).then(function(ok) {
			if (!ok)
				return;

			self.btnSend.disabled = true;
			return api.smsSend(number, text).then(function(res) {
				var status = (res && res.status) || 'unknown';

				if (status === 'ok') {
					ui.addNotification(null, E('p', {}, _('Accepted by the modem.')), 'info');
					self.inText.value = '';
					self.updateEstimate();
				}
				else {
					ui.addNotification(null, E('p', {}, _('Send failed') + ': ' + status + ' - ' +
						((res && res.response) || _('(no response)'))), 'danger');
				}
				return self.watchIdle(12);
			}).catch(function(e) {
				ui.addNotification(null, E('p', {}, _('Send failed') + ': ' + e), 'danger');
				return api.status().then(function(s) { self.paint(s); });
			});
		});
	},

	handleMarkRead: function(m) {
		var self = this;

		return api.smsMarkRead(m.id).then(function() {
			return self.loadList();
		}).catch(function(e) {
			ui.addNotification(null, E('p', {}, _('Could not mark it read') + ': ' + e), 'danger');
		});
	},

	/*
	 * Deleting touches two copies, and the reply says which of them went.  The
	 * page repeats that distinction instead of collapsing it into "deleted":
	 * the local entry going away while the modem's copy stays is a real outcome
	 * (the store will fill up) and the user should see it.
	 */
	handleDelete: function(m) {
		var self = this;
		var lines = [
			_('To') + ': ' + (m.number || '-'),
			m.index_known
				? _('This removes the modem\'s copy (AT+CMGD=%d) as well as the copy held here.').format(m.index)
				: _('The modem has no copy of this one; only the entry held here is removed.'),
			_('Messages the modem still holds will not come back on the next refresh, but a message kept only here is gone for good.')
		];

		return confirmPrompt(_('Delete this message?'), lines, _('Delete')).then(function(ok) {
			if (!ok)
				return;

			return api.smsDelete(m.id).then(function(res) {
				var local = res && res.local_deleted;
				var modem = res && res.modem_deleted;

				if (local && (modem || !(res && res.modem_tried))) {
					ui.addNotification(null, E('p', {},
						modem ? _('Removed from the modem and from this list.')
						      : _('Removed from this list; the modem had no copy.')), 'info');
				}
				else if (local) {
					ui.addNotification(null, E('p', {},
						_('Removed from this list, but the modem refused to delete its own copy') + ': ' +
						((res && res.response) || _('(no response)'))), 'warning');
				}
				else {
					ui.addNotification(null, E('p', {}, _('Delete failed') + ': ' +
						((res && res.status) || 'unknown')), 'danger');
				}
				return self.loadList();
			}).catch(function(e) {
				ui.addNotification(null, E('p', {}, _('Delete failed') + ': ' + e), 'danger');
			});
		});
	},

	/* --- diagnostics --------------------------------------------------- */

	renderDiagnostics: function(state) {
		var s = api.smsOf(state);
		var rows = [];

		rows.push(row(_('Setup completed'), s.setup_done ? _('yes') : _('no')));
		rows.push(row(_('Storage state read'), s.probed ? _('yes') : _('no')));
		rows.push(row(_('Usable'), s.usable ? _('yes') : _('no')));
		rows.push(row(_('Message mode'),
			s.cmgf_known ? (s.cmgf ? _('text') : 'PDU') : _('-') + ' (' + _('not read') + ')'));
		rows.push(row(_('Last +CMS ERROR'),
			s.last_error ? '+' + s.last_error : _('none')));
		if (s.last_error_text)
			rows.push(row(_('Last error text'), s.last_error_text));
		rows.push(row(_('Operation in flight'), s.busy ? _('yes') : _('no')));

		rows.push(row(_('Sends accepted'),
			num(api.smsCounterOf(state, 'sent_ok'))));
		rows.push(row(_('Sends failed'),
			num(api.smsCounterOf(state, 'sent_fail'))));
		rows.push(row(_('Sends that timed out'),
			num(api.smsCounterOf(state, 'sent_timeout'))));

		/*
		 * PLAN §4 asks whether a long PDU is losing a zero-length packet.  These
		 * two counts are that question, kept separate from the totals so a
		 * multi-segment message that died is not averaged away.
		 */
		rows.push(row(_('Multi-segment sends accepted'),
			num(api.smsCounterOf(state, 'long_sent'))));
		rows.push(row(_('Multi-segment sends that timed out'),
			num(api.smsCounterOf(state, 'long_timeout'))));

		var last = s.last_send || {};
		if (last.command) {
			rows.push(row(_('Last send command'), last.command));
			rows.push(row(_('Segments in it'), num(last.segments)));
			rows.push(row(_('PDU length (hex digits)'), num(last.pdu_chars)));
			rows.push(row(_('+CMGS reference'), num(last.mr)));
		}

		return E('div', {}, [
			E('h3', {}, _('Diagnostics', 'fm160 diagnostics heading')),
			E('table', { 'class': 'table' }, E('tbody', {}, rows)),
			E('p', { 'class': 'cbi-section-descr' },
				_('The two multi-segment counters are the evidence for one specific question: whether a PDU that spans more than one USB packet can time out on this kernel. They are counted rather than assumed, so a workaround is only worth writing if the second number is non-zero.'))
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
