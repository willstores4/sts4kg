#!/usr/bin/env python3
"""Paced public listing crawl using the same parsers as the console app."""
import argparse,concurrent.futures,datetime,hashlib,io,json,re,subprocess,tempfile,threading,time,urllib.request
from pathlib import Path
from urllib.parse import urlparse
from PIL import Image
ROOT=Path(__file__).resolve().parents[1];ASSETS=ROOT/'assets'
ORIGINS={1:'https://ps4torrent.net',2:'https://gametorrent.org',3:'https://love-games1.net'}
NAMES={1:'PS4 Torrent',2:'GameTorrent',3:'Love Games'}
def listing(source,page):
    if source==1:return ORIGINS[1]+'/' if page==1 else ORIGINS[1]+f'/page/{page}/'
    if source==2:return ORIGINS[2]+'/category/playstation-4/'+('' if page==1 else f'page/{page}/')
    return ORIGINS[3]+'/load/igry_dlja_konsolej/igry_dlja_playstation_4/42'+('' if page==1 else f'-{page}')
def fetch(url):
    for attempt in range(3):
        try:
            with urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':'Portal-Catalog/1.4','Accept-Encoding':'identity'}),timeout=25) as r:
                if urlparse(r.url).hostname not in {urlparse(v).hostname for v in ORIGINS.values()}:raise ValueError('Unexpected source redirect')
                return r.read(8*1024*1024)
        except Exception:
            if attempt==2:raise
            time.sleep(2+attempt)
def crawl(source):
    records={};last=2 if source==1 else 1;page=1
    while page<=last:
        data=fetch(listing(source,page))
        with tempfile.NamedTemporaryFile(suffix='.html') as f:
            f.write(data);f.flush();parsed=subprocess.run([str(ROOT/'build/catalog_parser'),str(source),f.name],capture_output=True,text=True,timeout=10)
        if parsed.returncode:raise RuntimeError(f'{NAMES[source]} page {page}: no game cards; previous source snapshot retained')
        count,_,rows=parsed.stdout.partition('\n');last=max(last,int(count));
        if last>500:raise RuntimeError('Pagination safety bound exceeded')
        for row in rows.splitlines():
            parts=row.split('\t')
            if len(parts)==6:records.setdefault(parts[0],row)
        if page==1:
            (ROOT/f'tests/fixtures/source{source}-listing.html').write_bytes(data)
        if page==1 or page%10==0 or page==last:print(f'{NAMES[source]} {page}/{last}: {len(records)} entries',flush=True)
        page+=1;time.sleep(.35)
    path=ASSETS/f'catalog-source-{source}.tsv';temp=path.with_suffix('.tmp');temp.write_text('\n'.join(records.values())+'\n');temp.replace(path)
    return source,{'name':NAMES[source],'pages':last,'entries':len(records),'updated':datetime.datetime.now(datetime.timezone.utc).isoformat()}
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--all',action='store_true');ap.add_argument('--sources',type=int,nargs='+',choices=[1,2,3]);ap.add_argument('--covers-only',action='store_true');ap.add_argument('--no-covers',action='store_true');args=ap.parse_args()
    base=[row for row in (ASSETS/'catalog.tsv').read_text().splitlines() if '\thttps://ps4torrent.net/' in row]
    if base and not (ASSETS/'catalog-source-1.tsv').exists():(ASSETS/'catalog-source-1.tsv').write_text('\n'.join(base)+'\n')
    stats=json.loads((ASSETS/'catalog_sources.json').read_text()) if (ASSETS/'catalog_sources.json').exists() else {};sources=[] if args.covers_only else args.sources or ([1,2,3] if args.all else [2,3])
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        futures={pool.submit(crawl,s):s for s in sources}
        for future,source in futures.items():
            try:s,info=future.result();stats[str(s)]=info
            except Exception as e:print(f'SOURCE FAILED ({NAMES[source]}): {e}',flush=True)
    rows=[]
    for source in (1,2,3):
        path=ASSETS/f'catalog-source-{source}.tsv'
        if path.exists():rows.extend(path.read_text().splitlines())
    (ASSETS/'catalog.tsv.tmp').write_text('\n'.join(rows)+'\n');(ASSETS/'catalog.tsv.tmp').replace(ASSETS/'catalog.tsv')
    (ASSETS/'catalog_sources.json').write_text(json.dumps(stats,ensure_ascii=False,indent=2))
    if args.no_covers:return
    gates={urlparse(url).hostname:threading.Semaphore(2) for url in ORIGINS.values()}
    def cover(row):
        fields=row.split('\t');path=ASSETS/'covers'/(fields[0]+'.jpg')
        if path.exists():return True
        try:
            with gates[urlparse(fields[5]).hostname]:
                image=Image.open(io.BytesIO(fetch(fields[5]))).convert('RGB');image.thumbnail((300,400));image.save(path,'JPEG',quality=84);time.sleep(.1)
            return True
        except Exception as e:print(f'Cover {fields[0]} unavailable: {e}',flush=True);return False
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        available=0
        for i,ok in enumerate(pool.map(cover,rows),1):
            available+=ok
            if i%250==0:print(f'Artwork {i}/{len(rows)} checked',flush=True)
    print(f'Complete: {len(rows)} listings, {available} covers',flush=True)
if __name__=='__main__':main()
