'use strict';

/*
 * The three lists that must agree about one ubus method.
 *
 * A method reaches a browser through three files that are written at three
 * different times and in three different languages, and nothing in the build
 * checks any of them against the others:
 *
 *   ubus_methods.c   the daemon's table - what EXISTS
 *   api.js           rpc.declare({ method: '...' }) - what the front end CALLS
 *   acl.d/*.json     the grant - what the signed-in user is ALLOWED to call
 *
 * All three failure modes are silent at build time and appear only in a browser
 * on a router:
 *
 *   declared but not in the daemon   "Object not found" on a button press
 *   called but not granted           "Access denied", and only for non-root
 *   granted but not on the daemon    a grant that looks like a promise and is not
 *
 * The middle one is the worst: it works for the administrator who is testing,
 * because root is not subject to ACLs, and fails for everyone else.
 *
 * The api.js list is obtained by loading the file with a recording rpc stub, so
 * it is the list the module really declares rather than a regex over its text.
 * The other two are structural and are read directly.
 *
 * Usage: node tools/jscheck/test_contract.js
 */

const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..', '..');
const PKG = path.join(ROOT, 'luci-app-fm160');
const DAEMON_C = path.join(ROOT, 'fm160d', 'src', 'ubus_methods.c');
const API_JS = path.join(PKG, 'htdocs', 'luci-static', 'resources', 'fm160', 'api.js');
const ACL_JSON = path.join(PKG, 'root', 'usr', 'share', 'rpcd', 'acl.d', 'luci-app-fm160.json');
const MENU_JSON = path.join(PKG, 'root', 'usr', 'share', 'luci', 'menu.d', 'luci-app-fm160.json');
const VIEW_DIR = path.join(PKG, 'htdocs', 'luci-static', 'resources', 'view', 'fm160');

let checks = 0;
let failures = 0;

function ok(cond, what) {
	checks++;
	if (cond)
		console.log('  ok   ' + what);
	else {
		failures++;
		console.log('  FAIL ' + what);
	}
}

function eqArr(actual, expected, what) {
	const a = JSON.stringify(actual), e = JSON.stringify(expected);
	checks++;
	if (a === e)
		console.log('  ok   ' + what);
	else {
		failures++;
		console.log('  FAIL ' + what);
		console.log('        got  ' + a);
		console.log('        want ' + e);
	}
}

/* --- the daemon's table --------------------------------------------- */

/*
 * The method table is one expression per line:
 *     UBUS_METHOD_NOARG("status",   handle_status),
 *     UBUS_METHOD("profile",   handle_profile,   profile_policy),
 * so the name is the first string after the macro.  The regex is checked below
 * against a planted line, because a pattern that matches nothing would make
 * every "is it granted" answer a false yes.
 */
function daemonMethods(src) {
	const names = [];
	const re = /UBUS_METHOD(?:_NOARG)?[ \t]*\([ \t]*"([A-Za-z_][A-Za-z0-9_]*)"/g;
	let m;

	while ((m = re.exec(src)))
		names.push(m[1]);
	return names;
}

const daemonText = fs.readFileSync(DAEMON_C, 'utf8');
const daemon = daemonMethods(daemonText);

/* --- what the front end declares ------------------------------------ */

const apiSrc = fs.readFileSync(API_JS, 'utf8');
const declared = [];

const api = new Function('baseclass', 'rpc', '_', apiSrc)(
	{ extend: function(o) { return o; } },
	{
		/* Recording the declarations is the whole trick: the module is loaded
		 * for real, so what comes out is what it declares, and a reworded
		 * rpc.declare would change this list rather than slip past a regex. */
		declare: function(o) {
			if (o && o.object === 'fm160')
				declared.push(o.method);
			return function() {};
		}
	},
	function(s) { return s; });

/* --- the grant ------------------------------------------------------ */

const acl = JSON.parse(fs.readFileSync(ACL_JSON, 'utf8'));
const entry = acl['luci-app-fm160'];
const grantedRead = (entry.read.ubus && entry.read.ubus.fm160) || [];
const grantedWrite = (entry.write.ubus && entry.write.ubus.fm160) || [];
const granted = grantedRead.concat(grantedWrite);

console.log('daemon: ' + daemon.length + ' methods, api.js declares ' + declared.length +
	    ', acl grants ' + grantedRead.length + ' read + ' + grantedWrite.length + ' write');
console.log();

/* --- the checks ----------------------------------------------------- */

console.log('== the method table, the declarations and the grant agree ==');

/* The four self-checks first: every assertion below is an "is it in the list"
 * question, so a list that is silently empty makes all of them pass. */
eqArr(daemonMethods('static const struct ubus_method m[] = {\n'
		     + '\tUBUS_METHOD_NOARG("planted",   h),\n'
		     + '\tUBUS_METHOD("planted2", h, p),\n'
		     + '};'), [ 'planted', 'planted2' ],
      'the daemon-table pattern reads both method macros');
ok(daemon.length > 15, 'the daemon table was read (' + daemon.length + ' methods)');
ok(declared.length > 15, 'api.js declared its methods (' + declared.length + ')');
ok(granted.length > 15, 'the ACL was read (' + granted.length + ' grants)');

/* 1. Everything the front end calls exists on the daemon. */
const notOnDaemon = declared.filter(function(m) { return daemon.indexOf(m) < 0; });
ok(notOnDaemon.length === 0,
	'every method api.js calls exists on the daemon' +
	(notOnDaemon.length ? ' -- missing: ' + notOnDaemon.join(', ') : ''));

/* 2. Everything the front end calls is granted.  This is the one that fails
 *    only for a non-root user, i.e. for everyone except the person testing. */
const notGranted = declared.filter(function(m) { return granted.indexOf(m) < 0; });
ok(notGranted.length === 0,
	'every method api.js calls is granted by the ACL' +
	(notGranted.length ? ' -- not granted: ' + notGranted.join(', ') : ''));

/* 3. Nothing is granted that the daemon does not have.  A grant for a
 *    non-existent method is not dangerous, it is misleading: it says a feature
 *    exists. */
const emptyGrants = granted.filter(function(m) { return daemon.indexOf(m) < 0; });
ok(emptyGrants.length === 0,
	'the ACL grants nothing the daemon does not have' +
	(emptyGrants.length ? ' -- phantom: ' + emptyGrants.join(', ') : ''));

/* 4. Read and write are separate lists and must not overlap: a method that is
 *    both is a sign the write list was extended without thinking about whether
 *    the read side needs it, or the other way round.  `profile` is the known
 *    and deliberate exception - it is a read that also reports the foreground
 *    hold, which is why it appears in both. */
const both = grantedRead.filter(function(m) { return grantedWrite.indexOf(m) >= 0; });
eqArr(both, [ 'profile' ],
      'the only method in both lists is "profile", which reports the foreground hold');

/* --- the menu points at files that exist ---------------------------- */

console.log();
console.log('== the menu points at views that exist ==');

const menu = JSON.parse(fs.readFileSync(MENU_JSON, 'utf8'));
const viewEntries = Object.keys(menu).filter(function(k) {
	return menu[k].action && menu[k].action.type === 'view';
});

let missingViews = [];

viewEntries.forEach(function(k) {
	const p = menu[k].action.path;

	if (!fs.existsSync(path.join(VIEW_DIR, p.split('/').pop() + '.js')))
		missingViews.push(k + ' -> ' + p);
});

ok(viewEntries.length >= 6, 'the menu declares its views (' + viewEntries.length + ')');
ok(missingViews.length === 0,
	'every menu entry has a view file' +
	(missingViews.length ? ' -- missing: ' + missingViews.join(', ') : ''));

/* Every view file is reachable from the menu: an orphan is a page nobody can
 * open, which in practice means a half-finished rename. */
const menuPaths = viewEntries.map(function(k) {
	return menu[k].action.path.split('/').pop() + '.js';
});
const orphans = fs.readdirSync(VIEW_DIR).filter(function(f) {
	return f.endsWith('.js') && menuPaths.indexOf(f) < 0;
});

ok(orphans.length === 0,
	'every view file is reachable from the menu' +
	(orphans.length ? ' -- orphaned: ' + orphans.join(', ') : ''));

/* Depth: a first-level menu item with no children cannot be opened, and a view
 * declared at the top level is how that happens by accident. */
const parents = Object.keys(menu).filter(function(k) {
	return menu[k].action && menu[k].action.type === 'firstchild';
});

ok(parents.length === 1, 'exactly one firstchild entry roots the app');
ok(viewEntries.every(function(k) { return k.indexOf('/') >= 0; }),
	'every view entry is nested under a parent');

/* --- report --------------------------------------------------------- */

console.log();
console.log((checks - failures) + '/' + checks + ' assertions passed');
if (failures) {
	console.log('test_contract: FAILED');
	process.exit(1);
}
console.log('test_contract: clean');
