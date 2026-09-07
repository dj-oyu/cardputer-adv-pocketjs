"""Read background labels from the firmware's source of truth for USB tests.

The labels used to be a `names[]` array of their own. They are now the first
field of each scene_ops_t row in shell.c's SCENES[] table, which is what makes
adding a background one edit instead of two -- see main/scene/scene.h. The USB
tests count key presses against this list, so it has to come from the table the
firmware actually draws rather than from a copy kept here.
"""
from pathlib import Path
import re

source = (Path(__file__).resolve().parents[1] / 'main/ui/shell.c').read_text(encoding='utf-8')

match = re.search(r'SCENES\[\]\s*=\s*\{(.*?)\n\};', source, re.S)
if not match:
    raise RuntimeError('Scene table not found in main/ui/shell.c')
body = match.group(1)
# Comments inside the table may contain quotes and braces of their own; a row's
# name is the first string literal after the brace that opens the row.
body = re.sub(r'//[^\n]*', '', body)
body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
BACKGROUNDS = tuple(re.findall(r'\{\s*"([^"]*)"', body))
if not BACKGROUNDS:
    raise RuntimeError('Empty scene table')
if len(BACKGROUNDS) != body.count('{'):
    raise RuntimeError(
        f'{body.count("{")} scene rows but {len(BACKGROUNDS)} names: a row whose '
        'first field is not its name would silently shift every key count')

if __name__ == '__main__':
    for i, name in enumerate(BACKGROUNDS):
        print(i, name)
