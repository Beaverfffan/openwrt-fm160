'use strict';
'require view';
'require poll';
'require ui';
'require fs';
'require uci';
'require fm160.api as api';

/*
 * MWAN3 page: auto-registration of the WAN-side interfaces into mwan3.
 *
 * The discovery runs in /usr/sbin/fm160-mwan3-sync (ash, root): wired ethN
 * interfaces and the FM160 dial interface (proto *_fm160) become tracked
 * mwan3 interfaces plus members of the "balanced" policy.  This page only
 * edits the knobs in /etc/config/fm160, section "mwan3", and asks the
 * script for a status snapshot.
 *
 * Priority model (mwan3 native): members of a policy are tiered by metric,
 * the lowest tier carries ALL traffic and the next tier only takes over
 * when every member of the tier above is down - that is failover.  Weights
 * only matter BETWEEN members of the SAME tier: same metric, different
 * weight = true load balancing.  The defaults are failover on purpose:
 * wired first (metric 10), mobile as backup (metric 20).  For 3:1 real
 * balancing set both metrics equal, e.g. 10/3 and 10/1.
 */

var SYNC = '/usr/sbin/fm160-mwan3-sync';

function info(text) {
	ui.addNotification(null, E('p', {}, text), 'info');
}

function fail(text) {
	ui.addNotification(null, E('p', {}, text), 'danger');
}

function row(label, value) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td', 'style': 'width:34%' }, label),
		E('td', { 'class': 'td' }, value)
	]);
}

function section(title, children) {
	return E('div', { 'class': 'cbi-section' }, [ E('h3', {}, title) ].concat(children));
}

return view.extend({
	load: function() {
		return Promise.all([
			uci.load('fm160'),
			this.loadStatus()
		]);
	},

	loadStatus: function() {
		var self = this;
		return fs.exec(SYNC, [ 'status' ]).then(function(res) {
			try {
				self.mwan = JSON.parse((res && res.stdout) || '{}');
			} catch (e) {
				self.mwan = { error: String(e) };
			}
		}).catch(function(e) {
			self.mwan = { error: String((e && e.message) || e) };
		});
	},

	render: function() {
		var self = this;

		this.container = E('div', {});
		this.paint();

		poll.add(function() {
			if (!document.body.contains(self.container)) {
				poll.stop();
				return Promise.resolve();
			}
			return self.loadStatus().then(self.paint.bind(self));
		}, 5);

		return this.container;
	},

	g: function(key, def) {
		var v = uci.get('fm160', 'mwan3', key);
		return (v === null || v === undefined) ? def : v;
	},

	num: function(id, def, min, max) {
		var v = parseInt(this.g(id, def), 10);
		if (isNaN(v)) v = def;
		return Math.min(max, Math.max(min, v));
	},

	paint: function() {
		var self = this, st = this.mwan || {};

		this.container.innerHTML = '';

		if (st.error)
			this.container.appendChild(E('div', { 'class': 'alert-message warning' },
				_('The status snapshot failed') + ': ' + st.error));

		if (!st.installed)
			this.container.appendChild(E('div', { 'class': 'alert-message warning' }, [
				E('strong', {}, _('mwan3 is not installed.')),
				' ',
				_('The registration below is written to /etc/config/mwan3, but without the package installed it does nothing. Install it with') + ' ',
				E('code', {}, 'opkg install mwan3'), '.'
			]));

		/* --- settings ------------------------------------------------ */

		this.inEnabled = E('input', { 'type': 'checkbox' });
		this.inEnabled.checked = this.g('enabled', '0') === '1';

		this.inWiredMetric = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'size': 6 });
		this.inWiredMetric.value = this.num('wired_metric', 10, 1, 999);
		this.inWiredWeight = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'size': 6 });
		this.inWiredWeight.value = this.num('wired_weight', 3, 1, 1000);
		this.inMobileMetric = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'size': 6 });
		this.inMobileMetric.value = this.num('mobile_metric', 20, 1, 999);
		this.inMobileWeight = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'size': 6 });
		this.inMobileWeight.value = this.num('mobile_weight', 1, 1, 1000);

		this.inTrack4 = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'style': 'width:80%' });
		this.inTrack4.value = L.toArray(this.g('track_ip4', '223.5.5.5')).join(' ');
		this.inTrack6 = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'style': 'width:80%' });
		this.inTrack6.value = L.toArray(this.g('track_ip6', '2400:3200::1')).join(' ');

		this.inCount = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'size': 4 });
		this.inCount.value = this.num('count', 1, 1, 10);
		this.inTimeout = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'size': 4 });
		this.inTimeout.value = this.num('timeout', 2, 1, 30);
		this.inInterval = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'size': 4 });
		this.inInterval.value = this.num('interval', 5, 1, 3600);
		this.inFailureInterval = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'size': 4 });
		this.inFailureInterval.value = this.num('failure_interval', 5, 1, 3600);
		this.inDown = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'size': 4 });
		this.inDown.value = this.num('down', 3, 1, 1000);
		this.inUp = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'size': 4 });
		this.inUp.value = this.num('up', 3, 1, 1000);

		this.container.appendChild(section(_('MWAN3 integration'), [
			E('table', { 'class': 'table' }, [
				row(_('Auto-register into mwan3'), this.inEnabled),
				row(_('Wired priority (metric)'), this.inWiredMetric),
				row(_('Wired weight'), this.inWiredWeight),
				row(_('Mobile priority (metric)'), this.inMobileMetric),
				row(_('Mobile weight'), this.inMobileWeight),
				row(_('Track targets IPv4 (space separated)'), this.inTrack4),
				row(_('Track targets IPv6 (space separated)'), this.inTrack6),
				row(_('Ping count per round'), this.inCount),
				row(_('Ping timeout (s)'), this.inTimeout),
				row(_('Check interval (s)'), this.inInterval),
				row(_('Failure check interval (s)'), this.inFailureInterval),
				row(_('Rounds to declare down'), this.inDown),
				row(_('Rounds to declare up'), this.inUp)
			]),
			E('p', { 'class': 'hint' }, [
				E('strong', {}, _('Lower metric wins failover; equal metrics balance by weight.')),
				' ',
				_('With the defaults above this is FAILOVER, not balancing: wired eth (metric 10) carries all traffic while it is up, and the FM160 link (metric 20) only takes over when wired goes down. Weights apply between members of the same metric tier only - set both classes to the same metric (e.g. 10/3 wired and 10/1 mobile) for true weighted balancing while both are up. Interfaces are discovered automatically - wired (device ethN) and the FM160 dial interface (whichever dial mode is active). Everything this feature writes into mwan3 is tagged and cleaned up when the interface disappears or auto-registration is turned off; your own mwan3 sections are never touched.')
			]),
			E('div', { 'class': 'cbi-page-actions', 'style': 'text-align:left' }, [
				E('button', {
					'class': 'btn cbi-button-action',
					'click': ui.createHandlerFn(this, this.save)
				}, _('Save and sync')),
				' ',
				E('button', {
					'class': 'btn cbi-button',
					'click': ui.createHandlerFn(this, this.syncNow)
				}, _('Sync now')),
				' ',
				E('button', {
					'class': 'btn cbi-button-negative',
					'click': ui.createHandlerFn(this, this.unregister)
				}, _('Remove registration'))
			])
		]));

		/* --- status --------------------------------------------------- */

		var rows = [];
		(st.desired || []).forEach(function(d) {
			rows.push(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, d.name),
				E('td', { 'class': 'td' }, d.family),
				E('td', { 'class': 'td' }, d.class === 'wired' ? _('wired') : _('mobile')),
				E('td', { 'class': 'td' }, d.device || '-')
			]));
		});

		var managedNames = (st.managed || []).map(function(m) { return m.name; });

		this.container.appendChild(section(_('Discovered interfaces'), [
			rows.length ? E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr' }, [
					E('th', { 'class': 'th' }, _('Interface')),
					E('th', { 'class': 'th' }, _('Family', 'fm160 mwan3 table')),
					E('th', { 'class': 'th' }, _('Class')),
					E('th', { 'class': 'th' }, _('Device'))
				])
			].concat(rows)) :
				E('p', { 'class': 'hint' }, _('Nothing discovered yet. Auto-registration is off, or no WAN-side interface exists.')),
			E('p', { 'class': 'hint' },
				(managedNames.length ?
					_('Registered in mwan3: %s').format(managedNames.join(', ')) + ' ' :
					_('No fm160-managed mwan3 sections yet.') + ' ') +
				_('The hotplug hook re-runs the registration on every interface up/down, so plugging or unplugging a cable is enough - no save needed.'))
		]));

		this.liveHost = E('pre', { 'class': 'hint', 'style': 'white-space:pre-wrap' },
			_('Live "mwan3 interfaces" output appears here after Sync now.'));
		this.container.appendChild(section(_('Live state'), [ this.liveHost ]));
	},

	save: function() {
		var self = this;

		function num(id, def, min, max) {
			var v = parseInt(self[id].value, 10);
			if (isNaN(v)) v = def;
			return String(Math.min(max, Math.max(min, v)));
		}

		function track(input) {
			return input.value.split(/[\s,]+/).filter(function(s) { return s; });
		}

		uci.set('fm160', 'mwan3', 'enabled', this.inEnabled.checked ? '1' : '0');
		uci.set('fm160', 'mwan3', 'wired_metric', num('inWiredMetric', 10, 1, 999));
		uci.set('fm160', 'mwan3', 'wired_weight', num('inWiredWeight', 3, 1, 1000));
		uci.set('fm160', 'mwan3', 'mobile_metric', num('inMobileMetric', 20, 1, 999));
		uci.set('fm160', 'mwan3', 'mobile_weight', num('inMobileWeight', 1, 1, 1000));
		uci.set('fm160', 'mwan3', 'track_ip4', track(this.inTrack4));
		uci.set('fm160', 'mwan3', 'track_ip6', track(this.inTrack6));
		uci.set('fm160', 'mwan3', 'count', num('inCount', 1, 1, 10));
		uci.set('fm160', 'mwan3', 'timeout', num('inTimeout', 2, 1, 30));
		uci.set('fm160', 'mwan3', 'interval', num('inInterval', 5, 1, 3600));
		uci.set('fm160', 'mwan3', 'failure_interval', num('inFailureInterval', 5, 1, 3600));
		uci.set('fm160', 'mwan3', 'down', num('inDown', 3, 1, 1000));
		uci.set('fm160', 'mwan3', 'up', num('inUp', 3, 1, 1000));

		api.opLog('mwan3 config', 'settings changed from mwan page', 'initiated');

		return uci.save()
			.then(function() { return uci.apply(); })
			.then(function() { return self.runSync(); })
			.then(function() { info(_('Saved and synced.')); })
			.catch(function(e) { fail(_('Save failed') + ': ' + String((e && e.message) || e)); });
	},

	syncNow: function() {
		var self = this;
		api.opLog('mwan3 sync', 'manual sync from mwan page', 'initiated');
		return this.runSync().then(function() {
			info(_('Synced.'));
		}).catch(function(e) {
			fail(_('Sync failed') + ': ' + String((e && e.message) || e));
		});
	},

	runSync: function() {
		var self = this;
		return fs.exec(SYNC, [ '--force' ]).then(function(res) {
			if (res && res.code)
				fail(_('fm160-mwan3-sync exited with') + ' ' + res.code +
					((res.stderr && res.stderr.trim()) ? ': ' + res.stderr.trim() : ''));
			return self.loadStatus();
		}).then(self.paint.bind(self)).then(self.loadLive.bind(self));
	},

	loadLive: function() {
		var host = this.liveHost;
		if (!host)
			return Promise.resolve();
		return fs.exec('/usr/sbin/mwan3', [ 'interfaces' ]).then(function(res) {
			host.textContent = ((res && res.stdout) || '').trim() ||
				_('mwan3 is not installed.');
		}).catch(function() {
			host.textContent = _('mwan3 is not installed.');
		});
	},

	unregister: function() {
		var self = this;
		return ui.showModal(_('Remove mwan3 registration?'), [
			E('p', {}, _('All fm160-managed mwan3 interface and member sections are deleted and removed from every policy. Sections you wrote yourself are not touched. The interfaces themselves keep working - only the mwan3 side is unwound.')),
			E('div', { 'class': 'right' }, [
				E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')),
				' ',
				E('button', {
					'class': 'btn cbi-button-negative',
					'click': function() {
						ui.hideModal();
						uci.set('fm160', 'mwan3', 'enabled', '0');
						api.opLog('mwan3 unregister', 'registration removed from mwan page', 'initiated');
						return uci.save().then(function() { return uci.apply(); })
							.then(function() { return self.runSync(); })
							.then(function() { info(_('Registration removed.')); });
					}
				}, _('Remove registration'))
			])
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
