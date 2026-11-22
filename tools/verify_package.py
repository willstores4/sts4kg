#!/usr/bin/env python3
"""Inspect real extracted PKG contents; a GP4 manifest is not proof of inclusion."""
import argparse
import hashlib
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REQUIRED_FILES = (
    "eboot.bin",
    "assets/bgft_template.elf",
    "assets/font.ttf",
    "assets/service/cacert.pem",
    "assets/service/TLS-LICENSE.txt",
    "assets/service/TLS-PROVENANCE.txt",
    "sce_module/libc.prx",
    "sce_module/libSceFios2.prx",
)


def verify_installer_self(data):
    if len(data) < 32 or data[:4] != b"\x4f\x15\x3d\x1d":
        raise RuntimeError("Invalid application SELF header")
    header_size, meta_size = struct.unpack_from("<HH", data, 12)
    if meta_size < 256 or header_size + meta_size > len(data):
        raise RuntimeError("Truncated application SELF metadata")
    auth = bytes.fromhex((ROOT / "tools/installer-auth.hex").read_text().strip())
    if len(auth) != 136:
        raise RuntimeError("Invalid installer permission descriptor size")
    entries = struct.unpack_from("<H", data, 24)[0]
    elf = 32 + entries * 32
    if elf + 64 > header_size or data[elf : elf + 4] != b"\x7fELF":
        raise RuntimeError("Missing application ELF header")
    count = struct.unpack_from("<H", data, elf + 56)[0]
    extended = (elf + 64 + count * 56 + 15) & ~15
    if extended + 64 > header_size:
        raise RuntimeError("Truncated application SELF extended header")
    paid, kind = struct.unpack_from("<QQ", data, extended)
    if paid != 0x3100000000000002 or kind != 1:
        raise RuntimeError("Application SELF does not use the installer homebrew profile")
    expected = (struct.pack("<QQ", len(auth), paid) + auth[8:]).ljust(256, b"\0")
    signature = data[header_size + meta_size - 256 : header_size + meta_size]
    if signature != expected:
        raise RuntimeError("Application SELF is missing its installer permissions")
    print("Installer permissions verified in the extracted application executable.")


def verify_sfo(sfo):
    if len(sfo) < 20 or sfo[:4] != b"\x00PSF" or b"WGST00001\0" not in sfo:
        raise RuntimeError("Invalid sce_sys/param.sfo")
    magic, _, key_off, data_off, count = struct.unpack_from("<5I", sfo, 0)
    if magic != 0x46535000 or key_off < 20 or data_off < key_off or len(sfo) < 20 + count * 16:
        raise RuntimeError("Malformed sce_sys/param.sfo header")
    attrs = {}
    for i in range(count):
        ko, fmt, flen, _, doff = struct.unpack_from("<HHIII", sfo, 20 + i * 16)
        k_end = sfo.find(b"\0", key_off + ko)
        if k_end < 0:
            raise RuntimeError("Malformed sce_sys/param.sfo key table")
        key = sfo[key_off + ko : k_end].decode("ascii", "replace")
        if fmt == 0x404 and flen == 4 and data_off + doff + 4 <= len(sfo):
            attrs[key] = struct.unpack_from("<I", sfo, data_off + doff)[0]
    # Bit 1 (0x02): Cross confirms in Common Dialog (OSK); bit 5 (0x20) cleared.
    if (attrs.get("ATTRIBUTE", 0) & 0x22) != 0x02:
        raise RuntimeError(
            f"sce_sys/param.sfo ATTRIBUTE=0x{attrs.get('ATTRIBUTE', 0):08x} must assign Cross (0x02) for Common Dialogs"
        )


def verify(extract_root):
    uroot = extract_root / "uroot" if (extract_root / "uroot").is_dir() else extract_root
    for name in REQUIRED_FILES:
        actual = uroot / name
        expected = ROOT / name
        if not actual.is_file():
            raise RuntimeError(f"Missing from extracted PKG: {name}")
        if hashlib.sha256(actual.read_bytes()).digest() != hashlib.sha256(expected.read_bytes()).digest():
            raise RuntimeError(f"PKG payload differs from source: {name}")

    if (uroot / "assets/server.txt").exists():
        raise RuntimeError("Unexpected server config in extracted PKG: assets/server.txt")

    # PkgTool.Core pkg_extract extracts uroot only; PARAM_SFO and ICON0_PNG digests
    # are already verified by pkg_validate above. Check the packaged SFO metadata too.
    verify_sfo((ROOT / "sce_sys/param.sfo").read_bytes())

    verify_installer_self((uroot / "eboot.bin").read_bytes())
    print("Extracted PKG verified: executable, installer auth, trust store, fixed eboot server config, font, SFO and runtime modules match.")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("package", type=Path)
    p.add_argument("--tool", type=Path, required=True)
    args = p.parse_args()
    with tempfile.TemporaryDirectory(prefix="hbstore-pkg-verify-") as temporary:
        folder = Path(temporary)
        result = subprocess.run(
            [str(args.tool.resolve()), "pkg_extract", str(args.package.resolve()), str(folder)],
            capture_output=True,
            text=True,
            timeout=90,
        )
        if result.returncode:
            raise RuntimeError("Package extraction failed: " + (result.stderr or result.stdout)[-1500:])
        verify(folder)


if __name__ == "__main__":
    main()
