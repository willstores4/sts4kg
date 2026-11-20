#!/usr/bin/env python3
"""Pack bundled JPEGs into two files to avoid large-file-count PFS builder issues."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
assets=ROOT/'assets'
rows=[]
with (assets/'covers.pack.tmp').open('wb') as out:
    for path in sorted((assets/'covers').glob('*.jpg')):
        if not path.stem.isdigit():continue
        data=path.read_bytes();rows.append(f'{path.stem}\t{out.tell()}\t{len(data)}\n');out.write(data)
(assets/'covers.pack.tmp').replace(assets/'covers.pack')
(assets/'covers.idx.tmp').write_text(''.join(rows));(assets/'covers.idx.tmp').replace(assets/'covers.idx')
print(f'Packed {len(rows)} original covers; {(assets/"covers.pack").stat().st_size:,} bytes.')
