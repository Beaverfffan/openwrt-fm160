#!/usr/bin/env python3
"""Check the FM160 LuCI translation against everything that consumes it.

    python tools/i18n/check.py [--repo <openwrt-fm160>] [--luci <luci checkout>]
                               [--po2lmo <binary>] [--node <node>] [--quiet]

Lives in the repository rather than in the author's scratch area, so that the
package carries its own proof and CI can run it.  --luci, --po2lmo and --node
are external inputs: a luci checkout to compare against and to lift the real
cbi.js out of, a po2lmo built by build-po2lmo.sh, and node to run cbi.js.

The interesting property of this file is that it is not a spelling checker for
a .po.  A .po can be perfectly well-formed, complete and reviewed and still
translate nothing at all, because THREE independent implementations have to
agree on a 32-bit hash before any string is ever displayed:

    writer   po2lmo.c    snprintf(key, ..., "%s", msg->id); sfh_hash(key, len, len)
    reader   lmo.c       lmo_canon_hash(key, keylen, ctx, ctxlen, -1) -> sfh_hash
    reader   cbi.js      var k = trimws(s); TR[sfh(k)]

and they do not all agree.  The writer hashes the msgid *verbatim*; both
readers collapse whitespace first.  So a msgid containing a double space, a
tab, or a leading/trailing space is written under one hash and looked up under
another, and the string silently falls back to English.  Nothing about the .po
looks wrong.  That is the failure this script exists to make impossible, and it
is checked three ways, because each one alone can be fooled:

  * the msgid must be whitespace-canonical (section "canonical msgid")
  * the hash the real cbi.js computes for it must be non-null and injective
    (section "hash agreement", which loads the real function, not a copy)
  * the hash po2lmo actually wrote into a real .lmo must equal that hash
    (section "end to end", which builds the .lmo and reads its index back)

The second and third need node and po2lmo.  When they are missing they are
reported as SKIPPED with the reason, never as passing -- a check that cannot
run is not a check that succeeded.

What this does NOT prove: that a zh-cn user sees Chinese.  That needs the
package built and installed on a device with the language selected.  What it
proves is that every hash on the path lines up, which is the part that is
invisible when it is wrong.
"""
import argparse
import io
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------------------
# paths
# ---------------------------------------------------------------------------

_MSYS = re.compile(r'^/([A-Za-z])/(.*)$')


def native(path):
    """Translate an MSYS path to one this interpreter's OS can resolve.

    This script is driven from a Git Bash shell but runs under a native Windows
    python, so `--node /c/Program Files/node/node.exe` is a path that the
    Windows loader cannot open -- subprocess fails with WinError 2, "system
    cannot find the file", which reads as "node is not installed" rather than
    "the path form is wrong".  Two checks then SKIP and the run looks healthy.

    On POSIX this is the identity, so the build machine is unaffected.
    """
    if os.name == 'nt':
        m = _MSYS.match(path)
        if m:
            return '%s:/%s' % (m.group(1).upper(), m.group(2))
    return path


# ---------------------------------------------------------------------------
# harness
# ---------------------------------------------------------------------------


class Report:
    def __init__(self, quiet=False):
        self.quiet = quiet
        self.passed = 0
        self.failed = 0
        self.skipped = []
        self.warnings = []
        self.failures = []

    def ok(self, what):
        self.passed += 1
        if not self.quiet:
            print('  ok   %s' % what)

    def fail(self, what, detail=None):
        self.failed += 1
        self.failures.append(what)
        print('  FAIL %s' % what)
        if detail:
            for line in str(detail).split('\n'):
                print('       %s' % line)

    def check(self, what, cond, detail=None):
        if cond:
            self.ok(what)
        else:
            self.fail(what, detail)
        return bool(cond)

    def skip(self, what, why):
        self.skipped.append((what, why))
        print('  SKIP %s -- %s' % (what, why))

    def warn(self, what, detail=None):
        self.warnings.append(what)
        print('  WARN %s' % what)
        if detail:
            for line in str(detail).split('\n'):
                print('       %s' % line)

    def section(self, name):
        print('\n== %s ==' % name)


# ---------------------------------------------------------------------------
# .po
# ---------------------------------------------------------------------------

_UNESCAPE = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\'}


def po_unescape(s):
    return re.sub(r'\\(.)', lambda m: _UNESCAPE.get(m.group(1), m.group(1)), s)


def po_escape(s):
    """The inverse, for the round-trip check.

    Only backslash and double quote need escaping in a `"..."` po string; an
    apostrophe inside it is literal.  `\\'` in a msgid is almost always the
    JavaScript spelling having leaked into the .po, which is a msgid that can
    never match anything, hence the explicit check for it.
    """
    return s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')


def entry_keys(e):
    """The lookup key(s) po2lmo builds for an entry, byte for byte.

    From po2lmo.c:158-173, where '\\1' and '\\2' are the C octal escapes for
    0x01 and 0x02:

        ctxt and id_plural   "%s\\1%s\\2%d"   ctxt, msgid, plural index
        ctxt only            "%s\\1%s"        ctxt, msgid
        id_plural only       "%s\\2%d"        msgid, plural index
        neither              "%s"             msgid

    Note the third case: a plural entry's key is NOT the bare msgid, so a
    plural upstream message and a plain msgid of ours never collide even when
    the words are the same.  Getting this wrong would invent collisions.

    The test is on truthiness, not on `is not None`: the parser gives a plain
    entry an empty dict, and `{} is not None` is true, so the stricter form
    would send every ordinary message down the plural branch and return no key
    for it at all.  That failure is silent and looks like "nothing collides".
    """
    if e['plural']:
        base = (e['ctxt'] + '\x01' if e['ctxt'] else '') + e['msgid']
        return [base + '\x02' + str(i) for i in sorted(e['plural'])]
    if e['ctxt']:
        return [e['ctxt'] + '\x01' + e['msgid']]
    return [e['msgid']]


def parse_po(path, allow_plural=False):
    """Return (header, entries).

    entries is a list of dicts with keys msgid/msgstr/ctxt/plural/source, in
    file order.  Raises ValueError on a shape this parser does not model,
    rather than skipping it -- a silently ignored construct is a missed
    translation.

    allow_plural is off for the .po we ship: we do not use plural forms, so a
    plural entry appearing in it is a mistake worth stopping on.  It is on for
    the upstream catalogue we compare against, which does use them and whose
    plural entries still occupy keys in the shared namespace.
    """
    text = io.open(path, encoding='utf-8').read()
    header = None
    entries = []
    cur = None          # 'id' | 'str' | 'ctxt' | 'idp' | ('pstr', n) | None
    id_ = None
    str_ = None
    ctxt = None
    id_plural = None
    plurals = None
    source = None

    def flush():
        nonlocal header
        if id_ is None:
            return
        if id_ == '' and header is None:
            header = str_
        else:
            if plurals and id_plural is None:
                raise ValueError(
                    'msgid %r carries msgstr[n] but no msgid_plural, which this '
                    'parser does not model' % id_)
            entries.append({'msgid': id_, 'msgstr': str_, 'ctxt': ctxt,
                            'plural': plurals or None, 'id_plural': id_plural,
                            'source': source})

    for lineno, raw in enumerate(text.split('\n'), 1):
        if raw.startswith('#:'):
            source = raw[2:].strip()
            continue
        if raw.startswith('#'):
            continue
        if raw.startswith('msgid_plural '):
            if not allow_plural:
                raise ValueError('line %d: plural forms are not modelled by this '
                                 'checker; extend it before using them' % lineno)
            id_plural = ''
            cur = 'idp'
            body = raw[13:]
        elif raw.startswith('msgstr['):
            if not allow_plural:
                raise ValueError('line %d: plural forms are not modelled by this '
                                 'checker; extend it before using them' % lineno)
            if plurals is None:
                raise ValueError('line %d: %r with no msgid before it'
                                 % (lineno, raw.split(' ')[0]))
            try:
                n = int(raw[7:raw.index(']')])
            except ValueError:
                raise ValueError('line %d: unparsable plural index in %r'
                                 % (lineno, raw))
            plurals[n] = ''
            cur = ('pstr', n)
            body = raw[raw.index(']') + 1:]
        elif raw.startswith('msgctxt '):
            # A context makes it a different message: the two readers build the
            # lookup key as ctxt + '\1' + msgid (lmo_canon_hash, and the
            # '\u0001' in cbi.js's _()).  Kept as its own field so that a
            # contexted msgid is never matched against a bare one.
            flush()
            id_, str_, ctxt = None, None, ''
            id_plural, plurals = None, {}
            cur = 'ctxt'
            body = raw[8:]
        elif raw.startswith('msgid '):
            flush()
            id_, str_ = '', ''
            id_plural, plurals = None, {}
            cur = 'id'
            body = raw[6:]
        elif raw.startswith('msgstr '):
            cur = 'str'
            body = raw[7:]
        elif raw.startswith('"'):
            body = raw
        elif raw.strip() == '':
            flush()
            id_ = str_ = ctxt = None
            id_plural, plurals = None, {}
            cur = None
            continue
        else:
            raise ValueError('line %d: unhandled po construct %r' % (lineno, raw))

        body = body.strip()
        if not (body.startswith('"') and body.endswith('"')) or len(body) < 2:
            raise ValueError('line %d: malformed string %r' % (lineno, body))
        chunk = po_unescape(body[1:-1])
        if cur == 'id':
            id_ += chunk
        elif cur == 'str':
            str_ += chunk
        elif cur == 'ctxt':
            ctxt += chunk
        elif cur == 'idp':
            id_plural += chunk
        elif isinstance(cur, tuple) and cur[0] == 'pstr':
            plurals[cur[1]] += chunk
        else:
            raise ValueError('line %d: continuation with no msgid/msgstr' % lineno)

    flush()
    return header, entries


WS = re.compile(r'[ \t\n\r\v\f]+')


def canon(s):
    """What the two readers collapse a lookup key to (lmo_canon_hash/trimws).

    Note \v, \f and \r: lmo_canon_hash uses isspace(), which covers all of them,
    while cbi.js's trimws() only collapses [ \t\n].  The two readers therefore
    disagree with EACH OTHER on a key containing a vertical tab or a form feed,
    which is another reason section "canonical msgid" refuses them outright.
    """
    return WS.sub(' ', s).strip()


# ---------------------------------------------------------------------------
# JavaScript sources
# ---------------------------------------------------------------------------

JS_BLOCK = re.compile(r'/\*[\s\S]*?\*/')
JS_LINE = re.compile(r'(?m)^[ \t]*//.*$')
JS_CALL = re.compile(r"(?<![\w$.])_\(\s*'((?:[^'\\]|\\.)*)'\s*"
                     r"(?:,\s*'((?:[^'\\]|\\.)*)')?\s*\)")

# A _() call the extractor cannot see, because JS_CALL only reads single quoted
# literals.  Kept separate so the coverage section can say so out loud instead
# of quietly reporting a smaller backlog.
JS_CALL_OTHER = re.compile(r'(?<![\w$.])_\(\s*["`]')


def strip_js_comments(text):
    return JS_LINE.sub(' ', JS_BLOCK.sub(' ', text))


def js_literal_calls(text):
    """Return [(msgid, ctxt)] for every _() call, ctxt None when absent.

    The second argument is the translation context and it changes the lookup
    key (the readers build ctxt + '\\1' + msgid), so dropping it here would let
    a contexted entry pass as an ordinary one and then never resolve.
    """
    return [(po_unescape(s), po_unescape(c) or None)
            for s, c in JS_CALL.findall(strip_js_comments(text))]


def extract_js_function(text, name):
    """Slice `function <name>(...) { ... }` out of a file, braces matched.

    Used to lift sfh()/u16()/s8()/trimws() out of the real cbi.js, so the hash
    comparison runs the shipped implementation rather than a transcription of
    it.  A transcription would agree with itself no matter what upstream does.
    """
    m = re.search(r'(?m)^function\s+%s\s*\(' % re.escape(name), text)
    if not m:
        return None
    i = text.index('{', m.end() - 1)
    depth = 0
    for j in range(i, len(text)):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return text[m.start():j + 1]
    return None


# ---------------------------------------------------------------------------
# .lmo
# ---------------------------------------------------------------------------


def parse_lmo(path):
    """Return (plural_formula, entries) from a compiled .lmo.

    Layout, from luci-base/src/lib/lmo.c and lmo.h: values back to back, each
    padded to a multiple of 4; then the index, 4 big-endian uint32 per entry
    (key_id, val_id, offset, length), sorted by key_id; then the offset of the
    index as a big-endian uint32 in the last four bytes.  The plural formula is
    stored as the entry with key_id 0 (po2lmo.c gives it key_id = val_id = 0),
    which is also what `TR['00000000']` is on the JavaScript side.
    """
    data = io.open(path, 'rb').read()
    if len(data) < 8:
        raise ValueError('%s: too short to be an .lmo' % path)
    idx_off = struct.unpack('>I', data[-4:])[0]
    if idx_off >= len(data) - 4:
        raise ValueError('%s: index offset %d is out of range' % (path, idx_off))
    slack = len(data) - 4 - idx_off
    # The index is a whole number of 16-byte records.  If it is not, the file
    # was written by something that altered its bytes on the way out -- in
    # practice a text-mode fopen on Windows, which rewrites every 0x0A byte in
    # the index as 0x0D 0x0A.  Integer division would quietly read a truncated
    # index and every comparison after that would be nonsense, so say what it
    # is instead: C's reader does the same division and gets the same wrong
    # answer, which is why this is worth failing loudly over.
    if slack % 16:
        raise ValueError(
            '%s: the index is not a whole number of 16-byte records\n'
            '         %d bytes from index offset %d to the trailing word, '
            'which is %d too many\n'
            '         a .lmo written in text mode has one extra byte per 0x0A '
            'byte in the file\n'
            '         rebuild po2lmo with the binary-mode shim '
            '(see tools/i18n/build-po2lmo.sh)'
            % (path, slack, idx_off, slack % 16))
    n = slack // 16
    entries = []
    for i in range(n):
        off = idx_off + 16 * i
        key_id, val_id, offset, length = struct.unpack('>IIII', data[off:off + 16])
        entries.append({
            'key_id': key_id,
            'val_id': val_id,
            'offset': offset,
            'length': length,
            'value': data[offset:offset + length],
        })
    plural = None
    for e in entries:
        if e['key_id'] == 0:
            plural = e['value'].decode('utf-8', 'replace')
            break
    return plural, entries


# ---------------------------------------------------------------------------
# C sources
# ---------------------------------------------------------------------------


def daemon_labels(text, funcname):
    """Strings returned by `const char *<funcname>(...)`.

    These are the labels fm160d publishes over ubus -- a dial step name, a
    profile verdict.  The front end renders them through _() at the point of
    display, which means no extractor can ever see them: at extraction time
    they do not exist as literals anywhere in the JavaScript.  They are the
    reason this checker derives a required msgid set from the C source instead
    of only trusting what it can extract from .js.
    """
    m = re.search(r'\nconst char \*' + re.escape(funcname) + r'\(', text)
    if not m:
        return None
    body = text[m.end():]
    end = body.find('\n}\n')
    body = body[:end if end >= 0 else len(body)]
    out = []
    for rm in re.finditer(r'return\s+((?:\s*"(?:[^"\\]|\\.)*"\s*)+);', body):
        out.append(''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', rm.group(1))))
    return out


# ---------------------------------------------------------------------------
# node
# ---------------------------------------------------------------------------


def node_hashes(node, cbi_js, strings):
    """Run the real cbi.js hash over `strings` and return {string: hash}.

    Returns (hashes, detail).  detail is non-None when node could not be used.
    """
    src = io.open(cbi_js, encoding='utf-8').read()
    parts = []
    for fn in ('u16', 's8', 'sfh', 'trimws'):
        body = extract_js_function(src, fn)
        if body is None:
            return None, 'cbi.js no longer defines %s()' % fn
        parts.append(body)

    driver = '\n'.join(parts) + '''
const fs = require('fs');
const input = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const out = {};
for (const s of input)
    out[s] = { raw: sfh(s), canon: sfh(trimws(s)) };
process.stdout.write(JSON.stringify(out));
'''

    tmp = tempfile.mkdtemp(prefix='fm160-i18n-')
    js = os.path.join(tmp, 'driver.js')
    inp = os.path.join(tmp, 'in.json')
    io.open(js, 'w', encoding='utf-8', newline='\n').write(driver)
    io.open(inp, 'w', encoding='utf-8', newline='\n').write(json.dumps(strings))
    try:
        p = subprocess.run([node, js, inp], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=120)
    except Exception as e:                                        # noqa: BLE001
        return None, 'node failed to run: %s' % e
    if p.returncode != 0:
        return None, 'node exited %d: %s' % (p.returncode,
                                             p.stderr.decode('utf-8', 'replace')[:400])
    return json.loads(p.stdout.decode('utf-8')), None


# ---------------------------------------------------------------------------
# the checks
# ---------------------------------------------------------------------------


def selftest_parser(rep):
    """Plant every construct parse_po claims to handle, and require it."""
    text = (
        'msgid ""\n'
        'msgstr ""\n'
        '"Plural-Forms: nplurals=1; plural=0;\\n"\n'
        '\n'
        '#: a.js\n'
        'msgid "plain"\n'
        'msgstr "\u5e73\u51e1"\n'
        '\n'
        '#: b.js\n'
        'msgid ""\n'
        '"a quote \\" here"\n'
        'msgstr ""\n'
        '"value \\" too"\n'
        '\n'
        '#: c.js\n'
        'msgid "vendor\'s manual"\n'
        'msgstr "vendor\'s"\n'
    )
    tmp = tempfile.mkdtemp(prefix='fm160-po-')
    p = os.path.join(tmp, 'self.po')
    io.open(p, 'w', encoding='utf-8', newline='\n').write(text)
    header, entries = parse_po(p)
    rep.check('selftest: po parser reads a header block',
              header is not None and 'Plural-Forms' in header, header)
    rep.check('selftest: po parser joins continuation lines',
              len(entries) == 3 and entries[1]['msgid'] == 'a quote " here',
              [e['msgid'] for e in entries])
    rep.check('selftest: po parser unescapes and keeps apostrophes literal',
              entries[2]['msgid'] == "vendor's manual", entries[2])
    rep.check('selftest: po parser records the source comment',
              entries[0]['source'] == 'a.js', entries[0]['source'])

    # And the negative: a construct the parser does not model must raise, not
    # be skipped, because skipping it loses a translation silently.
    bad = os.path.join(tmp, 'plural.po')
    io.open(bad, 'w', encoding='utf-8', newline='\n').write(
        'msgid "x"\nmsgid_plural "xs"\nmsgstr[0] "y"\n')
    try:
        parse_po(bad)
        rep.fail('selftest: plural forms are refused rather than skipped')
    except ValueError:
        rep.ok('selftest: plural forms are refused rather than skipped')

    # The positive side of the same construct, which the collision scan relies
    # on: upstream catalogues do use plurals, and their keys are decorated.
    # A plural key must NOT be the bare msgid, or the scan invents collisions --
    # and an ordinary key must not be empty, or the scan invents silence.
    _, pl = parse_po(bad, allow_plural=True)
    rep.check('selftest: a plural entry keeps its forms',
              len(pl) == 1 and pl[0]['id_plural'] == 'xs'
              and pl[0]['plural'] == {0: 'y'},
              pl)
    rep.check('selftest: a single-form plural entry keys on index 0',
              entry_keys(pl[0]) == ['x\x020'],
              entry_keys(pl[0]))

    # Deliberately run the key model over what parse_po RETURNS, not over dicts
    # written to match it.  A self-test fed a hand-made entry agrees with the
    # code even when the code disagrees with the parser -- which is precisely
    # how an `is not None` test on a parser-produced {} survived a green run
    # and made every message collide with nothing.
    keys_po = os.path.join(tmp, 'keys.po')
    io.open(keys_po, 'w', encoding='utf-8', newline='\n').write(
        'msgid "bare"\nmsgstr "b"\n'
        '\n'
        'msgctxt "ctx"\nmsgid "used"\nmsgstr "u"\n'
        '\n'
        'msgid "one"\nmsgid_plural "many"\nmsgstr[0] "m0"\nmsgstr[1] "m1"\n')
    _, ke = parse_po(keys_po, allow_plural=True)
    got_keys = dict((e['msgid'], entry_keys(e)) for e in ke)
    rep.check('selftest: a bare key is the msgid',
              got_keys.get('bare') == ['bare'], got_keys)
    rep.check('selftest: a contexted key is ctxt + \\x01 + msgid',
              got_keys.get('used') == ['ctx\x01used'], got_keys)
    rep.check('selftest: a plural key is msgid + \\x02 + index, not the msgid',
              got_keys.get('one') == ['one\x020', 'one\x021'], got_keys)
    rep.check('selftest: every entry yields at least one key',
              all(entry_keys(e) for e in ke),
              [(e['msgid'], entry_keys(e)) for e in ke])


def selftest_lmo(rep, lmo_path, id2hash, id2val):
    """Corrupt one key in a copy of a real .lmo and require it to be caught."""
    data = bytearray(io.open(lmo_path, 'rb').read())
    idx_off = struct.unpack('>I', bytes(data[-4:]))[0]
    if len(data) < idx_off + 16:
        rep.fail('selftest: lmo has no index to corrupt')
        return
    data[idx_off] ^= 0x01                    # flip a bit in the first key_id
    tmp = tempfile.mkdtemp(prefix='fm160-lmo-')
    p = os.path.join(tmp, 'broken.lmo')
    io.open(p, 'wb').write(bytes(data))
    try:
        _, entries = parse_lmo(p)
        problems = compare_lmo(entries, id2hash, id2val)
    except Exception as e:                                        # noqa: BLE001
        rep.fail('selftest: could not re-read the corrupted lmo', e)
        return
    rep.check('selftest: a flipped lmo key is detected', bool(problems),
              'compare_lmo reported nothing on a corrupted index')


def compare_lmo(entries, id2hash, id2val):
    """Return the list of ways `entries` disagrees with the .po.

    `id2val` is the po's msgstr bytes, which is also how the identity
    translations are recognised: po2lmo does not store an entry whose msgstr
    hashes the same as its msgid (po2lmo.c: `if (key_id != val_id)`), because
    both readers fall back to the source string anyway.  So those entries are
    required to be ABSENT, and everything else is required to be present and
    correct.
    """
    problems = []
    seen = {}
    for e in entries:
        if e['key_id'] == 0:
            continue
        seen.setdefault(e['key_id'], []).append(e)

    hashes = set(int(v, 16) for v in id2hash.values())

    for msgid, h in id2hash.items():
        want = int(h, 16)
        if want == 0:
            problems.append('msgid %r hashes to 0, which is the plural entry'
                            % msgid)
        elif id2val.get(msgid) == msgid.encode('utf-8'):
            if want in seen:
                problems.append('msgid %r is untranslated, so po2lmo should '
                                'have omitted it, but the .lmo carries it'
                                % msgid)
        elif want not in seen:
            problems.append('msgid %r (hash %s) is not in the .lmo index'
                            % (msgid, h))
    for h in seen:
        if h not in hashes:
            problems.append('the .lmo carries key %08x which no msgid produces' % h)
    for msgid, h in id2hash.items():
        for e in seen.get(int(h, 16), []):
            if e['value'] != id2val[msgid]:
                problems.append('msgid %r: .lmo holds %r but the po says %r'
                                % (msgid, e['value'], id2val[msgid]))
    return problems


# ---------------------------------------------------------------------------
# the Makefile's half of the contract
# ---------------------------------------------------------------------------

def declared_translations(mk):
    """The (alias, po directory) pairs FM160_TRANSLATIONS and FM160_PO define.

    Parsed, not substring-matched.  The check this replaced looked for the
    literal string 'FM160_TRANSLATIONS:=zh_Hans:zh-cn' in the Makefile, which is
    a *prefix* of a longer list -- so appending a language to that list left the
    check green while the entry it added named a po directory that is not there.
    The alias is the key and the po directory is a lookup beside it, so an alias
    whose FM160_PO is missing comes back as an empty directory rather than being
    silently absent from the list.  Returns None when the list is unparsable.
    """
    m = re.search(r'^FM160_TRANSLATIONS\s*:=\s*(.*?)\s*$', mk, re.M)
    if not m:
        return None
    po_of = dict(re.findall(r'^FM160_PO\.(\S+)\s*:=\s*(\S+)\s*$', mk, re.M))
    return [(a, po_of.get(a, '')) for a in m.group(1).split()]


def translation_gaps(mk, po_dir):
    """Where the declared translations and po/ disagree, in both directions.

    `missing` is the dangerous direction: an alias the Makefile declares whose
    po/<locale> is absent builds a translation package that installs cleanly and
    translates nothing -- and because the Makefile's own $(error) fires during
    the package scan, the scan stops there and records the application but not
    its translations, so the image ships with no .lmo at all.  `extra` is a po/
    directory that looks maintained and never reaches a device.
    """
    declared = declared_translations(mk)
    if declared is None:
        return None, None

    def pos_in(d):
        if not d:
            return []
        full = os.path.join(po_dir, d)
        if not os.path.isdir(full):
            return []
        return [f for f in os.listdir(full) if f.endswith('.po')]

    missing = [a for a, d in declared if not pos_in(d)]
    named = [d for _, d in declared if d]
    present = sorted(
        d for d in (os.listdir(po_dir) if os.path.isdir(po_dir) else [])
        if os.path.isdir(os.path.join(po_dir, d)) and d != 'templates')
    return missing, [d for d in present if d not in named]


def translation_call_is_split(mk):
    """Whether the $(call) that builds the translation package spans lines.

    True/False for the answer, None when there is no such call to look at --
    which is a different answer, and also a failure.

    $(call) does not trim its arguments, and a backslash-newline inside a call
    expands to a space.  A call written across lines therefore passes its
    arguments with a leading space, so the alias arrives as " zh-cn" and every
    name built from it inherits one: the package becomes
    "luci-i18n-fm160- zh-cn", which is a space in a Kconfig symbol and takes
    `make defconfig` down with it, and the installed file becomes
    "fm160. zh-cn.lmo", which load_catalog() never matches against its
    "*.zh-cn.lmo" glob.  The Makefile looks entirely normal either way.
    """
    m = re.search(r'\$\(call BuildFm160Translation,([^\n]*)', mk)
    if m is None:
        return None
    return not m.group(1).rstrip().endswith(')')


def gui_po_files(luci):
    """Every zh_Hans catalogue in a luci checkout, luci-base included.

    Deliberately not just luci-base.  load_catalog() merges EVERY *.zh-cn.lmo
    in the i18n directory into one namespace, so the question a context has to
    answer is "does any installed catalogue translate this bare msgid
    differently", and luci-base is one of ninety-odd.  Asking only luci-base
    gets the answer wrong in both directions -- it calls 'up' decoration when
    35 other catalogues carry it, and it never sees the collisions in 'raw',
    'on', 'Number', 'Age' or 'Altitude' at all.

    This over-approximates: a luci checkout carries apps this device does not
    have installed, so a context may be kept for a collision that would not
    happen.  That is the cheap direction to be wrong in -- an unnecessary
    context costs one distinct key, a missed collision is a reader resolving
    somebody else's translation into our page.
    """
    out = []
    for dp, dns, fns in os.walk(luci):
        dns[:] = [d for d in dns if d != '.git']
        here = dp.replace('\\', '/')
        if not here.endswith('po/zh_Hans'):
            continue
        if 'luci-app-fm160' in here:
            # Our own catalogue.  Sharing a msgid with itself proves nothing
            # and would make every context look justified.
            continue
        for f in sorted(fns):
            if f.endswith('.po'):
                out.append(os.path.join(dp, f))
    return sorted(out)


def bare_translations(entries):
    """msgid -> the set of translations for it, over entries that carry a key.

    Only bare (context-free, non-plural) entries: a contexted entry keys on
    ctxt \\x01 msgid, which is not the key a bare lookup in another app uses.
    An entry whose msgstr equals its msgid stores no key at all, because
    po2lmo never writes one, so it is evidence of nothing either.
    """
    out = {}
    for e in entries:
        if e['ctxt'] is not None or e['plural']:
            continue
        if e['msgstr'] == e['msgid']:
            continue
        out.setdefault(canon(e['msgid']), set()).add(e['msgstr'])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', default=None)
    ap.add_argument('--luci', default=None, help='a luci checkout, for cbi.js and the collision check')
    ap.add_argument('--po2lmo', default=None)
    ap.add_argument('--node', default=None)
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args()

    # Every path that reaches an OS call has to be in the OS's own form, and
    # this has to happen before anything is derived from them: the interpreter
    # here is a native Windows build even when the shell that launched it is
    # MSYS, so `/c/Users/x` resolves to `C:\c\Users\x` and fails.  --repo and
    # --luci are read with open(), the other two reach the loader through
    # subprocess, and all four are equally unable to resolve an MSYS path.
    args.repo = args.repo and native(args.repo)
    args.luci = args.luci and native(args.luci)
    args.po2lmo = args.po2lmo and native(args.po2lmo)
    args.node = args.node and native(args.node)

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, '..', '..'))
    repo = os.path.abspath(args.repo or root)
    pkg = os.path.join(repo, 'luci-app-fm160')
    res = os.path.join(pkg, 'htdocs', 'luci-static', 'resources')
    po_dir = os.path.join(pkg, 'po')

    rep = Report(args.quiet)
    print('fm160 i18n check')
    print('  repo %s' % repo)

    # -- what we ship ------------------------------------------------------
    pos = sorted(
        os.path.join(d, f)
        for d, _, fs in os.walk(po_dir) for f in fs if f.endswith('.po'))
    if not pos:
        rep.fail('a .po exists under %s' % po_dir)
        return 1
    for p in pos:
        print('  po   %s' % os.path.relpath(p, root))

    rep.section('parser self-test')
    selftest_parser(rep)

    # -- po shape ----------------------------------------------------------
    rep.section('po shape')
    po = pos[0]
    try:
        header, entries = parse_po(po)
    except ValueError as e:
        rep.fail('the .po parses', e)
        return 1
    rep.ok('the .po parses')
    rep.check('it has a header block', header is not None)
    rep.check('the header carries Plural-Forms',
              header and 'Plural-Forms:' in header)
    rep.check('it has message entries', len(entries) > 0, '%d entries' % len(entries))
    empty = [e['msgid'] for e in entries if not e['msgstr']]
    rep.check('every entry is translated', not empty, empty[:5])
    # A duplicated (msgid, ctxt) is worse than untidy: po2lmo emits two index
    # entries with the same key, and lmo_find_entry() binary-searches, so which
    # of the two answers is arbitrary.  The same msgid under two *different*
    # contexts is a different key and perfectly legal -- that is the whole
    # point of a context.
    pairs = [(e['msgid'], e['ctxt']) for e in entries]
    dupes = sorted(set(p for p in pairs if pairs.count(p) > 1))
    rep.check('no (msgid, context) pair appears twice', not dupes, dupes[:5])
    with_nl = [e['msgid'] for e in entries if '\n' in e['msgid']]
    rep.check('no message msgid spans lines', not with_nl, with_nl[:3])
    with_nl = [e['msgid'] for e in entries if '\n' in e['msgstr']]
    rep.check('no message msgstr spans lines', not with_nl, with_nl[:3])

    # -- canonical msgid ---------------------------------------------------
    # The load-bearing one: po2lmo hashes the msgid verbatim, both readers
    # collapse whitespace first, so an uncanonical msgid is written and looked
    # up under different hashes and never matches.  The context is hashed the
    # same way and needs the same discipline, so it is checked too.
    rep.section('canonical msgid (writer hashes verbatim, readers collapse)')
    bad = [(e['msgid'], canon(e['msgid'])) for e in entries if canon(e['msgid']) != e['msgid']]
    rep.check('every msgid is already whitespace-canonical', not bad,
              '\n'.join('%r would be looked up as %r' % b for b in bad[:6]))
    bad = [(e['ctxt'], canon(e['ctxt'])) for e in entries
           if e['ctxt'] is not None and canon(e['ctxt']) != e['ctxt']]
    rep.check('every context is already whitespace-canonical', not bad,
              '\n'.join('%r would be looked up as %r' % b for b in bad[:6]))

    # -- escaping ----------------------------------------------------------
    rep.section('escaping')
    leaked = [e['msgid'] for e in entries if "\\'" in e['msgid']]
    rep.check("no msgid carries a javascript \\' escape", not leaked, leaked[:5])
    ctxs = sorted(set(e['ctxt'] for e in entries if e['ctxt'] is not None))
    # Which contexts are justified is decided in the collision section, where
    # luci-base is available to say so.  Here the shapes only: empty, and a
    # context carrying the 0x01 separator itself, would both produce keys the
    # readers can never construct.
    rep.check('every context is non-empty and free of the separator',
              all(c and '\x01' not in c for c in ctxs), ctxs)
    print('       %d message(s) carry a context: %s'
          % (sum(1 for e in entries if e['ctxt'] is not None),
             ', '.join(repr(c) for c in ctxs) or 'none'))
    raw = io.open(po, encoding='utf-8').read()
    roundtrip = []
    for e in entries:
        if po_escape(e['msgid']) not in raw:
            roundtrip.append(e['msgid'])
    rep.check('every msgid round-trips through the po quoting', not roundtrip,
              roundtrip[:3])

    # -- extraction agreement ---------------------------------------------
    rep.section('extraction agreement (po <-> sources)')
    #
    # Every javascript file the package ships, with the number of distinct
    # (msgid, context) pairs each one is known to produce.  The floors earn
    # their place here: a pattern that quietly stopped matching one file would
    # show up as a smaller backlog, and a smaller backlog is exactly what a
    # translation nobody has written yet looks like.  With the floors in
    # place, the two set comparisons below are about the po rather than about
    # the extractor.
    #
    # The earlier M2 tranche read only dial.js and the M2 section of api.js.
    # It does not any more: the whole front end is translated, so the basis is
    # the whole front end.  A file that grows a new string is the one case
    # these floors cannot see, and that case is caught by the coverage section
    # at the end, which fails on any message the po does not carry.
    #
    # dial.js and usb.js used to be one file.  Splitting a page moves strings
    # between files without changing a single po key -- the po is keyed by
    # (msgid, context), not by file -- so the floors are the only thing here
    # that has to be told, and they are set from the real extraction, not by
    # splitting the old number.
    JS_SOURCES = [
        ('fm160/api.js',          68),
        ('view/fm160/cells.js',   73),
        ('view/fm160/debug.js',   21),
        ('view/fm160/dial.js',    70),
        ('view/fm160/gnss.js',   101),
        ('view/fm160/logs.js',    15),
        ('view/fm160/mwan.js',    44),
        ('view/fm160/overview.js', 72),
        ('view/fm160/signal.js',  21),
        ('view/fm160/sms.js',     97),
        ('view/fm160/usb.js',     57),
    ]
    menu_json = os.path.join(pkg, 'root', 'usr', 'share', 'luci', 'menu.d',
                             'luci-app-fm160.json')
    net_c = os.path.join(repo, 'fm160d', 'src', 'net.c')
    usb_c = os.path.join(repo, 'fm160d', 'src', 'usbmode.c')

    extracted = {}

    def ext(src, s, ctxt=None):
        extracted.setdefault((s, ctxt), []).append(src)

    for name, floor in JS_SOURCES:
        path = os.path.join(res, name)
        if not rep.check('%s is still there to extract from' % name,
                         os.path.isfile(path),
                         'not found under %s' % res):
            return 1
        calls = js_literal_calls(io.open(path, encoding='utf-8').read())
        if not rep.check('%s still yields its %d messages' % (name, floor),
                         len(set(calls)) >= floor,
                         'only %d distinct (msgid, context) pairs; either the '
                         'file lost strings or the extractor stopped seeing '
                         'them, and the po would then be compared against the '
                         'wrong set' % len(set(calls))):
            return 1
        for s, c in calls:
            ext(name, s, c)
    # A menu title is a plain string in the menu tree -- there is no field for
    # a context, so these are always bare.
    menu = json.load(io.open(menu_json, encoding='utf-8'))
    for v in menu.values():
        if 'title' in v:
            ext('menu.d', v['title'])

    net_text = io.open(net_c, encoding='utf-8').read()
    usb_text = io.open(usb_c, encoding='utf-8').read()
    step_labels = daemon_labels(net_text, 'fm160_net_step_name')
    verdict_labels = daemon_labels(usb_text, 'fm160_usbmode_verdict_text')
    if not rep.check('fm160d still publishes its step labels from net.c',
                     step_labels, 'fm160_net_step_name() not found or empty'):
        return 1
    if not rep.check('fm160d still publishes its verdict labels from usbmode.c',
                     verdict_labels, 'fm160_usbmode_verdict_text() not found or empty'):
        return 1
    for s in step_labels:
        ext('fm160d/net.c', s)
    for s in verdict_labels:
        ext('fm160d/usbmode.c', s)

    # The po's identity is the (msgid, context) PAIR, because that is what the
    # readers key on: matching a contexted call against a bare msgid would let
    # an entry that can never resolve pass as correspondence.  The bare msgids
    # are kept too, for the checks that are about the string itself.
    in_po_keys = set((e['msgid'], e['ctxt']) for e in entries)
    in_po = set(e['msgid'] for e in entries)
    missing = sorted(set(extracted) - in_po_keys)
    orphan = sorted(in_po_keys - set(extracted))

    def fmt(pair):
        s, c = pair
        return '%r (context %r)  (%s)' % (s, c, ', '.join(extracted.get(pair, [])))

    rep.check('every extracted string is in the po, under the same context',
              not missing, '\n'.join(fmt(p) for p in missing[:8]))
    rep.check('the po carries nothing that no source produces', not orphan,
              '\n'.join('%r (context %r)' % o for o in orphan[:8]))
    print('       extracted %d unique (string, context) pairs from %d sources'
          % (len(extracted),
             len(set(x for v in extracted.values() for x in v))))

    # A check that cannot fail is not a check: prove the comparison detects a
    # string the po is missing, using a copy of the real po.
    planted = 'EXTRA STRING NOT IN THE PO'
    cut = po + '.mut'
    io.open(cut, 'w', encoding='utf-8', newline='\n').write(
        raw + '\n#: selftest\nmsgid "%s"\nmsgstr "x"\n' % planted)
    _, e2 = parse_po(cut)
    added = set((x['msgid'], x['ctxt']) for x in e2) - in_po_keys
    rep.check('selftest: a planted extra msgid is seen as an orphan',
              added == {(planted, None)}, added)

    # -- hash agreement ----------------------------------------------------
    # What gets hashed is the KEY, not the msgid: for a contexted message the
    # readers hash ctxt + '\1' + msgid, so hashing the bare msgid would compare
    # a value nothing ever looks up.
    po_keys = sorted(set(entry_keys(e)[0] for e in entries))
    rep.section('hash agreement (real cbi.js)')
    cbi = os.path.join(args.luci, 'modules/luci-base/htdocs/luci-static/resources/cbi.js') \
        if args.luci else None
    node = args.node or 'node'
    if not cbi or not os.path.isfile(cbi):
        rep.skip('cbi.js hash comparison',
                 'no --luci checkout with cbi.js, so the real sfh() is unavailable')
    else:
        h, err = node_hashes(node, cbi, po_keys)
        if err:
            rep.skip('cbi.js hash comparison', err)
        else:
            nulls = [s for s, v in h.items() if not v['canon']]
            rep.check('the real sfh() hashes every lookup key', not nulls, nulls[:5])
            noncanon = [s for s, v in h.items() if v['raw'] != v['canon']]
            rep.check('hash(key) equals hash(trimws(key)) for every key',
                      not noncanon,
                      '\n'.join('%r: %s vs %s' % (s, h[s]['raw'], h[s]['canon'])
                                for s in noncanon[:6]))
            vals = [v['canon'] for v in h.values() if v['canon']]
            rep.check('the hashes are injective', len(set(vals)) == len(vals),
                      '%d hashes for %d strings' % (len(set(vals)), len(vals)))
            # The negative, which is the whole point: show that the canonical
            # check is load bearing by hashing a deliberately padded twin.
            twin = '__pad  me__'
            h2, err2 = node_hashes(node, cbi, [twin, canon(twin)])
            if err2:
                rep.skip('selftest: padded twin differs', err2)
            else:
                rep.check('selftest: a padded msgid hashes differently from its '
                          'canonical form',
                          h2[twin]['raw'] != h2[twin]['canon'],
                          'raw and canonical hashes agree, so the canonical '
                          'check would be pointless')

    # -- end to end --------------------------------------------------------
    rep.section('end to end (po2lmo -> .lmo -> index)')
    if not args.po2lmo or not os.path.isfile(args.po2lmo):
        rep.skip('po2lmo round trip',
                 'no --po2lmo binary; run tools/i18n/build-po2lmo.sh')
        rep.skip('lmo index comparison', 'no po2lmo, so there is no .lmo to read')
    else:
        out_dir = os.path.join(root, '_tmp', 'i18n')
        os.makedirs(out_dir, exist_ok=True)
        lmo = os.path.join(out_dir, 'fm160.zh-cn.lmo')
        p = subprocess.run([args.po2lmo, po, lmo], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
        if not rep.check('po2lmo accepts the .po', p.returncode == 0,
                         p.stderr.decode('utf-8', 'replace')[:400]):
            pass
        else:
            try:
                plural, lmo_entries = parse_lmo(lmo)
            except ValueError as e:
                rep.fail('the .lmo is internally consistent', e)
                lmo_entries = None
            if lmo_entries is not None:
                rep.ok('the .lmo is internally consistent (index is a whole '
                       'number of 16-byte records)')
                rep.check('the .lmo carries the po Plural-Forms',
                          plural is not None and plural.strip() ==
                          [l for l in (header or '').split('\n')
                           if l.startswith('Plural-Forms:')][0].split(':', 1)[1].strip(),
                          'lmo says %r' % plural)
                msgs = [e for e in lmo_entries if e['key_id'] != 0]
                # po2lmo skips an entry whose msgstr is byte-identical to its
                # msgid -- the readers fall back to the source string anyway, so
                # storing it would be redundant.  Those entries are expected to
                # be absent, and the count has to allow for them.  Note the
                # ctxt check: po2lmo compares hash(key) against hash(msgstr),
                # and a contexted entry's key is "ctxt\1msgid", so its key can
                # never equal its value even when the words are the same.
                identity = set(e['msgid'] for e in entries
                               if e['ctxt'] is None and e['msgstr'] == e['msgid'])
                rep.check('the index has one entry per message, less the '
                          'identity translations',
                          len(msgs) == len(entries) - len(identity),
                          '%d index entries, %d messages, %d identity '
                          'translations' % (len(msgs), len(entries), len(identity)))
                if identity:
                    print('       %d msgstr equal their msgid and are omitted '
                          'by design:' % len(identity))
                    for s in sorted(identity):
                        print('         %r' % s)
                rep.check('the index is sorted by key_id',
                          all(msgs[i]['key_id'] <= msgs[i + 1]['key_id']
                              for i in range(len(msgs) - 1)))
                rep.check('every entry declares one plural form',
                          all(e['val_id'] == 1 for e in msgs))

                # The hash the .lmo really carries, against the hash the real
                # cbi.js really computes.  If node was unavailable, the cbi.js
                # comparison was skipped and this has nothing to compare
                # against -- say so rather than quietly pass.
                if not args.luci or not os.path.isfile(cbi):
                    rep.skip('lmo key ids vs cbi.js hashes',
                             'needs the cbi.js comparison, which was skipped')
                else:
                    h, err = node_hashes(node, cbi, po_keys)
                    if err:
                        rep.skip('lmo key ids vs cbi.js hashes', err)
                    else:
                        # Both maps are keyed by the canonical lookup key, which
                        # for a contexted message is ctxt + '\1' + msgid -- the
                        # string the readers actually hash.
                        id2hash = {s: v['canon'] for s, v in h.items()}
                        id2val = {}
                        for e in entries:
                            id2val[canon(entry_keys(e)[0])] = e['msgstr'].encode('utf-8')
                        problems = compare_lmo(lmo_entries, id2hash, id2val)
                        rep.check('the .lmo key ids equal sfh(trimws(key)) and '
                                  'the stored bytes equal the po msgstr',
                                  not problems, '\n'.join(problems[:8]))
                        selftest_lmo(rep, lmo, id2hash, id2val)

    # -- collisions --------------------------------------------------------
    # load_catalog() merges EVERY *.zh-cn.lmo in /usr/lib/lua/luci/i18n into one
    # catalogue with no per-app separation, and the merge order is readdir, then
    # prepend (lmo.c:239-251).  So a key we share with luci-base is stored twice
    # and resolved by whichever archive the reader reaches first -- and that is
    # NOT the same archive for the two readers, because lmo_translate_ctxt
    # returns the first match (lmo.c:522) while window.TR is built by
    # lmo_iterate (lmo.c:607) with last-write-wins.  A shared key whose
    # translation differs is therefore a visible coin toss: the server-rendered
    # string and the browser-resolved string can disagree.
    #
    # luci-base is reported on by name because it is the catalogue that is
    # always installed, so a disagreement with it is the one a reader is
    # guaranteed to be able to hit.  The policy below is stricter than that
    # baseline and runs over every catalogue in the tree; see gui_po_files().
    rep.section('cross-catalogue collisions')
    if not args.luci:
        rep.skip('collision check against luci-base', 'no --luci checkout')
    else:
        base = os.path.join(args.luci, 'modules/luci-base/po/zh_Hans/base.po')
        if not os.path.isfile(base):
            rep.skip('collision check against luci-base', 'base.po not found at %s' % base)
        else:
            def catalogue(es):
                """key -> value, over the keys the readers really look up.

                Three things make this not a plain {msgid: msgstr}: the key is
                canonicalised (that is the shared lookup space), it carries
                po2lmo's context and plural decorations, and an entry whose
                translation is identical to its msgid claims no key at all
                because po2lmo never writes it.
                """
                out = {}
                for e in es:
                    vals = None
                    if e['plural']:
                        vals = [e['plural'][i] for i in sorted(e['plural'])]
                    if vals is not None:
                        if all(v == e['msgid'] for v in vals):
                            continue
                        val = vals
                    else:
                        if e['msgstr'] == e['msgid']:
                            continue
                        val = e['msgstr']
                    for k in entry_keys(e):
                        out.setdefault(canon(k), val)
                return out

            ours = catalogue(entries)
            _, base_entries = parse_po(base, allow_plural=True)
            theirs = catalogue(base_entries)
            shared = sorted(set(ours) & set(theirs))
            diff = [m for m in shared if ours[m] != theirs[m]]
            rep.ok('collision check ran (%d keys shared with luci-base, '
                   '%d translated differently)' % (len(shared), len(diff)))
            # The scan reports 0 when the key model is broken, which is
            # indistinguishable from having no collisions -- so require it to
            # find something.  Any app that reuses LuCI's vocabulary shares keys
            # with luci-base; if that ever stops being true, a human should look
            # rather than a zero be read as good news.
            rep.check('selftest: the collision scan can see a shared key at all',
                      len(shared) > 0,
                      'no shared key found, which is what a broken key model '
                      'also produces')
            # An upstream entry that is still untranslated is worse than a
            # disagreement: po2lmo does store it (its value is ""), so which of
            # the two the reader picks decides between our Chinese and nothing.
            blank = [m for m in diff if theirs[m] == '']
            if blank:
                rep.warn('luci-base shares these keys untranslated, so the '
                         'reader may resolve them to an empty string',
                         '\n'.join(repr(m) for m in blank[:10]))

            # The policy, both directions, so that a context is neither missing
            # where it is needed nor present where it is decoration.  `ours`
            # above is keyed with contexts; this asks what a BARE lookup would
            # find -- and "what it would find" is every catalogue the device
            # will merge, not the one that happens to be easiest to name.
            pool = {}
            cats = gui_po_files(args.luci)
            unreadable = []
            for p in cats:
                try:
                    _h, es = parse_po(p, allow_plural=True)
                except ValueError as ex:
                    unreadable.append('%s (%s)' % (os.path.relpath(p, args.luci), ex))
                    continue
                for k, vs in bare_translations(es).items():
                    pool.setdefault(k, set()).update(vs)
            rep.ok('collision pool: %d catalogues, %d distinct bare keys'
                   % (len(cats) - len(unreadable), len(pool)))
            if unreadable:
                # A catalogue this parser cannot model narrows the pool, and a
                # narrowed pool is how a real collision goes unnoticed.
                rep.warn('these catalogues could not be read, so the pool is '
                         'missing them', '\n'.join(unreadable[:8]))

            unescaped = []      # collides and differs, but carries no context
            decorative = []     # carries a context, but has nothing to escape
            for e in entries:
                vals = pool.get(canon(e['msgid']))
                if vals is None:
                    if e['ctxt'] is not None:
                        decorative.append((e['msgid'], e['ctxt']))
                    continue
                if e['ctxt'] is None and e['msgstr'] not in vals:
                    unescaped.append((e['msgid'], e['msgstr'], sorted(vals)))
            rep.check('every shared msgid that some luci catalogue translates '
                      'differently carries a context',
                      not unescaped,
                      '\n'.join('%r: ours %r, another catalogue has %r -- add a '
                                'second argument to _() and a msgctxt to the '
                                'po, or rename it if it is a menu title (a '
                                'title has no _() call to carry a context)'
                                % (u[0], u[1], u[2][:3]) for u in unescaped[:8]))
            rep.check('a context is only used where the bare msgid really '
                      'collides',
                      not decorative,
                      '\n'.join('%r (context %r) is not shared with any of the '
                                '%d catalogues, so the context escapes nothing'
                                % (d[0], d[1], len(cats) - len(unreadable))
                                for d in decorative[:8]))
            if diff:
                rep.warn('shared keys with a different translation in luci-base',
                         '\n'.join('%r: ours %r, theirs %r'
                                   % (m, ours[m], theirs[m]) for m in diff[:10]))

    # -- coverage ----------------------------------------------------------
    rep.section('coverage')
    all_sites = {}
    drifted = []
    for d, _, fs in os.walk(res):
        for f in fs:
            if not f.endswith('.js'):
                continue
            p = os.path.join(d, f)
            text = io.open(p, encoding='utf-8').read()
            got = js_literal_calls(text)
            if got:
                all_sites[os.path.relpath(p, res).replace('\\', '/')] = got
            # The extractor only understands _('single quoted').  A double
            # quoted or backticked call is a real call it cannot see, and every
            # number below would then look better than the code is.  There is
            # no such call today; the check is here so that adding one is loud
            # rather than silent.
            if JS_CALL_OTHER.search(strip_js_comments(text)):
                drifted.append(os.path.relpath(p, res).replace('\\', '/'))
    rep.check('every _() call uses the quote form the extractor matches',
              not drifted,
              'these files call _() with a quote JS_CALL does not match: %s'
              % ', '.join(drifted))

    for name in sorted(all_sites, key=lambda n: -len(set(all_sites[n]))):
        u = set(all_sites[name])
        print('       %-28s %3d/%3d' % (name, len(u & in_po_keys), len(u)))
    # The union, not the sum: the same string in three files is one po entry to
    # write, so summing per file would report a "backlog" three times over.
    seen = set()
    for v in all_sites.values():
        seen |= set(v)
    print('       %-28s %3d/%3d  (%.0f%%)  %d distinct messages'
          % ('TOTAL', len(seen & in_po_keys), len(seen),
             100.0 * len(seen & in_po_keys) / max(len(seen), 1), len(seen)))
    print('       %-28s %3d' % ('still untranslated', len(seen - in_po_keys)))

    # This is the check that keeps the whole front end translated rather than
    # the tranche that was translated once.  It fails on a message with no po
    # entry, which is the state a page is in the moment a new string is
    # written -- so the cost of forgetting is a red gate at the time of the
    # change, not a page that quietly stays English for a year.
    #
    # A string that genuinely must stay as it is (an AT command, an acronym
    # the UI means literally) satisfies this by carrying an identity msgstr in
    # the po.  po2lmo then omits it from the .lmo, which is the same thing the
    # readers would have done with it anyway -- so the entry costs nothing and
    # the invariant stays simple: every message the front end shows is a
    # message the po knows about.
    backlog = sorted(seen - in_po_keys)
    rep.check('every message the front end shows has a po entry',
              not backlog,
              '%d message(s) are shown untranslated, e.g. %s'
              % (len(backlog), '; '.join(repr(s) for s, _ in backlog[:6])))

    # Two scopes, deliberately.  `extracted` is the set the po must match
    # exactly, and it includes the labels fm160d publishes from net.c and
    # usbmode.c plus the menu titles, which no javascript extractor can see.
    # `all_sites` above walks htdocs/ only.  That is why the exactness
    # assertion lives in the extraction section and this one asserts only that
    # nothing the front end shows is missing: an entry the po carries for a
    # daemon label would be an orphan there and must not be one here.

    # -- makefile wiring ---------------------------------------------------
    rep.section('packaging')
    mk = io.open(os.path.join(pkg, 'Makefile'), encoding='utf-8').read()
    for what, needle in (
            ('the lmo directory is the one dispatcher.uc reads',
             '/usr/lib/lua/luci'),
            ('po2lmo is a build dependency', 'luci-base/host'),
            ('the recipe compiles the po it ships', 'po2lmo $(po)'),
            ('the recipe records the language for auto detection',
             'uci set luci.languages'),
    ):
        rep.check('Makefile: %s' % what, needle in mk,
                  'expected to find %r' % needle)

    # FM160_TRANSLATIONS is where the Makefile declares a translation; po/ is
    # where one actually exists.  The two have to agree, and the check that used
    # to live here -- `'FM160_TRANSLATIONS:=zh_Hans:zh-cn' in mk` -- could not
    # see them disagree, because that string is a *prefix* of a longer list.  A
    # language was added with no po/ directory to back it and the gate stayed
    # green; the Makefile's own guard then turned it into $(error), which is the
    # right outcome at the wrong moment.  It fires when make reaches
    # package/compile, hours into a build, and -- worse -- during the package
    # scan, which stops at the error: the scan records the application and not
    # its translations, so fixing the Makefile alone would still have shipped an
    # image whose LuCI has no Chinese in it at all.
    assignments = re.findall(r'^FM160_TRANSLATIONS\s*:=', mk, re.M)
    rep.check('Makefile: FM160_TRANSLATIONS is assigned exactly once',
              len(assignments) == 1,
              'found %d assignments; the last one silently wins' % len(assignments))

    declared = declared_translations(mk)
    rep.check('Makefile: the list parses and every alias has an FM160_PO',
              declared is not None and all(d for _, d in declared),
              'no FM160_TRANSLATIONS assignment, or an alias with no '
              'FM160_PO.<alias> beside it: %r' % (declared,))
    declared = declared or []

    missing, extra = translation_gaps(mk, po_dir)
    rep.check('Makefile: every declared translation has a po/ directory',
              missing is not None and not missing,
              'declared with nothing to compile: %s' % ', '.join(missing or []))
    rep.check('Makefile: every po/ directory belongs to a declared translation',
              extra is not None and not extra,
              'never packaged: %s' % ', '.join(extra or []))

    for alias, po_name in declared:
        # The recipe writes this straight into the luci.languages option, so an
        # empty one is a blank entry in the language picker rather than an error.
        rep.check('Makefile: FM160_LANG_TITLE.%s is defined' % alias,
                  ('FM160_LANG_TITLE.%s:=' % alias) in mk,
                  'the luci.languages label would be empty')
        # load_catalog() globs "<basename>.<alias>.lmo", so the alias is a
        # filename component, not a display name.
        rep.check('Makefile: the alias %r is a lower-case lmo suffix' % alias,
                  re.match(r'^[a-z]{2,3}(-[a-z0-9]+)+$', alias) is not None,
                  'fm160.%s.lmo would never be matched by "*.%s.lmo"' % (alias, alias))

    # The silent one.  A HIDDEN package with no DEFAULT is generated as a bare
    # "tristate" whose only default is "y if DEFAULT_<itself>", a symbol nothing
    # defines, so no configuration can ever select it: it compiles for no image,
    # and on the device that is indistinguishable from a translation nobody
    # wrote.  luci.mk sets DEFAULT:=LUCI_LANG_<po directory>||(ALL&&m) on every
    # translation package it builds.  This template has to resolve the same
    # symbol, through FM160_PO so that the alias and the locale directory it
    # names cannot drift apart.
    resolved = ', '.join('luci-i18n-fm160-%s <- LUCI_LANG_%s||(ALL&&m)' % (a, d)
                         for a, d in declared if d)
    default_field = 'DEFAULT:=LUCI_LANG_$(FM160_PO.$(1))||(ALL&&m)'
    rep.check('Makefile: the template sets DEFAULT from the po directory',
              default_field in mk,
              'without it the generated symbol defaults only to "y if '
              'DEFAULT_<itself>", which nothing defines, so nothing can select '
              'the translation package and the image ships without the .lmo '
              '(%s)' % (resolved or 'no translation declared'))

    without = mk.replace(default_field, '')
    rep.check('selftest: dropping the DEFAULT field is detected',
              without != mk and default_field not in without,
              'the mutation did not apply, so the check above proves nothing')

    # The alias reaches $(1) of the template through a $(call), which does not
    # trim: a backslash-newline inside one expands to a space, so a call split
    # across lines passes " zh-cn" and every name built from it inherits the
    # space.  Assert the call is written on one line -- see the helper for what
    # the space breaks.
    split = translation_call_is_split(mk)
    rep.check('Makefile: the translation call is written on one line',
              split is False,
              'no such $(call) found' if split is None else
              'a backslash-newline inside $(call) becomes a space that $(call) '
              'does not trim, so the alias arrives as " zh-cn" and names the '
              'package "luci-i18n-fm160- zh-cn", which is not valid Kconfig')

    # Each of these checks is only worth having if it can see the failure it was
    # written for, so hand each one a Makefile that differs from this one by a
    # single line.
    planted = re.sub(r'^FM160_TRANSLATIONS\s*:=\s*.*$',
                     'FM160_TRANSLATIONS:=zz-zz', mk, count=1, flags=re.M)
    rep.check('selftest: the Makefile mutation for the gap check applied',
              planted != mk, 'no FM160_TRANSLATIONS line to mutate')
    p_missing, p_extra = translation_gaps(planted, po_dir)
    rep.check('selftest: a declared translation with no po/ directory is caught',
              p_missing == ['zz-zz'], 'saw missing=%r' % (p_missing,))
    here = sorted(
        d for d in (os.listdir(po_dir) if os.path.isdir(po_dir) else [])
        if os.path.isdir(os.path.join(po_dir, d)) and d != 'templates')
    rep.check('selftest: a po/ directory no entry names is caught',
              bool(here) and sorted(p_extra or []) == here,
              'saw extra=%r, expected %r' % (p_extra, here))

    # And the whitespace trap, which is invisible by construction: rewrap the
    # real call onto two lines, the way it was written before, and require the
    # check to notice.  Nothing about the file looks different afterwards.
    opens = [l for l in mk.splitlines() if '$(call BuildFm160Translation,' in l]
    broke = mk.replace(opens[0], opens[0].replace(
        '$(call BuildFm160Translation,',
        '$(call BuildFm160Translation,\\\n      '), 1) if opens else mk
    rep.check('selftest: a call split across lines is caught',
              bool(opens) and translation_call_is_split(broke) is True,
              'the predicate cannot see the split it exists to catch')

    # -- summary -----------------------------------------------------------
    print('\n%s: %d passed, %d failed, %d skipped, %d warnings'
          % (os.path.basename(__file__), rep.passed, rep.failed,
             len(rep.skipped), len(rep.warnings)))
    if rep.skipped:
        for what, why in rep.skipped:
            print('  skipped: %s -- %s' % (what, why))
    return 1 if rep.failed else 0


if __name__ == '__main__':
    sys.exit(main())
