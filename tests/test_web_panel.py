"""Testes do Painel Web de Gerenciamento e API de Sincronização PS4."""
import http.client
import json
import socket
import struct
import sys
import tempfile
import threading
import unittest
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HAS_PANEL = (ROOT / "web-panel" / "server.py").is_file()
if HAS_PANEL:
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


@unittest.skipUnless(HAS_PANEL, "web-panel/server.py not present in this checkout")
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

    def test_json_manifest_resolution_and_catalog_import_export(self):
        pkg = make_fake_pkg(content_id="IV0000-HBST00077_00-JSONMANIFEST0001", total_size=8192)
        status, _, data = self.request(
            "POST", "/api/admin/upload/pkg", body=pkg, headers={"X-Filename": "manifest-target.pkg"}
        )
        self.assertEqual(status, 201)
        pkg_url = f"http://127.0.0.1:{self.port}" + json.loads(data)["pkg_url"]

        # Mini servidor HTTP servindo um manifesto .manifest.json com formato "pieces" apontando para o .pkg acima
        class ManifestHandler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                payload = json.dumps({
                    "originalFileSize": 8192,
                    "numberOfSplitFiles": 1,
                    "pieces": [{"url": pkg_url, "fileOffset": 0, "fileSize": 8192}],
                }).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *args):
                pass

        manifest_srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), ManifestHandler)
        threading.Thread(target=manifest_srv.serve_forever, daemon=True).start()
        self.addCleanup(manifest_srv.server_close)
        self.addCleanup(manifest_srv.shutdown)
        manifest_url = f"http://127.0.0.1:{manifest_srv.server_port}/hb.manifest.json"

        # Inspecionar URL .json pelo painel
        status, _, data = self.request("POST", "/api/admin/inspect-pkg", {"pkg_url": manifest_url})
        self.assertEqual(status, 200)
        info = json.loads(data)
        self.assertTrue(info["from_json"])
        self.assertEqual(info["content_id"], "IV0000-HBST00077_00-JSONMANIFEST0001")

        # Importar catálogo .json diretamente
        status, _, data = self.request(
            "POST",
            "/api/admin/import-json",
            [{"name": "HB via JSON", "url": manifest_url, "category": "JSON Apps", "proxy_pkg": True}],
        )
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(data)["imported"], 1)

        # Proxy /api/pkg/<id>.pkg resolve .json e entrega os bytes do .pkg com Range
        status, _, data = self.request("GET", "/api/export.json")
        self.assertEqual(status, 200)
        exported = json.loads(data)
        self.assertEqual(len(exported["items"]), 1)
        proxy_path = urllib.parse.urlparse(exported["items"][0]["pkg_url"]).path
        status, _, head_bytes = self.request("GET", proxy_path, headers={"Range": "bytes=0-4095"})
        self.assertEqual(status, 206)
        self.assertEqual(head_bytes[:4], b"\x7fCNT")

    def test_split_manifest_proxy_merging_and_cross_boundary_range(self):
        # Constrói um PKG de 8192 bytes dividido em 2 peças de 4096 bytes cada
        full_pkg = bytearray(make_fake_pkg(content_id="IV0000-HBST00088_00-SPLITPKG00000001", total_size=8192))
        for i in range(0x1000, 8192):
            full_pkg[i] = i % 251
        part0 = bytes(full_pkg[:4096])
        part1 = bytes(full_pkg[4096:])

        class SplitHandler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path == "/split.manifest.json":
                    base = f"http://127.0.0.1:{self.server.server_port}"
                    payload = json.dumps({
                        "originalFileSize": 8192,
                        "numberOfSplitFiles": 2,
                        "pieces": [
                            {"url": f"{base}/part_0.pkg", "fileOffset": 0, "fileSize": 4096},
                            {"url": f"{base}/part_1.pkg", "fileOffset": 4096, "fileSize": 4096},
                        ],
                    }).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                    return

                blob = part0 if self.path == "/part_0.pkg" else part1 if self.path == "/part_1.pkg" else None
                if blob is None:
                    self.send_response(404)
                    self.end_headers()
                    return
                rng = self.headers.get("Range")
                if rng and rng.startswith("bytes="):
                    s, _, e = rng[6:].partition("-")
                    start = int(s) if s else 0
                    end = int(e) if e else len(blob) - 1
                    chunk = blob[start : end + 1]
                    self.send_response(206)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(len(chunk)))
                    self.send_header("Content-Range", f"bytes {start}-{end}/{len(blob)}")
                    self.end_headers()
                    self.wfile.write(chunk)
                else:
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(len(blob)))
                    self.end_headers()
                    self.wfile.write(blob)

            def log_message(self, *args):
                pass

        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), SplitHandler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        split_url = f"http://127.0.0.1:{srv.server_port}/split.manifest.json"

        # 1. inspect-pkg reconhece as 2 partes e recomenda proxy
        status, _, data = self.request("POST", "/api/admin/inspect-pkg", {"pkg_url": split_url})
        self.assertEqual(status, 200)
        info = json.loads(data)
        self.assertTrue(info["from_json"])
        self.assertEqual(info["split_pieces"], 2)
        self.assertTrue(info["recommend_proxy"])
        self.assertEqual(info["size_bytes"], 8192)
        self.assertEqual(info["content_id"], "IV0000-HBST00088_00-SPLITPKG00000001")

        # 2. Cadastrar sem passar proxy_pkg ativa proxy automaticamente via manifesto dividido
        status, _, data = self.request(
            "POST", "/api/admin/homebrews", {"title": "Split HB", "pkg_url": split_url}
        )
        self.assertEqual(status, 201)
        created = json.loads(data)["item"]
        self.assertEqual(created["proxy_pkg"], 1)

        # 3. Export e GET completo + Range cruzando fronteira entre part_0 e part_1 (bytes 3900-4300)
        status, _, data = self.request("GET", "/api/export.json")
        self.assertEqual(status, 200)
        proxy_path = urllib.parse.urlparse(json.loads(data)["items"][0]["pkg_url"]).path

        status, hdrs, cross = self.request("GET", proxy_path, headers={"Range": "bytes=3900-4300"})
        self.assertEqual(status, 206)
        self.assertEqual(hdrs.get("Content-Range"), "bytes 3900-4300/8192")
        self.assertEqual(cross, bytes(full_pkg[3900:4301]))

        status, _, whole = self.request("GET", proxy_path)
        self.assertEqual(status, 200)
        self.assertEqual(whole, bytes(full_pkg))

    def test_multi_links_options_and_manual_split_links(self):
        # Servidor auxiliar com Base PKG, Update PKG e 2 partes divididas sem manifesto JSON
        base_pkg = make_fake_pkg(content_id="IV0000-HBST00010_00-BASEPACKAGE00001", total_size=4096)
        upd_pkg = make_fake_pkg(content_id="IV0000-HBST00010_00-UPDATEPACK000002", total_size=4096)
        full_split = bytearray(make_fake_pkg(content_id="IV0000-HBST00011_00-MANUALSPLIT00001", total_size=8192))
        for i in range(0x1000, 8192):
            full_split[i] = (i * 3) % 251
        sp0 = bytes(full_split[:4096])
        sp1 = bytes(full_split[4096:])

        class UpstreamHandler(http.server.BaseHTTPRequestHandler):
            blobs = {"/base.pkg": base_pkg, "/upd.pkg": upd_pkg, "/sp_0.pkg": sp0, "/sp_1.pkg": sp1}

            def _serve(self, send_body: bool):
                blob = self.blobs.get(self.path)
                if blob is None:
                    self.send_response(404)
                    self.end_headers()
                    return
                rng = self.headers.get("Range")
                if rng and rng.startswith("bytes="):
                    s, _, e = rng[6:].partition("-")
                    start = int(s) if s else 0
                    end = int(e) if e else len(blob) - 1
                    chunk = blob[start : end + 1]
                    self.send_response(206)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(len(chunk)))
                    self.send_header("Content-Range", f"bytes {start}-{end}/{len(blob)}")
                    self.end_headers()
                    if send_body:
                        self.wfile.write(chunk)
                else:
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(len(blob)))
                    self.end_headers()
                    if send_body:
                        self.wfile.write(blob)

            def do_HEAD(self):
                self._serve(False)

            def do_GET(self):
                self._serve(True)

            def log_message(self, *args):
                pass

        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), UpstreamHandler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        up = f"http://127.0.0.1:{srv.server_port}"

        # 1. Cadastrar conteúdo com 2 opções (Base + Update com proxy no link extra)
        status, _, data = self.request("POST", "/api/admin/homebrews", {
            "title": "App Com Update",
            "primary_label": "Base v1.0",
            "pkg_url": f"{up}/base.pkg",
            "link_mode": "options",
            "extra_links": [{
                "label": "Update v1.1",
                "url": f"{up}/upd.pkg",
                "content_id": "IV0000-HBST00010_00-UPDATEPACK000002",
                "size_bytes": 4096,
                "content_type": 27,
                "proxy_pkg": True,
                "use_binloader": True,
            }],
        })
        self.assertEqual(status, 201)
        item_opt = json.loads(data)["item"]
        self.assertEqual(len(item_opt["extra_links"]), 1)

        status, _, cat_raw = self.request("GET", "/api/catalog.json")
        self.assertEqual(status, 200)
        cat = json.loads(cat_raw)
        opt_exported = cat["items"][0]
        self.assertEqual(len(opt_exported["links"]), 2)
        self.assertEqual(opt_exported["links"][0]["label"], "Base v1.0")
        self.assertEqual(opt_exported["links"][1]["label"], "Update v1.1")
        self.assertFalse(opt_exported["links"][0]["use_binloader"])
        self.assertTrue(opt_exported["links"][1]["use_binloader"])
        link1_path = urllib.parse.urlparse(opt_exported["links"][1]["url"])
        status, _, upd_bytes = self.request("GET", f"{link1_path.path}?{link1_path.query}")
        self.assertEqual(status, 200)
        self.assertEqual(upd_bytes, upd_pkg)

        # 2. Detectar sp_1.pkg como parte dividida (sem header \x7fCNT) e cadastrar modo split
        status, _, insp_raw = self.request(
            "POST", "/api/admin/inspect-pkg", {"pkg_url": f"{up}/sp_1.pkg", "allow_split_part": True}
        )
        self.assertEqual(status, 200)
        insp = json.loads(insp_raw)
        self.assertTrue(insp["is_split_part"])
        self.assertEqual(insp["size_bytes"], 4096)

        status, _, sp_raw = self.request("POST", "/api/admin/homebrews", {
            "title": "App Split Manual",
            "primary_label": "Parte 1",
            "pkg_url": f"{up}/sp_0.pkg",
            "link_mode": "split",
            "extra_links": [{
                "label": "Parte 2",
                "url": f"{up}/sp_1.pkg",
                "http_size_bytes": 4096,
                "size_bytes": 4096,
            }],
        })
        self.assertEqual(status, 201)
        sp_item = json.loads(sp_raw)["item"]
        self.assertEqual(sp_item["proxy_pkg"], 1)

        # Verificar Range cruzando fronteira sp_0 -> sp_1 e download completo
        status, _, whole_sp = self.request("GET", f"/api/pkg/{sp_item['id']}.pkg")
        self.assertEqual(status, 200)
        self.assertEqual(whole_sp, bytes(full_split))

        status, hdrs, cross_sp = self.request(
            "GET", f"/api/pkg/{sp_item['id']}.pkg", headers={"Range": "bytes=4000-4200"}
        )
        self.assertEqual(status, 206)
        self.assertEqual(hdrs.get("Content-Range"), "bytes 4000-4200/8192")
        self.assertEqual(cross_sp, bytes(full_split[4000:4201]))

        # 3. Round-trip import-json preservando "links"
        exported_json = json.dumps(cat).encode("utf-8")
        status, _, imp_raw = self.request(
            "POST", "/api/admin/import-json", body=exported_json, headers={"Content-Type": "application/json"}
        )
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(imp_raw)["imported"], 1)

        status, _, state_raw = self.request("GET", "/api/admin/state")
        self.assertEqual(status, 200)
        imported_matches = [it for it in json.loads(state_raw)["items"] if it["title"] == "App Com Update"]
        self.assertEqual(len(imported_matches), 2)
        self.assertEqual(len(imported_matches[0]["extra_links"]), 1)
        self.assertEqual(imported_matches[0]["extra_links"][0]["label"], "Update v1.1")
        self.assertTrue(imported_matches[0]["extra_links"][0]["use_binloader"])

    def test_06_proxy_head_and_ua_fallback(self):
        pkg_bytes = bytearray(make_fake_pkg(content_id="IV0000-WGST00099_00-PROXYTEST0000001", total_size=8192))
        for i in range(0x1000, 8192):
            pkg_bytes[i] = (i * 7) % 251
        blob = bytes(pkg_bytes)

        class StrictUpstreamHandler(http.server.BaseHTTPRequestHandler):
            def do_HEAD(self):
                # Simula origem que bloqueia método HEAD (405) e UA PlayStation 4 (403)
                self.send_response(405)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def do_GET(self):
                if "PlayStation" in (self.headers.get("User-Agent") or ""):
                    self.send_response(403)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                rng = self.headers.get("Range")
                if self.path == "/strict.pkg" and rng and rng.startswith("bytes="):
                    s, _, e = rng[6:].partition("-")
                    start = int(s) if s else 0
                    end = min(len(blob) - 1, int(e) if e else len(blob) - 1)
                    chunk = blob[start : end + 1]
                    self.send_response(206)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(len(chunk)))
                    self.send_header("Content-Range", f"bytes {start}-{end}/{len(blob)}")
                    self.end_headers()
                    self.wfile.write(chunk)
                    return
                if self.path in ("/strict.pkg", "/norange.pkg"):
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(len(blob)))
                    self.end_headers()
                    self.wfile.write(blob)
                    return
                self.send_response(404)
                self.end_headers()

            def log_message(self, *args):
                pass

        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), StrictUpstreamHandler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        up = f"http://127.0.0.1:{srv.server_port}"

        for idx, path in enumerate(("/strict.pkg", "/norange.pkg")):
            status, _, data = self.request("POST", "/api/admin/homebrews", {
                "title": f"Proxy Strict {idx}",
                "pkg_url": f"{up}{path}",
                "content_id": "IV0000-WGST00099_00-PROXYTEST0000001",
                "size_bytes": len(blob),
                "proxy_pkg": True,
            })
            self.assertEqual(status, 201)
            item_id = json.loads(data)["item"]["id"]

            # HEAD deve cair no fallback GET Range=0-0 e devolver tamanho + Accept-Ranges
            status, hdrs, body = self.request("HEAD", f"/api/pkg/{item_id}.pkg")
            self.assertEqual(status, 200)
            self.assertEqual(hdrs.get("Content-Length"), str(len(blob)))
            self.assertEqual(hdrs.get("Accept-Ranges"), "bytes")
            self.assertEqual(body, b"")

            status, hdrs, body = self.request(
                "HEAD", f"/api/pkg/{item_id}.pkg", headers={"Range": "bytes=0-31"}
            )
            self.assertEqual(status, 206)
            self.assertEqual(hdrs.get("Content-Length"), "32")
            self.assertEqual(hdrs.get("Content-Range"), f"bytes 0-31/{len(blob)}")
            self.assertEqual(body, b"")

            # GET com Range deve devolver fatia exata mesmo quando a origem ignora Range (/norange.pkg)
            status, hdrs, part = self.request(
                "GET", f"/api/pkg/{item_id}.pkg", headers={"Range": "bytes=16-63"}
            )
            self.assertEqual(status, 206)
            self.assertEqual(hdrs.get("Content-Range"), f"bytes 16-63/{len(blob)}")
            self.assertEqual(part, blob[16:64])

            status, _, whole = self.request("GET", f"/api/pkg/{item_id}.pkg")
            self.assertEqual(status, 200)
            self.assertEqual(whole, blob)

    def test_07_binloader_elf_and_send(self):
        # Cria template ELF sintético com a assinatura WILL_BGFT_CFG_1\x00
        cfg_size = 16 + 8 + 1024 + 256 + 64 + 512 + 16
        tpl = bytearray(b"\x7fELF" + b"\x00" * 124)
        tpl.extend(b"WILL_BGFT_CFG_1\x00" + b"\x00" * (cfg_size - 16))
        tpl.extend(b"\x00" * 64)
        tpl_path = Path(self.tmp.name) / "bgft_template.elf"
        tpl_path.write_bytes(bytes(tpl))
        self.srv.binloader_template_path = tpl_path

        status, _, data = self.request("POST", "/api/admin/homebrews", {
            "title": "App BinLoader ELF",
            "pkg_url": "https://cdn.example.org/app-binloader.pkg",
            "content_id": "IV0000-WGST00077_00-BINLOADERELF0001",
            "size_bytes": 123456789,
            "content_type": 26,
            "proxy_pkg": False,
        })
        self.assertEqual(status, 201)
        item_id = json.loads(data)["item"]["id"]

        # Download do .elf gerado
        status, hdrs, elf_bytes = self.request("GET", f"/api/pkg/{item_id}.elf")
        self.assertEqual(status, 200)
        self.assertEqual(len(elf_bytes), len(tpl))
        self.assertTrue(elf_bytes.startswith(b"\x7fELF"))
        pos = elf_bytes.find(b"WILL_BGFT_CFG_1\x00")
        self.assertGreater(pos, 0)
        self.assertEqual(struct.unpack_from("<Q", elf_bytes, pos + 16)[0], 123456789)
        self.assertEqual(elf_bytes[pos + 24 : pos + 24 + 1024].split(b"\x00", 1)[0], b"https://cdn.example.org/app-binloader.pkg")
        self.assertEqual(elf_bytes[pos + 24 + 1024 : pos + 24 + 1024 + 64].split(b"\x00", 1)[0], b"App BinLoader ELF")

        # Envio TCP para porta BinLoader local em 127.0.0.1
        srv_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv_sock.bind(("127.0.0.1", 0))
        srv_sock.listen(1)
        bin_port = srv_sock.getsockname()[1]
        self.addCleanup(srv_sock.close)

        received = []

        def accept_once():
            conn, _ = srv_sock.accept()
            with conn:
                while True:
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    received.append(chunk)

        th = threading.Thread(target=accept_once, daemon=True)
        th.start()

        status, _, send_res = self.request("POST", "/api/admin/send-binloader", {
            "item_id": item_id,
            "link_index": 0,
            "ip": "127.0.0.1",
            "port": bin_port,
        })
        th.join(timeout=3)
        self.assertEqual(status, 200)
        parsed = json.loads(send_res)
        self.assertTrue(parsed["ok"])
        self.assertEqual(parsed["bytes"], len(elf_bytes))
        self.assertEqual(b"".join(received), elf_bytes)

        # Rejeita IP externo público
        status, _, _ = self.request("POST", "/api/admin/send-binloader", {
            "item_id": item_id,
            "ip": "8.8.8.8",
            "port": 9090,
        })
        self.assertEqual(status, 400)

        # Manifesto dividido (.manifest.json) com proxy_pkg=False deve gerar .ELF direto sem exigir proxy
        from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

        class ManifestOnlyHandler(BaseHTTPRequestHandler):
            def do_GET(self):
                body = json.dumps({
                    "originalFileSize": 8192,
                    "pieces": [
                        {"url": f"http://127.0.0.1:{self.server.server_port}/part_0.pkg", "fileOffset": 0, "fileSize": 4096},
                        {"url": f"http://127.0.0.1:{self.server.server_port}/part_1.pkg", "fileOffset": 4096, "fileSize": 4096},
                    ],
                }).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, format, *args):
                return

        manifest_srv = ThreadingHTTPServer(("127.0.0.1", 0), ManifestOnlyHandler)
        threading.Thread(target=manifest_srv.serve_forever, daemon=True).start()
        self.addCleanup(manifest_srv.shutdown)
        self.addCleanup(manifest_srv.server_close)
        manifest_url = f"http://127.0.0.1:{manifest_srv.server_address[1]}/app.manifest.json"

        status, _, split_data = self.request("POST", "/api/admin/homebrews", {
            "title": "Split Manifest ELF",
            "pkg_url": manifest_url,
            "content_id": "IV0000-WGST00078_00-SPLITELF00000001",
            "size_bytes": 8192,
            "content_type": 26,
            "proxy_pkg": False,
            "use_binloader": True,
        })
        self.assertEqual(status, 201)
        split_item = json.loads(split_data)["item"]
        self.assertEqual(split_item["proxy_pkg"], 0)
        self.assertEqual(split_item["use_binloader"], 1)

        status, _, split_elf = self.request("GET", f"/api/pkg/{split_item['id']}.elf")
        self.assertEqual(status, 200)
        spos = split_elf.find(b"WILL_BGFT_CFG_1\x00")
        self.assertGreater(spos, 0)
        self.assertEqual(struct.unpack_from("<Q", split_elf, spos + 16)[0], 8192)
        self.assertEqual(
            split_elf[spos + 24 : spos + 24 + 1024].split(b"\x00", 1)[0],
            manifest_url.encode("utf-8"),
        )

    def test_08_direct_download_path_and_upload(self):
        # 1. Upload de arquivo qualquer (não-PKG) com X-Allow-Direct-File: 1
        bin_blob = b"\xde\xad\xbe\xefGOLDHEN_PAYLOAD"
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        conn.request(
            "POST",
            "/api/admin/upload/pkg",
            body=bin_blob,
            headers={
                "X-Filename": "goldhen_v2.4.bin",
                "X-Allow-Direct-File": "1",
                "Content-Length": str(len(bin_blob)),
            },
        )
        resp = conn.getresponse()
        up_data = json.loads(resp.read().decode("utf-8"))
        conn.close()
        self.assertEqual(resp.status, 201)
        self.assertTrue(up_data["pkg_url"].endswith(".bin"))

        # 2. Rejeita caminho inválido sem / inicial ou com ..
        status, _, err_rel = self.request("POST", "/api/admin/homebrews", {
            "title": "Invalido Relativo",
            "pkg_url": up_data["pkg_url"],
            "direct_download": True,
            "direct_path": "data/pkg",
        })
        self.assertEqual(status, 400)
        self.assertIn("começar com /", json.loads(err_rel)["error"])

        status, _, _ = self.request("POST", "/api/admin/homebrews", {
            "title": "Invalido Traversal",
            "pkg_url": up_data["pkg_url"],
            "direct_download": True,
            "direct_path": "/data/../etc",
        })
        self.assertEqual(status, 400)

        # 3. Cadastra item principal com /data/ e link extra com proxy + /data/pkg
        class DirectUpstream(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.send_header("Content-Length", str(len(bin_blob)))
                self.end_headers()
                self.wfile.write(bin_blob)

            def log_message(self, *args):
                return

        up_srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), DirectUpstream)
        threading.Thread(target=up_srv.serve_forever, daemon=True).start()
        self.addCleanup(up_srv.shutdown)
        self.addCleanup(up_srv.server_close)
        ext_url = f"http://127.0.0.1:{up_srv.server_address[1]}/files/cheats.zip"

        status, _, created_raw = self.request("POST", "/api/admin/homebrews", {
            "title": "GoldHEN Direct",
            "primary_label": "Payload BIN",
            "pkg_url": up_data["pkg_url"],
            "pkg_filename": up_data["pkg_filename"],
            "size_bytes": up_data["size_bytes"],
            "direct_download": True,
            "direct_path": "/data/",
            "use_binloader": True,
            "extra_links": [{
                "label": "Pacote Extra ZIP",
                "url": ext_url,
                "proxy_pkg": True,
                "direct_download": True,
                "direct_path": "/data/pkg",
            }],
        })
        self.assertEqual(status, 201)
        created = json.loads(created_raw)["item"]
        self.assertEqual(created["direct_download"], 1)
        self.assertEqual(created["direct_path"], "/data/")
        self.assertEqual(created["use_binloader"], 0)
        self.assertTrue(created["extra_links"][0]["direct_download"])
        self.assertEqual(created["extra_links"][0]["direct_path"], "/data/pkg")

        status, _, cat_raw = self.request("GET", "/api/catalog.json")
        self.assertEqual(status, 200)
        cat = json.loads(cat_raw)
        exported = cat["items"][0]
        self.assertTrue(exported["direct_download"])
        self.assertEqual(exported["direct_path"], "/data/")
        self.assertFalse(exported["use_binloader"])
        self.assertEqual(exported["links"][0]["direct_path"], "/data/")
        self.assertTrue(exported["links"][1]["direct_download"])
        self.assertEqual(exported["links"][1]["direct_path"], "/data/pkg")
        self.assertIn("/cheats.zip?link=1", exported["links"][1]["url"])

        # Proxy route preservando extensão original entrega os bytes exatos
        p_parsed = urllib.parse.urlparse(exported["links"][1]["url"])
        status, _, proxied_zip = self.request("GET", f"{p_parsed.path}?{p_parsed.query}")
        self.assertEqual(status, 200)
        self.assertEqual(proxied_zip, bin_blob)

        # Round-trip import-json preservando direct_download e direct_path
        status, _, imp_raw = self.request(
            "POST", "/api/admin/import-json", body=cat_raw, headers={"Content-Type": "application/json"}
        )
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(imp_raw)["imported"], 1)

    def test_09_user_manager_and_app_login(self):
        # 1. Sem usuários cadastrados: catálogo aberto e auth-info indica login não obrigatório
        status, _, raw = self.request("GET", "/api/app/auth-info")
        self.assertEqual(status, 200)
        info = json.loads(raw)
        self.assertFalse(info["login_required"])
        self.assertTrue(info["authenticated"])

        # Sobe um PKG local e cria um item
        pkg = make_fake_pkg("EP9999-USER99999_00-LOGINTEST0000001")
        status, _, up_raw = self.request(
            "POST",
            "/api/admin/upload/pkg",
            body=pkg,
            headers={
                "Content-Type": "application/octet-stream",
                "X-Filename": "login_test.pkg",
                "Content-Length": str(len(pkg)),
            },
        )
        self.assertEqual(status, 201)
        up = json.loads(up_raw)
        status, _, item_raw = self.request("POST", "/api/admin/homebrews", {
            "title": "App Protegido",
            "pkg_url": up["pkg_url"],
            "pkg_filename": up["pkg_filename"],
            "size_bytes": up["size_bytes"],
            "content_id": up["content_id"],
        })
        self.assertEqual(status, 201)

        # 2. Validações de criação de usuário + criação bem-sucedida
        status, _, _ = self.request("POST", "/api/admin/users", {"username": "ab", "password": "12"})
        self.assertEqual(status, 400)

        status, _, u_raw = self.request("POST", "/api/admin/users", {
            "username": "cliente01",
            "display_name": "Cliente Um",
            "password": "senha123",
            "notes": "PS4 Sala",
            "active": True,
        })
        self.assertEqual(status, 201)
        user_obj = json.loads(u_raw)["user"]
        uid = user_obj["id"]
        self.assertEqual(user_obj["username"], "cliente01")
        self.assertNotIn("password_hash", user_obj)

        # Usuário duplicado ignorando maiúsculas/minúsculas é rejeitado
        status, _, _ = self.request("POST", "/api/admin/users", {
            "username": "Cliente01",
            "password": "outra123",
        })
        self.assertEqual(status, 400)

        # 3. Com usuário ativo (modo auto), rotas públicas passam a exigir login
        status, _, info_raw = self.request("GET", "/api/app/auth-info")
        self.assertEqual(status, 200)
        info = json.loads(info_raw)
        self.assertTrue(info["login_required"])
        self.assertFalse(info["authenticated"])

        for protected in ("/api/catalog.json", "/catalog.json", "/api/export.json", up["pkg_url"]):
            st, _, _ = self.request("GET", protected)
            self.assertEqual(st, 401, msg=f"{protected} deveria exigir auth")

        # Export do painel continua disponível via rota administrativa
        status, _, adm_exp = self.request("GET", "/api/admin/export.json")
        self.assertEqual(status, 200)
        self.assertEqual(len(json.loads(adm_exp)["items"]), 1)

        # Estado admin lista usuários sem hash
        status, _, st_raw = self.request("GET", "/api/admin/state")
        self.assertEqual(status, 200)
        st_data = json.loads(st_raw)
        self.assertTrue(st_data["app_login_required"])
        self.assertEqual(len(st_data["users"]), 1)
        self.assertNotIn("password_hash", st_data["users"][0])

        # 4. Senha incorreta retorna 401; login válido retorna token e libera acesso
        status, _, _ = self.request("POST", "/api/app/login", {"username": "cliente01", "password": "errada"})
        self.assertEqual(status, 401)

        dev1 = "PS4-AA:BB:CC:00:00:01"
        status, _, login_raw = self.request("POST", "/api/app/login", {
            "username": "Cliente01",
            "password": "senha123",
            "device_id": dev1,
        })
        self.assertEqual(status, 200)
        login_data = json.loads(login_raw)
        tok = login_data["token"]
        self.assertTrue(tok)
        self.assertEqual(login_data["user"]["display_name"], "Cliente Um")

        # auth-info com X-App-Token valida sessão
        status, _, auth_ok_raw = self.request("GET", "/api/app/auth-info", headers={"X-App-Token": tok, "X-Device-Id": dev1})
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(auth_ok_raw)["authenticated"])

        # Catálogo retorna token embutido nas URLs internas para BGFT/PS4
        status, _, cat_raw = self.request("GET", "/api/catalog.json", headers={"X-App-Token": tok, "X-Device-Id": dev1})
        self.assertEqual(status, 200)
        cat = json.loads(cat_raw)
        exported_pkg_url = cat["items"][0]["pkg_url"]
        self.assertIn(f"token={tok}", exported_pkg_url)
        parsed_pkg = urllib.parse.urlparse(exported_pkg_url)
        status, _, fetched_pkg = self.request("GET", f"{parsed_pkg.path}?{parsed_pkg.query}")
        self.assertEqual(status, 200)
        self.assertEqual(fetched_pkg, pkg)

        # 5. Revogação derruba a sessão imediatamente
        status, _, rev_raw = self.request("POST", f"/api/admin/users/{uid}/revoke")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(rev_raw)["revoked"], 1)
        status, _, _ = self.request("GET", "/api/catalog.json", headers={"X-App-Token": tok, "X-Device-Id": dev1})
        self.assertEqual(status, 401)

        # 6. Bloqueio e expiração impedem login e invalidam sessões
        status, _, login_raw2 = self.request("POST", "/api/app/login", {
            "username": "cliente01",
            "password": "senha123",
            "device_id": dev1,
        })
        self.assertEqual(status, 200)
        tok2 = json.loads(login_raw2)["token"]

        status, _, _ = self.request("PUT", f"/api/admin/users/{uid}", {"active": False})
        self.assertEqual(status, 200)
        status, _, _ = self.request("GET", "/api/catalog.json", headers={"X-App-Token": tok2, "X-Device-Id": dev1})
        self.assertEqual(status, 401)
        status, _, _ = self.request("POST", "/api/app/login", {"username": "cliente01", "password": "senha123", "device_id": dev1})
        self.assertEqual(status, 403)

        # Reativa com data expirada
        status, _, _ = self.request("PUT", f"/api/admin/users/{uid}", {"active": True, "expires_at": 1000})
        self.assertEqual(status, 200)
        status, _, _ = self.request("POST", "/api/app/login", {"username": "cliente01", "password": "senha123", "device_id": dev1})
        self.assertEqual(status, 403)

        # 7. Modo '0' libera catálogo mesmo com usuários; excluir todos os usuários libera em modo 'auto'
        status, _, _ = self.request("PUT", "/api/admin/settings", {
            "store_name": "WILLGAME Store",
            "store_subtitle": "Teste",
            "require_app_login": "0",
        })
        self.assertEqual(status, 200)
        status, _, _ = self.request("GET", "/api/catalog.json")
        self.assertEqual(status, 200)

        status, _, _ = self.request("PUT", "/api/admin/settings", {
            "store_name": "WILLGAME Store",
            "store_subtitle": "Teste",
            "require_app_login": "auto",
        })
        self.assertEqual(status, 200)
        status, _, _ = self.request("GET", "/api/catalog.json")
        self.assertEqual(status, 401)

        status, _, _ = self.request("DELETE", f"/api/admin/users/{uid}")
        self.assertEqual(status, 200)
        status, _, _ = self.request("GET", "/api/catalog.json")
        self.assertEqual(status, 200)

    def test_10_session_limit_per_user_and_policy(self):
        dev1 = "PS4-AA:BB:CC:11:22:33"
        # 1. Cria usuário limitado a 1 sessão (política padrão 'block')
        status, _, u1_raw = self.request("POST", "/api/admin/users", {
            "username": "limitado",
            "password": "123",
            "max_sessions": 1,
        })
        self.assertEqual(status, 201)
        u1 = json.loads(u1_raw)["user"]
        self.assertEqual(u1["max_sessions"], 1)
        self.assertEqual(u1["effective_max_sessions"], 1)

        # Primeiro login ok
        status, _, l1_raw = self.request("POST", "/api/app/login", {"username": "limitado", "password": "123", "device_id": dev1})
        self.assertEqual(status, 200)
        tok1 = json.loads(l1_raw)["token"]
        self.assertEqual(json.loads(l1_raw)["user"]["max_sessions"], 1)

        # Segundo login bloqueado (403)
        status, _, l2_err = self.request("POST", "/api/app/login", {"username": "limitado", "password": "123", "device_id": dev1})
        self.assertEqual(status, 403)
        self.assertIn("Limite de sessões", json.loads(l2_err)["error"])

        # Sessão 1 segue válida
        status, _, _ = self.request("GET", "/api/app/me", headers={"X-App-Token": tok1, "X-Device-Id": dev1})
        self.assertEqual(status, 200)

        # Logout da sessão 1 libera novo login
        status, _, _ = self.request("POST", "/api/app/logout", headers={"X-App-Token": tok1})
        self.assertEqual(status, 200)
        status, _, l2_ok = self.request("POST", "/api/app/login", {"username": "limitado", "password": "123", "device_id": dev1})
        self.assertEqual(status, 200)
        tok2 = json.loads(l2_ok)["token"]

        # 2. Muda política para 'kick_oldest': novo login derruba sessão anterior automaticamente
        status, _, _ = self.request("PUT", "/api/admin/settings", {
            "store_name": "WILLGAME Store",
            "store_subtitle": "Teste",
            "session_limit_policy": "kick_oldest",
            "default_max_sessions": 2,
        })
        self.assertEqual(status, 200)

        status, _, l3_ok = self.request("POST", "/api/app/login", {"username": "limitado", "password": "123", "device_id": dev1})
        self.assertEqual(status, 200)
        tok3 = json.loads(l3_ok)["token"]

        status, _, _ = self.request("GET", "/api/app/me", headers={"X-App-Token": tok2, "X-Device-Id": dev1})
        self.assertEqual(status, 401)
        status, _, _ = self.request("GET", "/api/app/me", headers={"X-App-Token": tok3, "X-Device-Id": dev1})
        self.assertEqual(status, 200)

        # 3. Usuário usando padrão global (2 sessões) vs usuário com -1 (ilimitado)
        status, _, u_def_raw = self.request("POST", "/api/admin/users", {
            "username": "padrao_global",
            "password": "123",
            "max_sessions": 0,
        })
        self.assertEqual(status, 201)
        u_def = json.loads(u_def_raw)["user"]
        self.assertEqual(u_def["effective_max_sessions"], 2)

        status, _, _ = self.request("PUT", "/api/admin/settings", {
            "store_name": "WILLGAME Store",
            "store_subtitle": "Teste",
            "session_limit_policy": "block",
            "default_max_sessions": 2,
        })
        self.assertEqual(status, 200)

        t_a = json.loads(self.request("POST", "/api/app/login", {"username": "padrao_global", "password": "123", "device_id": dev1})[2])["token"]
        t_b = json.loads(self.request("POST", "/api/app/login", {"username": "padrao_global", "password": "123", "device_id": dev1})[2])["token"]
        status, _, _ = self.request("POST", "/api/app/login", {"username": "padrao_global", "password": "123", "device_id": dev1})
        self.assertEqual(status, 403)

        # Reduzir limite individual do usuário para 1 derruba excesso e preserva sessão mais recente
        status, _, _ = self.request("PUT", f"/api/admin/users/{u_def['id']}", {"max_sessions": 1})
        self.assertEqual(status, 200)
        status, _, st_raw = self.request("GET", "/api/admin/users")
        self.assertEqual(status, 200)
        u_now = [x for x in json.loads(st_raw)["users"] if x["id"] == u_def["id"]][0]
        self.assertEqual(u_now["active_sessions"], 1)
        status, _, _ = self.request("GET", "/api/app/me", headers={"X-App-Token": t_a, "X-Device-Id": dev1})
        self.assertEqual(status, 401)
        status, _, _ = self.request("GET", "/api/app/me", headers={"X-App-Token": t_b, "X-Device-Id": dev1})
        self.assertEqual(status, 200)

        # Usuário com max_sessions = -1 ignora limite global (2) e permite 3+ sessões
        status, _, u_unlim_raw = self.request("POST", "/api/admin/users", {
            "username": "vip_ilimitado",
            "password": "123",
            "max_sessions": -1,
        })
        self.assertEqual(status, 201)
        self.assertEqual(json.loads(u_unlim_raw)["user"]["effective_max_sessions"], 0)
        for _ in range(3):
            st, _, _ = self.request("POST", "/api/app/login", {"username": "vip_ilimitado", "password": "123", "device_id": dev1})
            self.assertEqual(st, 200)

    def test_11_device_binding_anti_sharing(self):
        ps4_a = "PS4-11:22:33:44:55:66"
        ps4_b = "PS4-AA:BB:CC:DD:EE:FF"

        # 1. Por padrão, usuário nasce com vínculo automático ativado
        status, _, u_raw = self.request("POST", "/api/admin/users", {
            "username": "vinculado",
            "password": "123",
        })
        self.assertEqual(status, 201)
        u = json.loads(u_raw)["user"]
        uid = u["id"]
        self.assertTrue(u["effective_bind_device"])
        self.assertEqual(u["bound_device_id"], "")

        # Login sem device_id é bloqueado
        status, _, err_no_dev = self.request("POST", "/api/app/login", {
            "username": "vinculado",
            "password": "123",
        })
        self.assertEqual(status, 403)
        self.assertIn("Identificação do PS4", json.loads(err_no_dev)["error"])

        # 2. Console A entra primeiro e fica vinculado permanentemente
        status, _, la_raw = self.request("POST", "/api/app/login", {
            "username": "vinculado",
            "password": "123",
            "device_id": ps4_a,
        })
        self.assertEqual(status, 200)
        la = json.loads(la_raw)
        tok_a = la["token"]
        self.assertEqual(la["user"]["bound_device_id"], ps4_a)

        # Mesmo se Console A fizer logout, Console B NÃO consegue usar o login alternadamente
        status, _, _ = self.request("POST", "/api/app/logout", headers={"X-App-Token": tok_a})
        self.assertEqual(status, 200)

        status, _, lb_err = self.request("POST", "/api/app/login", {
            "username": "vinculado",
            "password": "123",
            "device_id": ps4_b,
        })
        self.assertEqual(status, 403)
        self.assertIn("vinculada a outro PS4", json.loads(lb_err)["error"])
        self.assertIn(ps4_a, json.loads(lb_err)["error"])

        # Console A continua conseguindo logar novamente sem problemas
        status, _, la2_raw = self.request("POST", "/api/app/login", {
            "username": "vinculado",
            "password": "123",
            "device_id": ps4_a,
        })
        self.assertEqual(status, 200)
        tok_a2 = json.loads(la2_raw)["token"]

        # Token copiado de Console A usado no Console B (outro X-Device-Id) é rejeitado
        # sem derrubar o Console A legítimo
        status, _, _ = self.request("GET", "/api/app/me", headers={"X-App-Token": tok_a2, "X-Device-Id": ps4_b})
        self.assertEqual(status, 401)
        status, _, _ = self.request("GET", "/api/app/me", headers={"X-App-Token": tok_a2, "X-Device-Id": ps4_a})
        self.assertEqual(status, 200)

        # 3. Admin desvincula PS4: sessão antiga cai e Console B pode assumir a conta
        status, _, unbind_raw = self.request("POST", f"/api/admin/users/{uid}/unbind")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(unbind_raw)["user"]["bound_device_id"], "")

        status, _, _ = self.request("GET", "/api/app/me", headers={"X-App-Token": tok_a2, "X-Device-Id": ps4_a})
        self.assertEqual(status, 401)

        status, _, lb_ok = self.request("POST", "/api/app/login", {
            "username": "vinculado",
            "password": "123",
            "device_id": ps4_b,
        })
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(lb_ok)["user"]["bound_device_id"], ps4_b)

        # Agora Console A passa a ser bloqueado
        status, _, _ = self.request("POST", "/api/app/login", {
            "username": "vinculado",
            "password": "123",
            "device_id": ps4_a,
        })
        self.assertEqual(status, 403)

        # 4. Usuário com bind_device = 0 permite alternar aparelhos livremente
        status, _, uf_raw = self.request("POST", "/api/admin/users", {
            "username": "livre",
            "password": "123",
            "bind_device": 0,
        })
        self.assertEqual(status, 201)
        self.assertFalse(json.loads(uf_raw)["user"]["effective_bind_device"])
        status, _, _ = self.request("POST", "/api/app/login", {"username": "livre", "password": "123", "device_id": ps4_a})
        self.assertEqual(status, 200)
        status, _, _ = self.request("POST", "/api/app/login", {"username": "livre", "password": "123", "device_id": ps4_b})
        self.assertEqual(status, 200)

    def test_12_device_code_activation_flow(self):
        ps4_1 = "PS4-CC:DD:EE:11:22:33"
        ps4_2 = "PS4-99:88:77:66:55:44"

        # 1. Ativa exigência de autorização no PS4
        status, _, _ = self.request("PUT", "/api/admin/settings", {
            "store_name": "WILLGAME Store",
            "store_subtitle": "Teste",
            "require_app_login": "1",
        })
        self.assertEqual(status, 200)

        # 2. PS4_1 solicita código de ativação
        status, _, req_raw = self.request("POST", "/api/app/device-code/request", {
            "device_id": ps4_1,
        })
        self.assertEqual(status, 201)
        req_data = json.loads(req_raw)
        disp_code = req_data["code"]
        raw_code = req_data["raw_code"]
        self.assertIn("-", disp_code)
        self.assertEqual(len(raw_code), 6)

        # Outro console NÃO pode consultar ou roubar o código do PS4_1
        status, _, _ = self.request("GET", f"/api/app/device-code/poll?code={disp_code}", headers={"X-Device-Id": ps4_2})
        self.assertEqual(status, 403)

        # PS4_1 consulta enquanto pendente
        status, _, poll_p = self.request("GET", f"/api/app/device-code/poll?code={disp_code}", headers={"X-Device-Id": ps4_1})
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(poll_p)["status"], "pending")

        # Painel lista solicitações pendentes
        status, _, pend_raw = self.request("GET", "/api/admin/device-codes")
        self.assertEqual(status, 200)
        codes_list = json.loads(pend_raw)["codes"]
        self.assertEqual(len(codes_list), 1)
        self.assertEqual(codes_list[0]["device_id"], ps4_1)

        # 3. Admin autoriza o código criando cliente vinculado ao PS4_1
        status, _, auth_raw = self.request("POST", f"/api/admin/device-codes/{raw_code}/authorize", {
            "display_name": "Cliente Lucas",
            "expires_days": 30,
            "notes": "PS4 Pro Quarto",
        })
        self.assertEqual(status, 200)
        auth_user = json.loads(auth_raw)["user"]
        self.assertEqual(auth_user["bound_device_id"], ps4_1)
        self.assertGreater(auth_user["expires_at"], 0)

        # PS4_1 faz polling e recebe o token definitivo
        status, _, poll_ok = self.request("GET", f"/api/app/device-code/poll?code={disp_code}", headers={"X-Device-Id": ps4_1})
        self.assertEqual(status, 200)
        p_ok = json.loads(poll_ok)
        self.assertEqual(p_ok["status"], "authorized")
        tok = p_ok["token"]
        self.assertTrue(tok)
        self.assertEqual(p_ok["user"]["display_name"], "Cliente Lucas")

        # Replay do mesmo código consumido falha (410)
        status, _, _ = self.request("GET", f"/api/app/device-code/poll?code={disp_code}", headers={"X-Device-Id": ps4_1})
        self.assertEqual(status, 410)

        # Token libera catálogo apenas para PS4_1
        status, _, _ = self.request("GET", "/api/catalog.json", headers={"X-App-Token": tok, "X-Device-Id": ps4_2})
        self.assertEqual(status, 401)
        status, _, _ = self.request("GET", "/api/catalog.json", headers={"X-App-Token": tok, "X-Device-Id": ps4_1})
        self.assertEqual(status, 200)

        # 4. Novo código do PS4_1 detecta usuário já vinculado e recusa no PS4_2 funciona
        status, _, req2_raw = self.request("POST", "/api/app/device-code/request", {"device_id": ps4_2})
        self.assertEqual(status, 201)
        code2 = json.loads(req2_raw)["raw_code"]
        status, _, _ = self.request("POST", f"/api/admin/device-codes/{code2}/reject")
        self.assertEqual(status, 200)
        status, _, rej_poll = self.request("GET", f"/api/app/device-code/poll?code={code2}", headers={"X-Device-Id": ps4_2})
        self.assertEqual(status, 403)
        self.assertEqual(json.loads(rej_poll)["status"], "rejected")


if __name__ == "__main__":
    unittest.main()
