#!/usr/bin/env python3
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PKG_LOCK = ROOT / "build/.pkg-build.lock"
CID = "IV0000-WGST00001_00-HBSTORE000000000"


def build_package(project, output, tool):
    import fcntl

    project = Path(project).resolve()
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    PKG_LOCK.parent.mkdir(parents=True, exist_ok=True)
    with PKG_LOCK.open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        with tempfile.TemporaryDirectory(prefix="pkg-stage-", dir=output) as temporary:
            subprocess.run([str(Path(tool).resolve()), "pkg_build", str(project), temporary], cwd=project.parent, check=True)
            packages = list(Path(temporary).glob("*.pkg"))
            if len(packages) != 1:
                raise RuntimeError("Expected one completed package")
            packages[0].replace(output / packages[0].name)


def build():
    build_package(ROOT / "pkg.gp4", ROOT / "build", sys.argv[2])


def sfo(title="WILLGAME Store", title_id="WGST00001"):
    entries = {
        "APP_TYPE": 1,
        "APP_VER": "1.00",
        # Bit 1 (0x02): Enter Button Assignment for common dialogs (OSK/IME) = Cross (X).
        # Keeping bit 5 (0x20) cleared prevents regional fallback to Circle-as-Enter.
        "ATTRIBUTE": 0x00000002,
        "CATEGORY": "gd",
        "CONTENT_ID": CID,
        "DOWNLOAD_DATA_SIZE": 0,
        "SYSTEM_VER": 0,
        "TITLE": title,
        "TITLE_ID": title_id,
        "VERSION": "1.00",
    }
    caps = {"APP_VER": 8, "VERSION": 8, "CATEGORY": 4, "CONTENT_ID": 48, "TITLE": 128, "TITLE_ID": 12}
    keys = bytearray()
    data = bytearray()
    rows = []
    for k, v in sorted(entries.items()):
        ko = len(keys)
        keys.extend(k.encode() + b"\0")
        off = len(data)
        if isinstance(v, int):
            raw = struct.pack("<I", v)
            cap = 4
            fmt = 0x404
        else:
            raw = v.encode() + b"\0"
            cap = caps[k]
            fmt = 0x204
        rows.append((ko, fmt, len(raw), cap, off))
        data.extend(raw + b"\0" * (cap - len(raw)))
        data.extend(b"\0" * ((-len(data)) % 4))
    keyoff = 20 + 16 * len(rows)
    dataoff = (keyoff + len(keys) + 3) & ~3
    header = struct.pack("<5I", 0x46535000, 0x101, keyoff, dataoff, len(rows))
    result = header + b"".join(struct.pack("<HHIII", *r) for r in rows) + keys
    result += b"\0" * (dataoff - len(result)) + data
    (ROOT / "sce_sys/param.sfo").write_bytes(result)


def gp4():
    root = ET.Element("psproject", fmt="gp4", version="1000")
    v = ET.SubElement(root, "volume")
    ET.SubElement(v, "volume_type").text = "pkg_ps4_app"
    ET.SubElement(v, "volume_id").text = "PS4VOLUME"
    ET.SubElement(v, "volume_ts").text = "2026-11-19 00:00:00"
    ET.SubElement(
        v,
        "package",
        content_id=CID,
        passcode="00000000000000000000000000000000",
        storage_type="digital50",
        app_type="full",
        app_path="",
    )
    files = ET.SubElement(root, "files", img_no="0")
    paths = [ROOT / "eboot.bin"]
    for folder in ("assets", "sce_sys", "sce_module"):
        folder_path = ROOT / folder
        if not folder_path.exists():
            continue
        paths.extend(
            p
            for p in folder_path.rglob("*")
            if p.is_file()
            and not p.name.startswith(".")
            and p.stat().st_size > 0
            and p.name not in ("store-icon.png", "server.txt")
        )
    for p in paths:
        name = p.relative_to(ROOT).as_posix()
        if p.name == "param.sfo" and name != "sce_sys/param.sfo":
            raise RuntimeError(f"Nested param.sfo is omitted by LibOrbisPkg; rename this asset: {name}")
    for p in sorted(paths):
        name = p.relative_to(ROOT).as_posix()
        ET.SubElement(files, "file", targ_path=name, orig_path=name)
    rd = ET.SubElement(root, "rootdir")
    dirs = {}
    for p in sorted(paths):
        parent = rd
        parts = p.relative_to(ROOT).parts[:-1]
        prefix = ""
        for part in parts:
            prefix += "/" + part
            if prefix not in dirs:
                dirs[prefix] = ET.SubElement(parent, "dir", targ_name=part)
            parent = dirs[prefix]
    ET.indent(root)
    ET.ElementTree(root).write(ROOT / "pkg.gp4", encoding="utf-8", xml_declaration=True)


if __name__ == "__main__":
    {"sfo": sfo, "gp4": gp4, "build": build}[sys.argv[1]]()
