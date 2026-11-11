#!/usr/bin/env python3
"""Reject incomplete catalogs and translations that break runtime placeholders or HTML."""
from collections import Counter
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

catalog = Path(__file__).resolve().parents[1] / 'gui/translations/chiaki_zh_CN.ts'
root = ET.parse(catalog).getroot()
errors = []
count = 0
for context in root.findall('context'):
    for message in context.findall('message'):
        source = message.findtext('source') or ''
        translation = message.find('translation')
        if translation is not None and translation.get('type') in ('obsolete', 'vanished'):
            continue
        count += 1
        target = ''.join(translation.itertext()) if translation is not None else ''
        label = f'{context.findtext("name")}: {source[:80]}'
        if translation is None or translation.get('type') == 'unfinished' or (source and not target):
            errors.append(f'Unfinished translation: {label}')
            continue
        for pattern, kind in [(r'%L?\d+|%n', 'placeholder'), (r'</?([a-zA-Z][\w-]*)\b[^>]*>', 'HTML tag')]:
            if Counter(re.findall(pattern, source)) != Counter(re.findall(pattern, target)):
                errors.append(f'{kind} mismatch: {label}')
if root.get('language') != 'zh_CN' or count == 0:
    errors.append('Expected a nonempty Simplified Chinese catalog')
if errors:
    print('\n'.join(errors), file=sys.stderr)
    sys.exit(1)
print(f'Chinese catalog: {count} messages; placeholders and HTML preserved; no unfinished entries')
