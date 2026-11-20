#!/usr/bin/env python3
"""Link on local storage, then durably publish ELF to the external workspace."""
import hashlib,os,subprocess,sys,tempfile
from pathlib import Path
args=sys.argv[1:];at=args.index('-o')+1;target=Path(args[at])
with tempfile.TemporaryDirectory(prefix='nullstore-link-',dir='/tmp') as folder:
    args[at]=str(Path(folder)/'output.elf')
    subprocess.run(args,check=True)
    data=Path(args[at]).read_bytes()
    if data[:4]!=b'\x7fELF':raise RuntimeError('Linker did not produce a valid ELF')
    temp=target.with_suffix(target.suffix+'.publishing')
    with temp.open('wb') as output:output.write(data);output.flush();os.fsync(output.fileno())
    os.replace(temp,target)
    if hashlib.sha256(target.read_bytes()).digest()!=hashlib.sha256(data).digest():raise RuntimeError('ELF changed while publishing to workspace')
