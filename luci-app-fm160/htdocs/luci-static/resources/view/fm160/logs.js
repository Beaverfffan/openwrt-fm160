'use strict';
'require view';
'require ui';
'require fm160.api as api';

/*
 * Operations log page.
 *
 * Every state-changing action taken through the UI, the dial control
 * scripts or the keepalive daemon appends one JSON line to
 * /var/log/fm160-ops.log (ring, 300 lines) via fm160-oplog.  This page
 * tails the last N entries: time, who did it, what happened and how it
 * turned out.  It is the single audit trail for "what has this plugin
 * been doing to the modem" - the counterpart to the daemon log on the
 * AT console page, which records what the modem has been saying back.
 */

var POLL_MS = 3000;

var RESULT_CLASS = {
	'ok':      'success',
	'success': 'success',
	'fail':    'warning',
	'error':   'danger',
	'info':    'info'
};

function fmtTime(ts) {
	if (!ts)
		return '-';

	var d = new Date(ts * 1000);

	if (isNaN(d.getTime()))
		return String(ts);

	function p(n) { return (n < 10 ? '0' : '') + n; }

	return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) +
		' ' + p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
}

function resultClass(r) {
	r = String(r || '').toLowerCase();

	for (var k in RESULT_CLASS)
		if (r.indexOf(k) === 0)
			return RESULT_CLASS[k];

	return 'info';
}

return view.extend({
	render: function() {
		var self = this;

		this.tbody = E('tbody');
		this.statusline = E('p', { 'class': 'hint' }, _('Loading…'));
		this.autoscroll = E('input', {
			'type': 'checkbox',
			'checked': 'checked'
		});

		this.table = E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, _('Time')),
				E('th', { 'class': 'th' }, _('Actor')),
				E('th', { 'class': 'th' }, _('Action')),
				E('th', { 'class': 'th' }, _('Detail')),
				E('th', { 'class': 'th' }, _('Result'))
			]),
			this.tbody
		]);

		return E('div', {}, [
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Operations log')),
				E('p', { 'class': 'hint' },
				  _('Every action this plugin performs on the modem - dials, redials, module reloads, band and cell-lock changes, USB mode switches, SMS sends, keepalive verdicts - is appended here with its outcome. The log lives in /var/log/fm160-ops.log and keeps the most recent 300 entries.')),
				E('div', {}, [
					E('button', {
						'class': 'btn cbi-button',
						'click': ui.createHandlerFn(this, this.refresh)
					}, _('Refresh')),
					' ',
					E('button', {
						'class': 'btn cbi-button-negative',
						'click': ui.createHandlerFn(this, this.clear)
					}, _('Clear log')),
					' ',
					E('label', { 'class': 'cbi-checkbox' }, [
						this.autoscroll, ' ', _('Autoscroll')
					])
				]),
				this.statusline,
				E('div', { 'style': 'max-height:32em;overflow:auto' }, this.table)
			])
		]);
	},

	load: function() {
		return api.opLogTail(100).catch(function(e) { return { error: String(e) }; });
	},

	paint: function(res) {
		var entries = Array.isArray(res) ? res : (res && res.entries) ? res.entries : [];

		this.statusline.textContent = res && res.error ?
			_('Could not read the log:') + ' ' + res.error :
			(entries.length + ' ' + _('entries'));

		this.tbody.innerHTML = '';

		entries.forEach(function(e) {
			this.tbody.appendChild(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td', 'style': 'white-space:nowrap;font-family:monospace;font-size:12px' }, fmtTime(e.ts)),
				E('td', { 'class': 'td' }, e.actor || '-'),
				E('td', { 'class': 'td' }, e.action || '-'),
				E('td', { 'class': 'td', 'style': 'word-break:break-all' }, e.detail || ''),
				E('td', { 'class': 'td' }, E('span', {
					'class': 'label ' + resultClass(e.result)
				}, e.result || '-'))
			]));
		}.bind(this));

		if (this.autoscroll.checked) {
			var box = this.table.parentNode;
			if (box)
				box.scrollTop = box.scrollHeight;
		}
	},

	refresh: function() {
		return api.opLogTail(100).then(this.paint.bind(this)).catch(function(e) {
			this.statusline.textContent = _('Could not read the log') + ': ' + e;
		}.bind(this));
	},

	clear: function() {
		return api.opLogClear().then(function() {
			return this.refresh();
		}.bind(this)).catch(function(e) {
			ui.addNotification(null, E('p', {}, _('Could not clear the log') + ': ' + e), 'danger');
		});
	},

	poll: function() {
		return this.refresh();
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
