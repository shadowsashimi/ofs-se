"""Lists the text in the code that is translated by its English.

Writes localization/text.csv: one #TEXT row for each English string passed to
TRS() or to a helper that translates its text (tooltips, segmented controls,
steppers, toggle buttons, headings), with an empty translation column. A
translation fills that column in and adds the rows to its file. Run it again
after adding text, and compare.

    python tools/extract-text.py
"""
import csv
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Calls whose first string argument is translated, and arrays of labels and
# tips handed to SegmentedControl.
CALLS = r'(?:TRS|OFS::Tooltip|OFS::ShowWrappedTooltip|OFS::TooltipFmt|OFS::StepperFloat|OFS::StepperInt|' \
        r'OFS::ToggleButton|OFS::SeparatorText|OFS::SameLineIfCheckboxFits|sectionHeader|groupHeader|group|' \
        r'statisticsHeading)\('
ARRAYS = r'(?:labels|tips|Tips|Labels|[A-Za-z]*Tips|[A-Za-z]*Labels|[A-Za-z]*Names|InsetViews)\s*\[\s*[A-Za-z0-9_:]*\s*\]\s*=\s*\{'
# Window titles kept in constants, "Title###ID", which are translated where
# the window is begun.
TITLES = re.compile(r'const char\*\s+\w+\s*=\s*"([^"#]+)###')
LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')
FORMAT = re.compile(r'%[-+ 0#]*\d*(?:\.\d+)?[a-zA-Z]')


def literals_from(text, start):
    """The adjacent string literals from start, joined, and where they end."""
    out = ''
    i = start
    found = False
    while True:
        while i < len(text) and text[i] in ' \t\r\n':
            i += 1
        m = LITERAL.match(text, i)
        if not m:
            break
        out += m.group(1)
        i = m.end()
        found = True
    return (out if found else None), i


def unescape(s):
    return s.encode('utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')


def wanted(s):
    visible = s.split('##')[0]
    return re.search(r'[A-Za-z]{2,}', FORMAT.sub('', visible)) is not None


texts = set()
files = []
for pattern in ('src/**/*.cpp', 'src/**/*.h', 'OFS-lib/UI/*.cpp', 'OFS-lib/UI/*.h', 'OFS-lib/*.cpp'):
    files += glob.glob(os.path.join(ROOT, pattern), recursive=True)
for path in sorted(set(files)):
    text = open(path, encoding='utf-8').read()
    for m in re.finditer(CALLS, text):
        # Every string in the call, to its closing bracket: a heading's
        # description and a group's tooltip come after its title.
        i, depth = m.end(), 1
        while i < len(text) and depth > 0:
            if text[i] == '"':
                value, i = literals_from(text, i)
                if value is not None:
                    texts.add(value)
                continue
            depth += text[i] == '('
            depth -= text[i] == ')'
            i += 1
    for m in TITLES.finditer(text):
        texts.add(m.group(1))
    for m in re.finditer(ARRAYS, text):
        i = m.end()
        # To the end of the array, past any braces of structs in it.
        end = text.index('};', i)
        while i < end:
            value, j = literals_from(text, i)
            if value is not None:
                texts.add(value)
                i = j
            else:
                i += 1

rows = sorted(t for t in (unescape(t).split('##')[0] for t in texts) if wanted(t))
os.makedirs(os.path.join(ROOT, 'localization'), exist_ok=True)
out = os.path.join(ROOT, 'localization', 'text.csv')
with open(out, 'w', encoding='utf-8', newline='') as f:
    w = csv.writer(f, lineterminator='\n')
    w.writerow(['Key (do not touch)', 'Default', 'Translation'])
    for t in dict.fromkeys(rows):
        w.writerow(['#TEXT', t, ''])
print(f'{len(set(rows))} texts written to {out}')
