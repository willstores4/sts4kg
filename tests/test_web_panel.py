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


if __name__ == "__main__":
    unittest.main()
