#!/usr/bin/env python3
"""Inspect real extracted PKG contents; a GP4 manifest is not proof of inclusion."""
import argparse, hashlib, struct, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SERVICE_FILES=('compat-jb.prx','daemon.sfo','eboot.bin','sce_module/libc.prx','sce_module/libSceFios2.prx','GOLDHEN-SDK-LICENSE.txt','ITEMZFLOW-LICENSE.txt','cacert.pem','TLS-LICENSE.txt','TLS-PROVENANCE.txt')
def verify_installer_self(data):
    if len(data)<32 or data[:4]!=b'\x4f\x15\x3d\x1d':
        raise RuntimeError('Invalid application SELF header')
    header_size,meta_size=struct.unpack_from('<HH',data,12)
    if meta_size<256 or header_size+meta_size>len(data):
        raise RuntimeError('Truncated application SELF metadata')
    auth=bytes.fromhex((ROOT/'tools/installer-auth.hex').read_text().strip())
    if len(auth)!=136:
        raise RuntimeError('Invalid installer permission descriptor size')
    entries=struct.unpack_from('<H',data,24)[0]
    elf=32+entries*32
    if elf+64>header_size or data[elf:elf+4]!=b'\x7fELF':
        raise RuntimeError('Missing application ELF header')
    count=struct.unpack_from('<H',data,elf+56)[0]
    extended=(elf+64+count*56+15)&~15
    if extended+64>header_size:
        raise RuntimeError('Truncated application SELF extended header')
    paid,kind=struct.unpack_from('<QQ',data,extended)
    if paid!=0x3100000000000002 or kind!=1:
        raise RuntimeError('Application SELF does not use the installer homebrew profile')
    expected=(struct.pack('<QQ',len(auth),paid)+auth[8:]).ljust(256,b'\0')
    signature=data[header_size+meta_size-256:header_size+meta_size]
    if signature!=expected:
        raise RuntimeError('Application SELF is missing its installer permissions')
    print('Installer permissions verified in the extracted application executable.')
def verify_daemon_self(data):
    if len(data)<32 or data[:4]!=b'\x4f\x15\x3d\x1d':raise RuntimeError('Invalid service SELF header')
    entries=struct.unpack_from('<H',data,24)[0];elf=32+entries*32
    if elf+64>len(data) or data[elf:elf+4]!=b'\x7fELF':raise RuntimeError('Missing embedded service ELF header')
    count=struct.unpack_from('<H',data,elf+56)[0];extended=(elf+64+count*56+15)&~15
    if extended+64>len(data):raise RuntimeError('Truncated service SELF extended header')
    paid,kind=struct.unpack_from('<QQ',data,extended)
    if kind!=1 or paid!=0x3100000000000002:raise RuntimeError(f'Service must use homebrew fake SELF type and daemon PAID, got type={kind:#x} paid={paid:#x}')
    print('Daemon SELF verified: fake program type and daemon authentication ID.')
def verify(folder,catalog=False):
    for name in SERVICE_FILES:
        relative=Path('assets/service')/name
        actual=folder/relative
        expected=ROOT/relative
        if not actual.is_file():raise RuntimeError(f'Missing from extracted PKG: {relative}')
        if hashlib.sha256(actual.read_bytes()).digest()!=hashlib.sha256(expected.read_bytes()).digest():raise RuntimeError(f'PKG payload differs from source: {relative}')
    if catalog:
        for name in ('eboot.bin','assets/catalog.tsv','assets/covers.pack','assets/covers.idx','assets/font.ttf','sce_module/libc.prx','sce_module/libSceFios2.prx'):
            actual=folder/name;expected=ROOT/name
            if not actual.is_file() or hashlib.sha256(actual.read_bytes()).digest()!=hashlib.sha256(expected.read_bytes()).digest():raise RuntimeError(f'Catalogue payload differs from source: {name}')
        print('Catalogue payload verified: boot executable, packed artwork, index, metadata, font and runtime libraries match.')
        verify_installer_self((folder/'eboot.bin').read_bytes())
    sfo=(folder/'assets/service/daemon.sfo').read_bytes()
    if sfo[:4]!=b'\x00PSF' or b'PRTS00001\0' not in sfo:raise RuntimeError('Invalid bundled service SFO')
    verify_daemon_self((folder/'assets/service/eboot.bin').read_bytes())
    print('Extracted PKG verified: service SFO, executable, both runtime libraries and license match source files.')
def main():
    p=argparse.ArgumentParser();p.add_argument('--catalog',action='store_true');p.add_argument('package',type=Path);p.add_argument('--tool',type=Path,required=True);args=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='portal-pkg-verify-') as temporary:
        folder=Path(temporary)
        result=subprocess.run([str(args.tool.resolve()),'pkg_extract',str(args.package.resolve()),str(folder)],capture_output=True,text=True,timeout=90)
        if result.returncode:raise RuntimeError('Package extraction failed: '+(result.stderr or result.stdout)[-1500:])
        # LibOrbisPkg extracts the inner PFS below uroot.
        verify(folder/'uroot' if (folder/'uroot').is_dir() else folder,args.catalog)
if __name__=='__main__':main()
