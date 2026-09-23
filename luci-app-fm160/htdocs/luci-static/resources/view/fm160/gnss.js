'use strict';
'require view';
'require poll';
'require ui';
'require fm160.api as api';

/*
 * GNSS page (M5).
 *
 * This page is mostly a READER, and deliberately so.  Of the four GNSS
 * settings the module has, three are read-only here and only two can be
 * written at all:
 *
 *   engine      AT+GTGPSPOWER   writable, NOT stored by the module
 *   satellites  AT+GTGPSCFG=2   writable, stored - gated on AT+GTGPSCFG=?
 *   AGPS server AT+AGPSSERV     read-only in this milestone
 *   EPO         AT+GTGPSEPO     read-only in this milestone
 *
 * Everything else on the page comes out of fm160d's cached snapshot.
 *
 * Three things this page must get right, because getting them wrong would
 * misinform exactly when a user is most likely to be looking:
 *
 *   1. "Engine on, 0 satellites, no fix" is NORMAL indoors, not a fault.  It
 *      is rendered as a plain statement, not as an error, and never with a red
 *      banner.  The measured no-fix frame contains no RMC and no GGA, so the
 *      absence of a position says nothing about whether NMEA is healthy.
 *
 *   2. Missing and zero are different.  An absent elevation, DOP or SNR is
 *      printed as "-", while a measured zero is printed as 0.  A page that
 *      renders both as 0 invents data, and this is a page where "0" is a very
 *      common and very meaningful answer.
 *
 *   3. The satellite combination may only be offered from the intersection of
 *      the FM160 manual's values and the modem's own AT+GTGPSCFG=? answer, and
 *      that answer describes each FIELD separately: measured 2026-09-19, x=0 is
 *      (0-2) while x=2 is (0-15).  Only the x=2 set licenses the constellation
 *      write, so this page offers x=2's values and shows the other fields' sets
 *      as belonging to other settings - displaying their union as though it
 *      were the combination list would quote a different field's answer.
 *
 * One thing about the polling: the NMEA tier in fm160d runs ONLY while a page
 * holds the foreground - with nothing watching, AT+GTGPS? would be a command
 * per cycle that nobody reads.  So render() marks this page as foreground
 * through api.profile(true), exactly as the cells page does.  Without it the
 * page would sit still and look broken while the daemon was behaving perfectly.
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

/* Same contract as the M4 pages: "ok" means the write AND its read-back both
 * completed, i.e. the modem agrees the setting now holds that value. */
function reportWrite(res, what) {
	var status = (res && res.status) || 'unknown';

	if (status === 'ok')
		ui.addNotification(null, E('p', {}, what + ' ' + _('confirmed by read-back.')), 'info');
	else
		ui.addNotification(null, E('p', {},
			what + ' ' + _('failed') + ': ' + status + ' - ' +
			((res && res.response) || _('(no response)'))), 'danger');
}

function cell(v, opts) {
	return E('td', (opts && opts['class']) ? { 'class': opts['class'] } : {}, v);
}

/* A number that may legitimately be missing.  reported() rejects the u32-wrapped
 * sentinel; anything else is a real measurement and is shown as-is. */
function num(v, unit) {
	if (!api.reported(v))
		return '-';
	return (unit ? v + ' ' + unit : String(v));
}

function row(label, value) {
	return E('tr', { 'class': 'tr' }, [
		cell(label, { 'class': 'td left' }),
		cell(value)
	]);
}

return view.extend({
	load: function() {
		return api.status();
	},

	render: function(state) {
		var self = this;

		this.container = E('div', {});
		this.paint(state);

		/* api.profile(true) is what puts the daemon into foreground mode, and
		 * the NMEA tier only exists in foreground mode - see the note at the
		 * top of this file. */
		poll.add(function() {
			if (!document.body.contains(self.container)) {
				poll.stop();
				return Promise.resolve();
			}
			return api.profile(true).then(api.status).then(function(s) {
				self.paint(s);
			});
		}, 2);

		return this.container;
	},

	paint: function(state) {
		this.state = state;
		this.container.innerHTML = '';

		this.container.appendChild(this.renderBanner(state));
		this.container.appendChild(this.renderEngine(state));
		this.container.appendChild(this.renderFix(state));
		this.container.appendChild(this.renderConstellations(state));
		this.container.appendChild(this.renderSatellites(state));
		this.container.appendChild(this.renderConfig(state));
		this.container.appendChild(this.renderAgps(state));
		this.container.appendChild(this.renderDiagnostics(state));
	},

	/* --- banner ----------------------------------------------------- */

	renderBanner: function(state) {
		var out = E('div', {});
		var level = api.gnssLevel(state);

		if (state.gnss === undefined) {
			out.appendChild(E('div', { 'class': 'alert-message warning' },
				_('fm160d does not publish GNSS data - the installed daemon is older than this page.')));
			return out;
		}

		/* Only a genuine failure gets the warning banner.  "Engine on, nothing
		 * in view" and "engine off" are states the user chose or the sky
		 * imposed, and colouring them as problems would train the user to
		 * ignore this banner. */
		var cls = (level === 'down') ? 'alert-message warning'
					     : 'alert-message success';

		out.appendChild(E('div', { 'class': cls }, api.gnssStateText(state)));

		var r = api.gnssReadOf(state);

		if (r.read_error)
			out.appendChild(E('div', { 'class': 'alert-message warning' },
				_('AT+GTGPS? answered ERROR. On this module that means the engine is off, not that the receiver is broken - switch it on above and read again.')));

		return out;
	},

	/* --- engine ----------------------------------------------------- */

	renderEngine: function(state) {
		var self = this;
		var e = api.gnssEngineOf(state), r = api.gnssReadOf(state);
		var body = E('div', {});

		var btn = E('button', {
			'class': 'btn cbi-button-' + (e.on ? 'negative' : 'positive'),
			'click': function(ev) {
				ev.currentTarget.blur();
				self.writePower(!e.on);
			}
		}, e.on ? _('Turn the engine off') : _('Turn the engine on'));

		body.appendChild(E('p', {}, [
			E('strong', {}, _('Engine:') + ' '),
			e.known ? (e.on ? _('on', 'fm160 on state') : _('off', 'fm160 off state')) : _('not read yet'),
			' ',
			btn
		]));

		body.appendChild(E('p', { 'class': 'hint' },
			_('AT+GTGPSPOWER is the one GNSS setting this module does NOT store. It comes back off after every power cycle, so fm160d re-applies it whenever the module reappears.') +
			' ' +
			(e.autostart
				? _('Autostart is enabled in /etc/config/fm160, so it is switched back on automatically.')
				: _('Autostart is off in /etc/config/fm160 (option gnss_autostart), so it stays off until it is switched on here.'))));

		/* The two counters that describe a broken-looking-but-fine state. */
		if (e.on && r.empty_frames)
			body.appendChild(E('p', { 'class': 'hint' },
				_('The last answer contained no NMEA sentence.') + ' ' +
				_('The receiver needs a moment after switching on; one or two of these in a row is the measured normal.')));

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('GNSS engine')),
			body
		]);
	},

	/* --- current fix ------------------------------------------------ */

	renderFix: function(state) {
		var f = api.gnssFixOf(state), r = api.gnssReadOf(state);
		var rows = [];

		rows.push(row(_('State'), api.gnssStateText(state)));
		rows.push(row(_('Fix type'), api.gnssFixTypeName(f.fix_type)));
		rows.push(row(_('Satellites in view'), num(f.visible)));
		rows.push(row(_('Satellites used in fix'), num(f.sats_used)));
		rows.push(row(_('Satellites in use (GGA)'), num(f.sats_in_use)));
		rows.push(row(_('Average SNR (best)'), api.reported(f.snr_best_db) ? f.snr_best_db + ' dB' : '-'));
		rows.push(row('PDOP / HDOP / VDOP',
			api.gnssDop(f.pdop_x10) + ' / ' + api.gnssDop(f.hdop_x10) + ' / ' + api.gnssDop(f.vdop_x10)));

		rows.push(row(_('Latitude'), api.gnssLat(state)));
		rows.push(row(_('Longitude'), api.gnssLon(state)));
		rows.push(row(_('Altitude', 'fm160 gnss field'), api.reported(f.alt_dm) ? (f.alt_dm / 10).toFixed(1) + ' m' : '-'));
		rows.push(row(_('Geoid separation'), api.reported(f.geoid_dm) ? (f.geoid_dm / 10).toFixed(1) + ' m' : '-'));

		var kmh = api.gnssSpeedKmh(state);
		rows.push(row(_('Speed'), kmh === null ? '-' : kmh.toFixed(1) + ' km/h'));
		rows.push(row(_('Course'), api.reported(f.course_d10) ? (f.course_d10 / 10).toFixed(1) + '\u00b0' : '-'));
		rows.push(row(_('Quality'), api.reported(f.quality) ? api.gnssQualityName(f.quality) : '-'));
		rows.push(row(_('UTC'), f.utc || '-'));
		rows.push(row(_('Date'), f.date || '-'));
		rows.push(row(_('Age of this reading'), api.fmtAge(r.age_ms)));

		var body = E('div', {});

		body.appendChild(E('table', { 'class': 'table' }, rows));

		if (!f.has_position)
			body.appendChild(E('p', { 'class': 'hint' },
				_('There is no position, so the position rows above are empty rather than zero. An indoor receiver with no sky view reports exactly this, and the module omits the position and time sentences entirely in that state - their absence is not damage.')));

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Position')),
			body
		]);
	},

	/* --- constellations --------------------------------------------- */

	renderConstellations: function(state) {
		var cons = api.gnssConstsOf(state);
		var body = E('div', {});

		if (!cons.length) {
			body.appendChild(E('p', {}, _('No constellation has been heard from yet.')));
			return E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Constellations')),
				body
			]);
		}

		var rows = [ E('tr', { 'class': 'tr table-titles' }, [
			cell(_('Constellation'), { 'class': 'th' }),
			cell(_('Talker'), { 'class': 'th' }),
			cell(_('In view (reported)'), { 'class': 'th' }),
			cell(_('Parsed'), { 'class': 'th' }),
			cell(_('Used in fix'), { 'class': 'th' }),
			cell(_('Best SNR'), { 'class': 'th' })
		]) ];

		cons.forEach(function(c) {
			rows.push(E('tr', { 'class': 'tr' }, [
				cell(api.gnssConstName(c)),
				cell(c.talker || '-'),
				cell(num(c.visible)),
				cell(num(c.parsed)),
				cell(num(c.in_fix)),
				cell(api.reported(c.snr_best_db) ? c.snr_best_db + ' dB' : '-')
			]));
		});

		body.appendChild(E('table', { 'class': 'table' }, rows));
		body.appendChild(E('p', { 'class': 'hint' },
			_('"In view" is what the module itself reports, and "parsed" is how many of those satellites were actually present in the NMEA. They are shown side by side on purpose: if they differ, the answer was truncated, and that is a transport problem rather than a sky problem.')));

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Constellations')),
			body
		]);
	},

	/* --- satellites ------------------------------------------------- */

	renderSatellites: function(state) {
		var sats = api.gnssSatsOf(state);
		var body = E('div', {});

		if (!sats.length) {
			body.appendChild(E('p', {},
				_('0 satellites in view. On this hardware that is the normal reading indoors - the module has antennas, but it needs a view of the sky to find anything.')));
			return E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Satellites')),
				body
			]);
		}

		var rows = [ E('tr', { 'class': 'tr table-titles' }, [
			cell(_('Constellation'), { 'class': 'th' }),
			cell('PRN', { 'class': 'th' }),
			cell(_('Elevation'), { 'class': 'th' }),
			cell(_('Azimuth'), { 'class': 'th' }),
			cell('SNR', { 'class': 'th' }),
			cell(_('In fix'), { 'class': 'th' })
		]) ];

		/* Satellites in the fix first: that is the short list a user is
		 * actually looking for when there IS a fix. */
		sats.slice().sort(function(a, b) {
			if (!!b.in_fix !== !!a.in_fix)
				return b.in_fix ? 1 : -1;
			return (b.snr_db || 0) - (a.snr_db || 0);
		}).forEach(function(s) {
			rows.push(E('tr', { 'class': 'tr' }, [
				cell(s.talker || '-'),
				cell(num(s.prn)),
				cell(api.reported(s.elev_deg) ? s.elev_deg + '\u00b0' : '-'),
				cell(api.reported(s.azim_deg) ? s.azim_deg + '\u00b0' : '-'),
				cell(api.reported(s.snr_db) ? s.snr_db + ' dB' : '-'),
				cell(s.in_fix ? _('yes') : _('no'))
			]));
		});

		body.appendChild(E('table', { 'class': 'table' }, rows));

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Satellites') + ' (' + sats.length + ')'),
			body
		]);
	},

	/* --- stored configuration --------------------------------------- */

	renderConfig: function(state) {
		var self = this;
		var c = api.gnssCfgOf(state), caps = api.gnssCfgCapsOf(state);
		var body = E('div', {});
		var rows = [];

		rows.push(row(_('Satellite combination'),
			c.valid ? api.gnssCfgLabel(c.constellation) : '-'));
		rows.push(row(_('SUPL version'),
			api.reported(c.supl_version) ? c.supl_version : '-'));
		rows.push(row(_('SUPL certificate'), api.reported(c.cert) ? c.cert : '-'));
		/* x=1 is missing on this firmware, so "not reported" and "0" must be
		 * distinguishable rather than both printed as 0. */
		rows.push(row(_('xtra'), c.xtra_present ? String(c.xtra) : _('not reported by this firmware')));
		if (c.unknown)
			rows.push(row(_('Unrecognised entries'), c.unknown));
		if (c.age_ms !== undefined)
			rows.push(row(_('Age', 'fm160 gnss field'), api.fmtAge(c.age_ms)));

		body.appendChild(E('table', { 'class': 'table' }, rows));

		/* AT+GTGPSCFG has several fields and the modem answers each one with
		 * its own value set - on this unit x=0 is (0-2) while x=2, the
		 * satellite combination, is (0-15).  The other fields are shown as
		 * exactly that: another setting's range.  Folding them together into
		 * one list would put values in front of the reader as combinations
		 * when they describe a different setting altogether. */
		var writeX = api.gnssCfgWriteX(state);
		var others = api.gnssCfgCapsByX(state).filter(function(r) {
			return r.x !== writeX;
		});

		if (others.length) {
			body.appendChild(E('table', { 'class': 'table' }, others.map(function(r) {
				return E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td left' },
						(r.x === 4)
							? _('a group with no field name')
							: _('field') + ' x=' + r.x),
					E('td', { 'class': 'td left' },
						(r.values || []).join(', '))
				]);
			})));
			body.appendChild(E('p', { 'class': 'hint' },
				_('The modem reports a separate value set for each field of AT+GTGPSCFG. Only x=%d is the satellite combination and only its set below can be written; the other fields are listed so that their numbers are not read as combinations.').format(writeX)));
		}

		var choices = api.gnssCfgChoices(state);

		if (!caps.valid) {
			body.appendChild(E('p', { 'class': 'hint' },
				_('The modem has not answered AT+GTGPSCFG=? yet, so changing the satellite combination is disabled. The setting is stored, and a value the modem does not accept could not be taken back by guessing.')));
		}
		else if (!choices.length) {
			body.appendChild(E('p', { 'class': 'hint' },
				_('AT+GTGPSCFG=? answered, but nothing in it matches a combination the FM160 manual documents. Writing is disabled rather than guessing what the numbers mean.')));
		}
		else if (choices.length === 1) {
			body.appendChild(E('p', { 'class': 'hint' },
				_('The only documented combination this modem accepts is already the one in use.')));
		}
		else {
			var sel = E('select', { 'class': 'cbi-input-select' }, choices.map(function(v) {
				return E('option', {
					'value': v,
					'selected': (v === c.constellation) ? 'selected' : null
				}, api.gnssCfgLabel(v));
			}));

			body.appendChild(E('div', { 'class': 'cbi-value' }, [
				E('label', { 'class': 'cbi-value-title' }, _('Change to')),
				E('div', { 'class': 'cbi-value-field' }, [
					sel, ' ',
					E('button', {
						'class': 'btn cbi-button-apply',
						'click': function(ev) {
							ev.currentTarget.blur();
							self.writeCfg(Number(sel.value));
						}
					}, _('Write'))
				])
			]));

			body.appendChild(E('p', { 'class': 'hint' },
				_('Only combinations that are both documented by the manual and reported by this modem are offered. The setting survives a power cycle.') + ' ' +
				_('AT+GTGPSCFG=? has not been observed on this hardware, so the list may be narrower than the modem really allows.')));
		}

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Configuration')),
			body
		]);
	},

	/* --- AGPS -------------------------------------------------------- */

	renderAgps: function(state) {
		var a = api.gnssAgpsOf(state);
		var body = E('div', {});

		body.appendChild(E('table', { 'class': 'table' }, [
			row(_('EPO'), a.valid ? api.gnssEpoName(a.epo) : '-'),
			row(_('SUPL server'), a.server || '-'),
			row(_('SUPL port'), api.reported(a.port) && a.port > 0 ? a.port : '-')
		]));

		body.appendChild(E('p', { 'class': 'hint' },
			_('AGPS is read-only in this version. It is displayed because it decides how long the receiver takes to get its first fix, and because a wrong server looks exactly like a weak signal from the outside.')));

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Assisted GPS')),
			body
		]);
	},

	/* --- diagnostics ------------------------------------------------ */

	renderDiagnostics: function(state) {
		var r = api.gnssReadOf(state);
		var rows = [];

		rows.push(row(_('Sentences in the last block'), num(r.sentences)));
		rows.push(row(_('NMEA bytes'), num(r.nmea_bytes)));
		rows.push(row(_('Answer size'), num(r.resp_bytes)));
		rows.push(row(_('Age of the last block'), api.fmtAge(r.age_ms)));
		rows.push(row(_('Bad checksum'), num(r.bad_checksum)));
		rows.push(row(_('Malformed'), num(r.bad_shape)));
		rows.push(row(_('Uninterpreted'), num(r.ignored)));
		rows.push(row(_('Empty answers in a row'), num(r.empty_frames)));
		if (api.reported(r.empty_bytes))
			rows.push(row(_('Size of the last empty answer'), num(r.empty_bytes)));
		if (r.empty_age_ms !== undefined)
			rows.push(row(_('Age of the last empty answer'), api.fmtAge(r.empty_age_ms)));

		var body = E('div', {});

		body.appendChild(E('table', { 'class': 'table' }, rows));

		body.appendChild(E('p', { 'class': 'hint' },
			_('"Uninterpreted" counts sentences that are well formed but carry nothing this daemon uses, such as VTG and TXT - a non-zero value there is not a problem.') +
			' ' +
			_('A non-zero "bad checksum" is: it means the module sent a sentence that does not match its own checksum, and that one sentence was discarded while the rest of the block was kept.')));

		/* The extra field past the last GSV quad group: recorded by the daemon
		 * as evidence, never interpreted. Shown so a firmware change becomes
		 * visible instead of silent. */
		if (r.gsv_trailing)
			body.appendChild(E('p', { 'class': 'hint' },
				_('A GSV sentence carried one field more than the standard describes (signal id)') +
				(api.reported(r.gsv_trailing_value) ? ': ' + r.gsv_trailing_value : '') +
				_('. It is recorded, not interpreted.')));

		var raw = api.gnssRaw(state);

		if (raw) {
			body.appendChild(E('h4', {}, _('Last NMEA block')));
			body.appendChild(E('pre', {
				'style': 'max-height: 18em; overflow: auto; white-space: pre-wrap;'
			}, raw));
		}

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Diagnostics', 'fm160 diagnostics heading')),
			body
		]);
	},

	/* --- writes ----------------------------------------------------- */

	writePower: function(on) {
		var self = this;
		var what = on ? _('Switch the GNSS engine on') : _('Switch the GNSS engine off');

		var warn = on
			? [ _('This powers up the GNSS receiver. It draws current even while it finds nothing, and on this hardware it is normally the only reason fm160d is talking to the modem at all.'),
			    _('The setting is not stored by the module, so it will be off again after the next power cycle unless autostart is enabled.') ]
			: [ _('The receiver stops, and at least one NMEA read will answer ERROR until it is switched on again.') ];

		return confirmPrompt(what, warn, what).then(function(ok) {
			if (!ok)
				return;

			return api.setGnss(!!on).then(function(res) {
				reportWrite(res, what);
				return api.status().then(function(s) { self.paint(s); });
			}).catch(function(e) {
				ui.addNotification(null, E('p', {}, what + ' ' + _('failed') + ': ' + e), 'danger');
			});
		});
	},

	writeCfg: function(value) {
		var self = this;
		var what = _('Set the satellite combination to') + ' ' + api.gnssCfgLabel(value);

		return confirmPrompt(what, [
			_('This is stored in the module and survives a power cycle.'),
			_('The daemon reads the setting back after writing it, and only reports success once the modem agrees - so a failure here means the value was not applied.'),
			_('The engine keeps running throughout; no reset is involved.')
		], _('Write to modem')).then(function(ok) {
			if (!ok)
				return;

			return api.setGnssCfg(value).then(function(res) {
				reportWrite(res, what);
				return api.status().then(function(s) { self.paint(s); });
			}).catch(function(e) {
				ui.addNotification(null, E('p', {}, what + ' ' + _('failed') + ': ' + e), 'danger');
			});
		});
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
