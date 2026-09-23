'use strict';
'require view';
'require ui';
'require fm160.api as api';

/*
 * AT debug page.
 *
 * This is the only place in the UI that can put an arbitrary command on the
 * wire.  Requests go in at interactive priority (they overtake the polling
 * queue), the daemon opens a 10 s quiet window around them so nothing else
 * interleaves, and the raw response is shown verbatim - including ERROR, which
 * is often the most informative answer.
 *
 * It also carries the diagnostics bundle, which is the one thing on this page
 * that does NOT put anything on the wire.  The two belong together because they
 * answer the same question in different ways - "what is actually going on" -
 * and because this is the page someone is already on when they have decided to
 * report a problem: the last section of this file is the "send me the details"
 * button, next to the console that produced them.
 */

/*
 * Commands that need longer than the default timeout, measured on real hardware
 * (FM160-CN 89614.1000.00.04.01.02 with no SIM inserted -- the worst case,
 * because several of these block until the modem's own internal timeout):
 *
 *   AT+GTPKGVER?  12.9 s      AT+CCID      10.3 s      AT+CPMS?  10.3 s
 *   AT+CMGF?       5.4 s      AT+CIMI       5.2 s      AT+CGDCONT? 5.2 s
 *
 * At a flat 5 s these come back as "timeout", which reads as a broken modem
 * rather than as a slow query.
 *
 * AT+ICCID answers the same question as AT+CCID in 18 ms, so it is offered
 * instead; AT+CCID stays in the slow table because people type it from habit.
 */
var SLOW_MS = {
	'AT+GTPKGVER?': 20000,
	'AT+CCID':      20000,
	'AT+CPMS?':     20000,
	'AT+CMGF?':     12000,
	'AT+CIMI':      12000,
	'AT+CGDCONT?':  12000,
	/* The one GNSS command whose ANSWER TIME has never been measured: every
	 * other GNSS command came back in well under 500 ms, but this one was
	 * never answered on this hardware at all.  A generous timeout costs
	 * nothing here and turns a possible slow answer into an answer. */
	'AT+GTGPSCFG=?': 12000
};
var DEFAULT_TIMEOUT_MS = 5000;

function timeoutFor(cmd) {
	return SLOW_MS[cmd.toUpperCase()] || DEFAULT_TIMEOUT_MS;
}

var QUICK = [
	'AT',
	'ATI',
	'AT+CGMI',
	'AT+CGMM',
	'AT+CGMR',
	'AT+CGSN',
	'AT+CFSN',
	'AT+ICCID',
	'AT+CPIN?',
	'AT+CFUN?',
	'AT+CSQ',
	'AT+CESQ',
	'AT+CEREG?',
	'AT+C5GREG?',
	'AT+COPS?',
	'AT+GTUSBMODE?',
	'AT+GTUSBMODE=?',
	'AT+GTCCINFO?',
	'AT+GTCAINFO?',
	'AT+GTACT?',
	'AT+GTACT=?',
	'AT+GTCELLLOCK?',
	'AT+CGDCONT?',
	'AT+GTWWAN?',
	'AT+GTRNDIS?',
	'AT+CPMS?',
	'AT+CMGF?',
	'AT+CNMI?',
	'AT+GTGPSPOWER?',
	'AT+GTGPS?',
	/* GNSS, the parts that are safe to poke by hand.  AT+GTGPS=<item> needs
	 * the item in double quotes - unquoted it answers ERROR, which looks like
	 * a broken receiver and is not.  AT+GTGPSCFG=? has never been answered on
	 * this hardware, so it is here to be looked at rather than trusted. */
	'AT+GTGPS="RMC"',
	'AT+GTGPSCFG?',
	'AT+GTGPSCFG=?',
	'AT+GTGPSEPO?',
	'AT+GTAGPSSERV?'
];

return view.extend({
	render: function() {
		var self = this;

		this.input = E('input', {
			'type': 'text',
			'class': 'cbi-input-text',
			'style': 'width:100%;font-family:monospace',
			'placeholder': 'AT+CSQ',
			'keydown': function(ev) {
				if (ev.key === 'Enter') {
					ev.preventDefault();
					self.send();
				}
			}
		});

		this.output = E('pre', {
			'style': 'max-height:26em;overflow:auto;white-space:pre-wrap;word-break:break-all;' +
				 'font-family:monospace;font-size:12px;padding:8px;border:1px solid rgba(0,0,0,0.15);border-radius:8px'
		}, '');

		var quick = E('div', {}, QUICK.map(function(cmd) {
			return E('button', {
				'class': 'btn cbi-button',
				'style': 'margin:0 4px 4px 0;font-family:monospace',
				'click': function() {
					self.input.value = cmd;
					self.send();
				}
			}, cmd);
		}));

		this.statusline = E('p', { 'class': 'hint' }, '');

		this.diagStatus = E('p', { 'class': 'hint' }, '');
		this.diagOut = E('pre', {
			'style': 'max-height:26em;overflow:auto;white-space:pre-wrap;word-break:break-all;' +
				 'font-family:monospace;font-size:12px;padding:8px;border:1px solid rgba(0,0,0,0.15);border-radius:8px'
		}, '');

		var body = E('div', {}, [
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Command')),
				E('div', {}, [ this.input, ' ',
					E('button', {
						'class': 'btn cbi-button-action',
						'click': ui.createHandlerFn(this, this.send)
					}, _('Send'))
				]),
				E('p', { 'class': 'hint' },
				  _('Commands are sent raw; fm160d appends the carriage return and waits for OK / ERROR / +CME ERROR. Most commands time out after 5 s; the ones that are known to be slow on real hardware get up to 20 s.'))
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Common queries')),
				quick
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Response')),
				this.statusline,
				this.output
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Diagnostics', 'fm160 diagnostics heading')),
				E('p', { 'class': 'hint' },
				  _('A text report of the state fm160d holds and the log lines it remembers, for a bug report. It reads only the daemon\'s cached state - it sends nothing to the modem, so asking for it cannot disturb what it is describing.')),
				E('p', { 'class': 'hint' },
				  _('It contains the modem\'s IMEI, serial number and the SIM\'s ICCID. Remove them before posting it publicly.')),
				E('div', {}, [
					E('button', {
						'class': 'btn cbi-button-action',
						'click': ui.createHandlerFn(this, this.diagShow)
					}, _('Fetch and show')),
					' ',
					E('button', {
						'class': 'btn cbi-button',
						'click': ui.createHandlerFn(this, this.diagDownload)
					}, _('Download as a file'))
				]),
				this.diagStatus,
				this.diagOut
			])
		]);

		return body;
	},

	diagFetch: function() {
		return api.diagnostics().then(function(res) {
			if (!res || typeof res.text !== 'string' || !res.text)
				throw new Error(res && res.error ?
						res.error : _('the daemon returned no report'));

			return res;
		});
	},

	/* The status line labels the report rather than summarising it: the
	 * numbers are the ones that decide whether the text below is worth
	 * reading at all.  A bundle built while everything is healthy says so;
	 * one that shows "128 of 4000 lines" is telling the reader that the
	 * interesting part has already scrolled out of the ring. */
	diagLabel: function(res) {
		var span = [];

		span.push(_('version') + ' ' + (res.version || '?'));
		span.push(_('up', 'fm160 diagnostics field') + ' ' + Math.round((res.uptime_ms || 0) / 1000) + ' s');
		span.push((res.bytes || 0) + ' ' + _('bytes'));
		span.push(_('log') + ': ' + (res.log_lines || 0) + '/' + (res.log_total || 0) +
			  ' ' + _('lines at level') + ' ' + (res.log_level == null ? '?' : res.log_level));

		return span.join(' | ');
	},

	diagShow: function() {
		return this.diagFetch().then(function(res) {
			this.diagStatus.textContent = this.diagLabel(res);
			this.diagOut.textContent = res.text;
			this.diagOut.scrollTop = 0;
		}.bind(this)).catch(function(e) {
			this.diagStatus.textContent = '';
			ui.addNotification(null,
				E('p', {}, _('Could not build the diagnostics report') + ': ' + e),
				'danger');
		}.bind(this));
	},

	/*
	 * The download is an object URL rather than a link to a handler, because
	 * the report is fetched once and kept in memory: a second HTTP round trip
	 * for the same bytes would not be the same report, and two bundles whose
	 * headers differ are two bundles that cannot be compared.
	 */
	diagDownload: function() {
		return this.diagFetch().then(function(res) {
			var stamp = new Date().toISOString().replace(/[:.]/g, '-');
			var blob = new Blob([ res.text ], { type: 'text/plain;charset=utf-8' });
			var url = URL.createObjectURL(blob);
			var a = E('a', {
				'href': url,
				'download': 'fm160-diagnostics-' + stamp + '.txt'
			});

			document.body.appendChild(a);
			a.click();
			document.body.removeChild(a);
			/* Revoking immediately cancels the download in some browsers;
			 * the object is small and the page short-lived, so it is left
			 * for a while rather than freed at once. */
			window.setTimeout(function() { URL.revokeObjectURL(url); }, 30000);

			this.diagStatus.textContent = this.diagLabel(res);
			this.diagOut.textContent = res.text;
			this.diagOut.scrollTop = 0;
		}.bind(this)).catch(function(e) {
			this.diagStatus.textContent = '';
			ui.addNotification(null,
				E('p', {}, _('Could not build the diagnostics report') + ': ' + e),
				'danger');
		}.bind(this));
	},

	send: function() {
		var cmd = (this.input.value || '').trim();

		if (!cmd) {
			ui.addNotification(null, E('p', {}, _('Enter a command first.')), 'warning');
			return Promise.resolve();
		}

		this.append('> ' + cmd);

		var limit = timeoutFor(cmd);

		return api.at(cmd, limit, '').then(function(res) {
			var status = res && res.status || 'unknown';
			var text = res && res.response ? res.response.replace(/\r/g, '') : '';

			this.statusline.innerHTML = '';
			this.statusline.appendChild(E('span', {
				'class': 'label ' + (status === 'ok' ? 'success' :
						     status === 'error' ? 'warning' : 'danger')
			}, status));
			this.statusline.appendChild(E('span', { 'class': 'hint' },
				' ' + _('limit') + ': ' + (limit / 1000) + ' s'));

			this.append(text.trim() ? text.trimEnd() : _('(empty response)'));
		}.bind(this)).catch(function(e) {
			this.append(_('call failed') + ': ' + e);
		}.bind(this));
	},

	append: function(text) {
		var el = this.output;

		el.textContent += (el.textContent ? '\n' : '') + text;
		el.scrollTop = el.scrollHeight;
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
