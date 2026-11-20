#!/usr/bin/env python3
"""Generate our own daemon SFO; never copies a system application's metadata."""
import struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    values={'ATTRIBUTE':0,'CATEGORY':'gdd','CONTENT_ID':'','FORMAT':'ngh','TITLE':'NULL_store Download Service','TITLE_ID':'PRTS00001','VERSION':'1.00'}
    caps={'CATEGORY':4,'CONTENT_ID':48,'FORMAT':4,'TITLE':128,'TITLE_ID':12,'VERSION':8}
    keys=bytearray();data=bytearray();rows=[]
    for k,v in sorted(values.items()):
        ko=len(keys);keys+=k.encode()+b'\0';offset=len(data)
        if isinstance(v,int):raw=struct.pack('<I',v);cap=4;fmt=0x404
        else:raw=v.encode()+b'\0';cap=caps[k];fmt=0x204
        rows.append((ko,fmt,len(raw),cap,offset));data+=raw+b'\0'*(cap-len(raw));data+=b'\0'*((-len(data))%4)
    ko=20+16*len(rows);do=(ko+len(keys)+3)&~3
    result=struct.pack('<5I',0x46535000,0x101,ko,do,len(rows))+b''.join(struct.pack('<HHIII',*r) for r in rows)+keys
    result+=b'\0'*(do-len(result))+data
    (ROOT/'assets/service/daemon.sfo').write_bytes(result)
    # LibOrbisPkg omits nested assets named param.sfo; only use it at install time.
    (ROOT/'assets/service/sce_sys/param.sfo').unlink(missing_ok=True)
    (ROOT/'assets/service/sce_module').mkdir(parents=True,exist_ok=True)
    for name in ('libc.prx','libSceFios2.prx'):
        (ROOT/'assets/service/sce_module'/name).write_bytes((ROOT/'sce_module'/name).read_bytes())
if __name__=='__main__':main()
