#!/usr/bin/env python3
"""Report bracket balance in JavaScript/LuCI view files.

LuCI's front-end is loaded, not compiled: a file with an unbalanced bracket is
not rejected by any build step, it just fails at page load.  The failure is also
reported somewhere other than where the mistake is -- an extra ')' four hundred
lines down shows up as an error at the closing brace of an unrelated block.

So this walks the source with a small lexer instead of counting characters:
comments, string literals and template literals are skipped, and a regex
literal is recognised from the token that precedes it.  The first position where
a bracket closes the wrong thing is reported with its line, column and a short
excerpt, which is the line that actually needs editing.

Exit status is 0 when every file balances.
"""

import sys

OPEN = {'(': ')', '[': ']', '{': '}'}
CLOSE = {v: k for k, v in OPEN.items()}

# A '/' starts a regex literal only where an operand cannot appear: after an
# operator, an opening bracket, a comma or a semicolon.  After an identifier,
# number, ')' or ']' it is division -- getting this backwards turns
# `(n / 1024)` into a fake regex that swallows the rest of the line and reports
# nonsense.  Keyword forms ('return /re/') are handled separately.
REGEX_PREV = set('([{,;=:!&|?+-*%~^<>')

# Keywords after which a '/' begins a regex rather than a division.
REGEX_PREV_WORDS = set([
    'return', 'typeof', 'instanceof', 'in', 'of', 'new', 'delete', 'void',
    'case', 'do', 'else', 'yield', 'await', 'throw',
])

WORD_CHARS = set('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$')


def scan(src):
    """Yield (line, col, char) for every character that is real code."""
    i, n = 0, len(src)
    line, col = 1, 1
    prev = ''          # last significant code character seen
    word = ''          # identifier/keyword being read, or '' outside one

    while i < n:
        ch = src[i]

        if ch == '\n':
            line += 1
            col = 1
            i += 1
            prev = '\n'
            word = ''
            continue

        # line comment
        if ch == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                i += 1
            word = ''
            continue

        # block comment
        if ch == '/' and i + 1 < n and src[i + 1] == '*':
            i += 2
            col += 2
            while i < n and not (src[i] == '*' and i + 1 < n and src[i + 1] == '/'):
                if src[i] == '\n':
                    line += 1
                    col = 1
                else:
                    col += 1
                i += 1
            i += 2
            col += 2
            continue

        # string literal
        if ch in '\'"':
            quote = ch
            i += 1
            col += 1
            while i < n and src[i] != quote:
                if src[i] == '\\':
                    i += 2
                    col += 2
                    continue
                if src[i] == '\n':
                    line += 1
                    col = 1
                else:
                    col += 1
                i += 1
            i += 1
            col += 1
            prev = 'x'          # a string literal behaves like an operand
            word = ''
            continue

        # template literal -- the braces inside ${} are balanced on their own,
        # so skipping the whole literal leaves the bracket count correct.
        if ch == '`':
            i += 1
            col += 1
            while i < n and src[i] != '`':
                if src[i] == '\\':
                    i += 2
                    col += 2
                    continue
                if src[i] == '\n':
                    line += 1
                    col = 1
                else:
                    col += 1
                i += 1
            i += 1
            col += 1
            prev = 'x'
            word = ''
            continue

        # regex literal
        if ch == '/' and (prev == '' or prev == '\n' or prev in REGEX_PREV
                          or (word in REGEX_PREV_WORDS)):
            i += 1
            col += 1
            in_class = False
            while i < n:
                c = src[i]
                if c == '\\':
                    i += 2
                    col += 2
                    continue
                if c == '[':
                    in_class = True
                elif c == ']':
                    in_class = False
                elif c == '/' and not in_class:
                    break
                if c == '\n':
                    line += 1
                    col = 1
                else:
                    col += 1
                i += 1
            i += 1
            col += 1
            while i < n and src[i].isalpha():
                i += 1
                col += 1
            prev = 'x'
            word = ''
            continue

        yield line, col, ch

        if ch in WORD_CHARS:
            word += ch
        elif not ch.isspace():
            word = ''
        if not ch.isspace():
            prev = ch
        i += 1
        col += 1


def check(path):
    with open(path, 'r', encoding='utf-8', newline='') as fh:
        src = fh.read()

    lines = src.splitlines()
    stack = []          # (char, line, col)

    for line, col, ch in scan(src):
        if ch in OPEN:
            stack.append((ch, line, col))
        elif ch in CLOSE:
            if not stack:
                return ('stray %r at line %d col %d' % (ch, line, col),
                        excerpt(lines, line))
            want = OPEN[stack[-1][0]]
            if want != ch:
                open_ch, ol, oc = stack[-1]
                return ('%r at line %d col %d closes %r opened at line %d col %d'
                        % (ch, line, col, open_ch, ol, oc),
                        excerpt(lines, line))
            stack.pop()

    if stack:
        open_ch, ol, oc = stack[-1]
        return ('unclosed %r opened at line %d col %d (%d still open)'
                % (open_ch, ol, oc, len(stack)), excerpt(lines, ol))
    return None, None


def excerpt(lines, line, width=72):
    if line < 1 or line > len(lines):
        return ''
    text = lines[line - 1].replace('\t', '    ')
    if len(text) > width:
        text = text[:width] + '...'
    return text


def main(argv):
    if len(argv) < 2:
        print('usage: balance.py <file.js> [file.js ...]', file=sys.stderr)
        return 2

    bad = 0
    for path in argv[1:]:
        try:
            problem, text = check(path)
        except (OSError, UnicodeDecodeError) as exc:
            print('  ERROR %s: %s' % (path, exc))
            bad += 1
            continue

        if problem:
            print('  FAIL  %s' % path)
            print('        %s' % problem)
            if text:
                print('        | %s' % text)
            bad += 1
        else:
            print('  ok    %s' % path)

    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
