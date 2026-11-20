#!/usr/bin/env python3
"""Rate-limited catalog and cover snapshot. Only fetches public listing pages/artwork."""
import argparse, concurrent.futures, datetime, html, io, json, re, time, urllib.request
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urljoin, urlparse
BASE = 'https://ps4torrent.net/'
ROOT = Path(__file__).resolve().parents[1]
class Cards(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True); self.games=[]; self.current=None; self.field=None; self.pages=set()
    def handle_starttag(self, tag, attrs):
        a=dict(attrs); classes=a.get('class','').split()
        if tag=='a':
            u=urljoin(BASE,a.get('href',''))
            if urlparse(u).netloc!='ps4torrent.net': return
            m=re.search(r'/page/(\d+)/',u)
            if m: self.pages.add(int(m[1]))
            if 'item' in classes and re.search(r'/\d+-[^/]+\.html$',u):
                self.current={'id':int(re.search(r'/(\d+)-',u)[1]),'title':'','year':'','size':'','url':u,'image':''}; self.field=None
        if self.current:
            if tag=='img': self.current['image']=urljoin(BASE,a.get('src',''))
            if 'item__title' in classes: self.field='title'
            if tag=='span': self.field='year' if not self.current['year'] else 'size'
    def handle_data(self,data):
        if self.current and self.field: self.current[self.field]+=data.strip()
    def handle_endtag(self,tag):
        if tag in ('div','span'): self.field=None
        if tag=='a' and self.current:
            if self.current['title'] and self.current['image']: self.games.append(self.current)
            self.current=None

def fetch(url):
    if urlparse(url).hostname!='ps4torrent.net': raise ValueError('Unexpected source host')
    for attempt in range(3):
        try:
            req=urllib.request.Request(url,headers={'User-Agent':'PS4-Portal-Catalog/1.0 (authorized catalog browser)','Accept-Encoding':'identity'})
            with urllib.request.urlopen(req,timeout=25) as r:
                if urlparse(r.url).hostname!='ps4torrent.net': raise ValueError('Unexpected redirect')
                return r.read(8*1024*1024)
        except Exception:
            if attempt==2: raise
            time.sleep(2+attempt)

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--no-covers',action='store_true'); args=ap.parse_args()
    out=ROOT/'assets'; (out/'covers').mkdir(parents=True,exist_ok=True); (ROOT/'tests/fixtures').mkdir(parents=True,exist_ok=True)
    games={}; last=2; page=1
    while page<=last:
        url=BASE if page==1 else BASE+f'page/{page}/'
        content=fetch(url).decode('utf-8'); parser=Cards(); parser.feed(content)
        if not parser.games: raise RuntimeError(f'No cards on {url}; previous snapshot preserved')
        if page<=2: (ROOT/f'tests/fixtures/page{page}.html').write_text(content)
        for g in parser.games: games.setdefault(g['id'],g)
        last=max([last]+list(parser.pages))
        if last>500: raise RuntimeError('Pagination exceeds safety bound')
        print(f'Page {page}/{last}: {len(games)} games',flush=True); page+=1; time.sleep(.3)
    def clean(s): return re.sub(r'[\t\r\n]+',' ',str(s))
    records=list(games.values()); payload=''.join('\t'.join(clean(g[k]) for k in ('id','title','year','size','url','image'))+'\n' for g in records)
    (out/'catalog.tsv.tmp').write_text(payload); (out/'catalog.tsv.tmp').replace(out/'catalog.tsv')
    (out/'catalog.json').write_text(json.dumps({'source':BASE,'updated':datetime.datetime.now(datetime.timezone.utc).isoformat(),'pages':last,'games':records},ensure_ascii=False,indent=2))
    if not args.no_covers:
        from PIL import Image
        def cover(g):
            path=out/'covers'/f"{g['id']}.jpg"
            if path.exists(): return True
            try:
                im=Image.open(io.BytesIO(fetch(g['image']))).convert('RGB'); im.thumbnail((300,400)); im.save(path,'JPEG',quality=85); time.sleep(.15); return True
            except Exception as e: print(f"Cover {g['id']}: {e}",flush=True); return False
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            results=list(pool.map(cover,records))
        print(f'{sum(results)}/{len(records)} covers available',flush=True)
    print(f'Complete: {len(records)} unique games from {last} pages',flush=True)
if __name__=='__main__': main()
