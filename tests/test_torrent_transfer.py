"""Local generated content only: no portal games, public peers, or public trackers."""
import hashlib, os, socket, socketserver, struct, subprocess, tempfile, threading, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def enc(x):
    if isinstance(x,int): return b'i'+str(x).encode()+b'e'
    if isinstance(x,str): x=x.encode()
    if isinstance(x,bytes): return str(len(x)).encode()+b':'+x
    if isinstance(x,list): return b'l'+b''.join(map(enc,x))+b'e'
    return b'd'+b''.join(enc(k)+enc(v) for k,v in sorted(x.items()))+b'e'
def exact(sock,n):
    data=b''
    while len(data)<n:
        chunk=sock.recv(n-len(data))
        if not chunk: raise EOFError
        data+=chunk
    return data
class Peer(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            self.request.settimeout(5);hs=exact(self.request,68);self.server.connections+=1
            if hs[28:48]!=self.server.ihash:return
            self.request.sendall(hs[:48]+b'-TEST00-'+b'123456789012')
            n=(len(self.server.payload)+self.server.plen-1)//self.server.plen
            bf=bytes([255])*((n+7)//8)
            self.request.sendall(struct.pack('>I',len(bf)+1)+b'\x05'+bf+struct.pack('>IB',1,1))
            while True:
                size=struct.unpack('>I',exact(self.request,4))[0];message=exact(self.request,size)
                if message and message[0]==6:
                    piece,offset,length=struct.unpack('>III',message[1:]);start=piece*self.server.plen+offset
                    self.server.requested.append(piece)
                    if getattr(self.server,'block_delay',0):
                        import time;time.sleep(self.server.block_delay)
                    data=self.server.payload[start:start+length]
                    if self.server.corrupt:
                        self.server.corrupt=False;data=bytes([data[0]^1])+data[1:]
                    packet=struct.pack('>I',len(data)+9)+b'\x07'+struct.pack('>II',piece,offset)+data
                    self.request.sendall(packet)
        except (EOFError,OSError):pass
class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address=True;daemon_threads=True
    def handle_error(self,*a):pass
class Transfers(unittest.TestCase):
    def setup_transfer(self,folder,multi=False,udp=False,corrupt=False,dht=False):
        payload=bytes((i*17+i//11)%256 for i in range(100123));plen=32768
        info={'name':'generated-sample' if multi else 'generated-sample.bin','piece length':plen,'pieces':b''.join(hashlib.sha1(payload[i:i+plen]).digest() for i in range(0,len(payload),plen))}
        if multi:info['files']=[{'length':19001,'path':['one.bin']},{'length':len(payload)-19001,'path':['nested','two.bin']}]
        else:info['length']=len(payload)
        server=Server(('127.0.0.1',0),Peer);server.payload=payload;server.plen=plen;server.ihash=hashlib.sha1(enc(info)).digest();server.connections=0;server.corrupt=corrupt;server.requested=[]
        threading.Thread(target=server.serve_forever,daemon=True).start();self.addCleanup(server.server_close);self.addCleanup(server.shutdown)
        compact=socket.inet_aton('127.0.0.1')+struct.pack('>H',server.server_address[1]);tracker=folder/'tracker.bencode';tracker.write_bytes(enc({'interval':60,'peers':compact}))
        announce='http://127.0.0.1/announce'
        if udp:
            sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);sock.bind(('127.0.0.1',0));sock.settimeout(2);self.addCleanup(sock.close)
            announce=f'udp://127.0.0.1:{sock.getsockname()[1]}/announce'
            def serve():
                try:
                    req,addr=sock.recvfrom(2048);sock.sendto(struct.pack('>IIQ',0,struct.unpack('>I',req[12:16])[0],12345),addr)
                    req,addr=sock.recvfrom(2048);self.assertEqual(struct.unpack('>I',req[80:84])[0],2);sock.sendto(struct.pack('>IIIII',1,struct.unpack('>I',req[12:16])[0],60,1,1)+compact,addr)
                except OSError:pass
            threading.Thread(target=serve,daemon=True).start()
        metadata={'announce':announce,'info':info}
        if dht:
            nodes=[]
            for _ in range(2):
                sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);sock.bind(('127.0.0.1',0));sock.settimeout(4);self.addCleanup(sock.close);nodes.append(sock)
            def respond(sock,index):
                try:
                    req,addr=sock.recvfrom(4096)
                    self.assertIn(b'1:q9:get_peers2:roi1e',req)
                    self.assertIn(server.ihash,req)
                    tx=req[req.index(b'1:t4:')+5:][:4]
                    response={'id':bytes([index+1])*20,'token':b'local-token'}
                    if index==0: response['nodes']=b'2'*20+socket.inet_aton('127.0.0.1')+struct.pack('>H',nodes[1].getsockname()[1])
                    else: response['values']=[compact,compact] # duplicate peer must be ignored
                    sock.sendto(enc({'r':response,'t':tx,'y':'r'}),addr)
                except OSError:pass
            for index,sock in enumerate(nodes):threading.Thread(target=respond,args=(sock,index),daemon=True).start()
            metadata={'info':info,'nodes':[['127.0.0.1',nodes[0].getsockname()[1]]]}
        meta=folder/'sample.torrent';meta.write_bytes(enc(metadata));return meta,tracker,server,payload
    def run_engine(self,meta,dest,tracker,stop=0):
        return subprocess.run([str(ROOT/'build/torrent_driver'),str(meta),str(dest),str(tracker),str(stop)],capture_output=True,text=True,timeout=25)
    def test_http_tracker_multifile_and_hash_retry(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d).resolve();meta,tr,server,payload=self.setup_transfer(p,multi=True,corrupt=True);r=self.run_engine(meta,p/'out',tr)
            self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertGreater(server.requested.count(0),2)
            self.assertEqual((p/'out/generated-sample/one.bin').read_bytes()+(p/'out/generated-sample/nested/two.bin').read_bytes(),payload)
    def test_pause_resume_and_completed_recheck(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d).resolve();meta,tr,server,payload=self.setup_transfer(p);r=self.run_engine(meta,p/'out',tr,32768);self.assertEqual(r.returncode,2,r.stdout+r.stderr)
            self.assertEqual((p/'out/generated-sample.bin').read_bytes(),payload[:32768]);server.requested.clear()
            r=self.run_engine(meta,p/'out',tr);self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertNotIn(0,server.requested);self.assertEqual((p/'out/generated-sample.bin').read_bytes(),payload)
            before=server.connections;r=self.run_engine(meta,p/'out',tr);self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertEqual(before,server.connections)
    def test_udp_tracker(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d).resolve();meta,tr,server,payload=self.setup_transfer(p,udp=True);r=self.run_engine(meta,p/'out',tr);self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertEqual((p/'out/generated-sample.bin').read_bytes(),payload)
    def test_trackerless_dht_iterative_lookup(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d).resolve();meta,tr,server,payload=self.setup_transfer(p,dht=True);r=self.run_engine(meta,p/'out',tr);self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertEqual((p/'out/generated-sample.bin').read_bytes(),payload)
    def test_app_kill_reopen_automatically_resumes(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d).resolve();meta,tr,server,payload=self.setup_transfer(p);server.block_delay=.025
            command=[str(ROOT/'build/queue_driver'),str(p/'app-data'),str(meta),str(tr)]
            crashed=subprocess.run(command+['crash'],capture_output=True,text=True,timeout=25);self.assertEqual(crashed.returncode,0,crashed.stdout+crashed.stderr)
            partial=p/'app-data/downloads/1/generated-sample.bin';self.assertTrue(partial.exists());self.assertGreaterEqual(partial.stat().st_size,32768);self.assertLess(partial.stat().st_size,len(payload));server.requested.clear()
            resumed=subprocess.run(command+['resume'],capture_output=True,text=True,timeout=25);self.assertEqual(resumed.returncode,0,resumed.stdout+resumed.stderr);self.assertEqual(partial.read_bytes(),payload);self.assertNotIn(0,server.requested)
    def test_app_graceful_close_reopen_automatically_resumes(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d).resolve();meta,tr,server,payload=self.setup_transfer(p);server.block_delay=.025
            command=[str(ROOT/'build/queue_driver'),str(p/'app-data'),str(meta),str(tr)]
            crashed=subprocess.run(command+['close'],capture_output=True,text=True,timeout=25);self.assertEqual(crashed.returncode,0,crashed.stdout+crashed.stderr)
            partial=p/'app-data/downloads/1/generated-sample.bin';self.assertTrue(partial.exists());self.assertGreaterEqual(partial.stat().st_size,32768);self.assertLess(partial.stat().st_size,len(payload));server.requested.clear()
            resumed=subprocess.run(command+['resume'],capture_output=True,text=True,timeout=25);self.assertEqual(resumed.returncode,0,resumed.stdout+resumed.stderr);self.assertEqual(partial.read_bytes(),payload);self.assertNotIn(0,server.requested)
    def test_rejects_unsafe_paths_and_bad_sizes(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d).resolve();meta,tr,server,payload=self.setup_transfer(p)
            for info in [{'name':'../escape','length':1,'piece length':1,'pieces':b'0'*20},{'name':'safe','length':100,'piece length':1,'pieces':b'0'*20},{'name':'safe','files':[{'path':['..','escape'],'length':1}],'piece length':1,'pieces':b'0'*20}]:
                meta.write_bytes(enc({'announce':'http://127.0.0.1/announce','info':info}));r=self.run_engine(meta,p/'out',tr);self.assertEqual(r.returncode,1);self.assertFalse((p/'out').exists())
if __name__=='__main__':unittest.main()
