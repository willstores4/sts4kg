import importlib.util, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('scrape', ROOT/'tools/scrape.py'); m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class ScraperTests(unittest.TestCase):
    def test_live_fixtures(self):
        for n in (1,2):
            p=m.Cards();p.feed((ROOT/f'tests/fixtures/page{n}.html').read_text());self.assertGreaterEqual(len(p.games),12)
            for g in p.games:
                self.assertTrue(g['title']);self.assertRegex(g['year'],r'^\d{4}$');self.assertRegex(g['size'],r'(?i)[mg]b$')
        self.assertIn(63,p.pages)
    def test_entities_single_quotes_and_nested_title(self):
        p=m.Cards();p.feed("<a class='item grid-item' href='/42-test.html'><img src='/a.jpg'><div class='item__meta'><span>2024</span><span>2 GB</span></div><div class='item__title'>A &amp; B</div></a>")
        self.assertEqual(p.games[0]['title'],'A & B');self.assertEqual(p.games[0]['id'],42)
    def test_offsite_and_non_games(self):
        p=m.Cards();p.feed('<a class="item" href="https://evil.example/1-test.html"><img src="/a.jpg"><div class="item__title">No</div></a>');self.assertEqual(p.games,[])
    def test_snapshot_is_unique_and_complete(self):
        import json
        data=json.loads((ROOT/'assets/catalog.json').read_text());self.assertEqual(data['pages'],63);self.assertEqual(len(data['games']),len({g['id'] for g in data['games']}));self.assertGreater(len(data['games']),700)
