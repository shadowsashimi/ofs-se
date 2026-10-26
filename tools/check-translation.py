"""Checks a translation file against the English it translates.

    python tools/check-translation.py data/lang/ja.csv

Every keyed row of localization/localization.csv and every #TEXT row of
localization/text.csv should be there, with a translation, its English
unchanged (the English is what #TEXT rows are looked up by), and the same
format specifiers in the same order, so a %d can't turn into a crash.
"""
import csv
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC = re.compile(r'%%|%[-+0#]*\d*(?:\.\d+)?[diouxXfFeEgGcsp]')


def rows(path):
    with open(path, encoding='utf-8', newline='') as f:
        return list(csv.reader(f))[1:]


def main(path):
    keyed = {r[0]: r[1] for r in rows(os.path.join(ROOT, 'localization', 'localization.csv')) if len(r) >= 2}
    texts = {r[1] for r in rows(os.path.join(ROOT, 'localization', 'text.csv')) if len(r) >= 2}
    problems = []
    got_keyed, got_texts = {}, {}
    for n, r in enumerate(rows(path), start=2):
        if len(r) != 3:
            problems.append(f'line {n}: {len(r)} columns')
            continue
        key, english, translation = r
        if key == '#TEXT':
            got_texts[english] = translation
            if english not in texts:
                problems.append(f'line {n}: #TEXT not in the code: {english[:60]!r}')
        else:
            got_keyed[key] = translation
            if key not in keyed:
                problems.append(f'line {n}: unknown key {key}')
            elif keyed[key] != english:
                problems.append(f'line {n}: {key} English differs')
        if key != 'INVALID_TR':
            if not translation.strip():
                problems.append(f'line {n}: empty translation for {english[:60]!r}')
            elif SPEC.findall(english) != SPEC.findall(translation):
                problems.append(f'line {n}: format {SPEC.findall(english)} vs {SPEC.findall(translation)} in {english[:50]!r}')
            if english.count('\n') and not translation.count('\n'):
                problems.append(f'line {n}: line breaks lost in {english[:50]!r}')
    for key in keyed:
        if key not in got_keyed:
            problems.append(f'missing key {key}')
    for text in texts:
        if text not in got_texts:
            problems.append(f'missing text {text[:60]!r}')
    print(f'{path}: {len(got_keyed)} keyed, {len(got_texts)} texts, {len(problems)} problems')
    for p in problems[:40]:
        print('  ' + p)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
