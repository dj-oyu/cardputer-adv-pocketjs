"""Fail the build before firmware consumes the reserved SKK/font partitions."""
import sys
import csv
from pathlib import Path

def check(path, partitions=None):
    partitions = Path(partitions or Path(__file__).resolve().parents[1] / 'partitions.csv')
    entries = {}
    end = 0x9000
    for row in csv.reader(partitions.read_text().splitlines()):
        if not row or row[0].lstrip().startswith('#'):
            continue
        name, kind, subtype, offset, length = [s.strip() for s in row[:5]]
        offset, length = int(offset, 0), int(length, 0)
        if offset < end or offset % 4096 or length <= 0 or length % 4096:
            raise SystemExit(f'Invalid/overlapping partition: {name}')
        end = offset + length
        entries[name] = (offset, length)
    if end > 0x800000:
        raise SystemExit('Partitions exceed physical 8 MiB Flash')
    for name in ('skk_dict', 'jp_font'):
        if entries.get(name, (0, 0))[1] < 0x200000:
            raise SystemExit(f'{name} must reserve at least 2 MiB')
    size = Path(path).stat().st_size
    budget = entries['factory'][1]
    if budget > 0x300000:
        raise SystemExit('Factory budget must not exceed 3 MiB')
    if size > budget:
        raise SystemExit(f'Firmware {size} bytes exceeds factory budget {budget}')
    print(f'FLASH_BUDGET app={size} limit={budget} spare={budget-size} '
          f'skk_reserved={entries["skk_dict"][1]} font_reserved={entries["jp_font"][1]} '
          f'storage={entries["storage"][1]}')

if __name__ == '__main__':
    check(sys.argv[1])
