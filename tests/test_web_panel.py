"""Testes do Painel Web de Gerenciamento e API de Sincronização PS4."""
import http.client
import json
import struct
import sys
import tempfile
import threading
import unittest
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "web-panel"))
import server as panel_server  # noqa: E402


def make_fake_pkg(content_id="IV0000-HBST00099_00-SAMPLEHOMEBREW01", total_size=4096):
    buf = bytearray(total_size)
    buf[0:4] = b"\x7fCNT"
    buf[0x40 : 0x40 + 36] = content_id.encode("ascii")
    struct.pack_into(">I", buf, 0x74, 0x1A)
    struct.pack_into(">Q", buf, 0x20, 0x1000)
    struct.pack_into(">Q", buf, 0x28, 0)
    struct.pack_into(">Q", buf, 0x430, total_size)
    return bytes(buf)


def make_minimal_png():
    # PNG 1x1 RGB válido
    import zlib

    def chunk(tag, data):
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    idat = zlib.compress(b"\x00\x31\xb8\x78")
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


class WebPanelTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.srv = panel_server.create_server("127.0.0.1", 0, Path(self.tmp.name))
        self.port = self.srv.server_port
        self.thread = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.srv.server_close)
        self.addCleanup(self.srv.shutdown)

    def request(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        hdrs = headers or {}
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode("utf-8")
            hdrs.setdefault("Content-Type", "application/json")
        conn.request(method, path, body=body, headers=hdrs)
        resp = conn.getresponse()
        data = resp.read()
        resp_headers = dict(resp.getheaders())
        conn.close()
        return resp.status, resp_headers, data

    def test_starts_empty_without_bundled_content(self):
        status, _, data = self.request("GET", "/api/catalog.json")
        self.assertEqual(status, 200)
        cat = json.loads(data)
        self.assertEqual(cat["items"], [])
        self.assertGreaterEqual(len(cat["categories"]), 5)

    def test_crud_categories_homebrews_covers_and_pkg_range(self):
        # Criar nova categoria
        status, _, data = self.request("POST", "/api/admin/categories", {"name": "Portes SDL"})
        self.assertEqual(status, 201)
        new_cat = json.loads(data)["category"]

        # Upload de capa
        png = make_minimal_png()
        status, _, data = self.request(
            "POST", "/api/admin/upload/cover", body=png, headers={"Content-Type": "image/png"}
        )
        self.assertEqual(status, 201)
        cover_info = json.loads(data)
        self.assertTrue(cover_info["cover_file"])

        # Upload de PKG e detecção automática de metadados
        pkg = make_fake_pkg(total_size=8192)
        status, _, data = self.request(
            "POST",
            "/api/admin/upload/pkg",
            body=pkg,
            headers={"X-Filename": "meu-homebrew.pkg"},
        )
        self.assertEqual(status, 201)
        pkg_info = json.loads(data)
        self.assertEqual(pkg_info["content_id"], "IV0000-HBST00099_00-SAMPLEHOMEBREW01")
        self.assertEqual(pkg_info["size_bytes"], 8192)

        # Cadastrar homebrew
        status, _, data = self.request(
            "POST",
            "/api/admin/homebrews",
            {
                "title": "Meu Homebrew Teste",
                "category_id": new_cat["id"],
                "version": "1.2.0",
                "author": "DevBR",
                "description": "App de teste homebrew",
                "pkg_url": pkg_info["pkg_url"],
                "pkg_filename": pkg_info["pkg_filename"],
                "cover_file": cover_info["cover_file"],
                "content_id": pkg_info["content_id"],
                "size_bytes": pkg_info["size_bytes"],
                "size_text": pkg_info["size_text"],
            },
        )
        self.assertEqual(status, 201)
        item = json.loads(data)["item"]

        # Verificar catálogo sincronizado pelo PS4
        status, _, data = self.request("GET", "/api/catalog.json")
        self.assertEqual(status, 200)
        catalog = json.loads(data)
        self.assertEqual(len(catalog["items"]), 1)
        exported = catalog["items"][0]
        self.assertEqual(exported["title"], "Meu Homebrew Teste")
        self.assertEqual(exported["category"], "Portes SDL")
        self.assertIn(f":{self.port}/covers/", exported["cover_url"])
        self.assertIn(f":{self.port}/pkgs/", exported["pkg_url"])

        # Verificar Range request do PKG (206 Partial Content)
        pkg_path = urllib.parse.urlparse(exported["pkg_url"]).path
        status, hdrs, partial = self.request("GET", pkg_path, headers={"Range": "bytes=0-4095"})
        self.assertEqual(status, 206)
        self.assertEqual(len(partial), 4096)
        self.assertEqual(partial[:4], b"\x7fCNT")
        self.assertIn("bytes 0-4095/8192", hdrs.get("Content-Range", ""))

        # Verificar download da capa
        cover_path = urllib.parse.urlparse(exported["cover_url"]).path
        status, _, cover_bytes = self.request("GET", cover_path)
        self.assertEqual(status, 200)
        self.assertGreater(len(cover_bytes), 20)

        # Remover capa via edição
        status, _, _ = self.request("PUT", f"/api/admin/homebrews/{item['id']}", {"clear_cover": True})
        self.assertEqual(status, 200)
        status, _, _ = self.request("GET", cover_path)
        self.assertEqual(status, 404)

        # Remover homebrew + PKG do disco
        status, _, _ = self.request("DELETE", f"/api/admin/homebrews/{item['id']}?delete_pkg=1")
        self.assertEqual(status, 200)
        status, _, _ = self.request("GET", pkg_path)
        self.assertEqual(status, 404)

        status, _, data = self.request("GET", "/api/catalog.json")
        self.assertEqual(json.loads(data)["items"], [])


if __name__ == "__main__":
    unittest.main()
