"""Catalog checks independent of the runtime lookup: duplicate keys,
printf argument signatures and availability of every translated Cyrillic glyph."""
import ast
import pathlib
import re

root = pathlib.Path(__file__).resolve().parents[3]
catalog = root / 'main/l10n_ru.inc'
pairs = []
for line in catalog.read_text(encoding='utf-8').splitlines():
    if line.startswith('T('):
        pairs.append(ast.literal_eval('(' + line[2:-1] + ')'))
keys = [en for en, _ in pairs]
assert len(keys) == len(set(keys)), 'duplicate translation key'
fmt = re.compile(r'%(?:[-+ #0]*)(?:\d+|\*)?(?:\.(?:\d+|\*))?([hljztL]*)([diouxXfFeEgGaAcspn%])')
for en, ru in pairs:
    assert fmt.findall(en) == fmt.findall(ru), (en, ru, 'format arguments differ')
    assert '\x00' not in ru
for size in (8, 14, 20, 28, 48):
    text = (root / f'main/font_cyrillic_{size}.c').read_text(encoding='utf-8')
    glyphs = {int(x, 16) for x in re.findall(r'U\+([0-9A-Fa-f]+)', text)}
    for _, ru in pairs:
        for c in ru:
            if 0x400 <= ord(c) <= 0x45f or 0x2010 <= ord(c) <= 0x2026:
                assert ord(c) in glyphs, (size, c, 'missing font glyph')
print(f'PASS: {len(pairs)} unique translations, format signatures, Cyrillic glyphs in 5 sizes')
