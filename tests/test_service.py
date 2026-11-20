"""Independent service/client process tests using generated payloads and local peers."""
import os, socket, struct, subprocess, tempfile, time, unittest
from pathlib import Path
import test_torrent_transfer as transfer_fixture
ROOT=transfer_fixture.ROOT

def u32(n):return struct.pack('>I',n)
def string(s):
    b=s.encode() if isinstance(s,str) else s;return u32(len(b))+b
def exact(sock,n):
    out=b''
    while len(out)<n:
        b=sock.recv(n-len(out))
        if not b:raise EOFError
        out+=b
    return out
class Read:
    def __init__(self,b):self.b=b;self.p=0
    def n(self):v=struct.unpack_from('>I',self.b,self.p)[0];self.p+=4;return v
    def q(self):v=struct.unpack_from('>Q',self.b,self.p)[0];self.p+=8;return v
    def s(self):n=self.n();v=self.b[self.p:self.p+n].decode();self.p+=n;return v

def rpc(port,body):
    with socket.create_connection(('127.0.0.1',port),timeout=2) as sock:
        sock.sendall(u32(len(body))+body);data=exact(sock,struct.unpack('>I',exact(sock,4))[0]);r=Read(data);code=r.n()
        if code:raise RuntimeError(r.s())
        return r

def queue(port):
    r=rpc(port,u32(2));rows=[]
    for _ in range(r.n()):
        j={'id':r.n(),'game':r.n(),'state':r.n()}
        for name in ('title','url','page','phase','error','outputDir'):j[name]=r.s()
        j.update(done=r.q(),total=r.q(),rate=r.q(),peers=r.n());rows.append(j)
    return rows
class ServiceTests(unittest.TestCase):
    def fixture(self,root,port):
        # Reuse the generated-content peer fixture without running other test methods.
        helper=transfer_fixture.Transfers();self.addCleanup(helper.doCleanups)
        meta,tracker,peer,payload=helper.setup_transfer(root);peer.block_delay=.18
        env=os.environ.copy();env.update(PORTAL_TEST_META=str(meta),PORTAL_TEST_TRACKER=str(tracker))
        app=root/'app';app.mkdir();log=(root/'service.log').open('wb');self.addCleanup(log.close)
        process=subprocess.Popen([str(ROOT/'build/portal-service-host'),str(app),str(port)],env=env,stdout=log,stderr=log)
        def stop():
            if process.poll() is None:
                process.terminate()
                try:process.wait(timeout=8)
                except subprocess.TimeoutExpired:process.kill();process.wait()
        self.addCleanup(stop)
        for _ in range(150):
            if process.poll() is not None:self.fail((root/'service.log').read_text())
            try:r=rpc(port,u32(1));self.assertEqual(r.n(),8);self.assertEqual(r.s(),'1.0');pid=r.n();break
            except OSError:time.sleep(.02)
        else:self.fail('Service did not start')
        return app,process,peer,payload,pid
    def test_transfer_continues_after_client_process_exits(self):
        # Actual C++ client uses the default loopback port used on PS4.
        with tempfile.TemporaryDirectory() as d:
            root=Path(d).resolve();app,service,peer,payload,pid=self.fixture(root,17941)
            result=subprocess.run([str(ROOT/'build/service-client-driver'),str(app),'enqueue'],capture_output=True,text=True,timeout=12)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            target=app/'downloads/1/generated-sample.bin'
            for _ in range(30):
                if target.exists():break
                time.sleep(.01)
            self.assertTrue(target.exists());self.assertLess(target.stat().st_size,len(payload))
            # No UI client is alive or connected during this interval.
            time.sleep(2)
            self.assertIsNone(service.poll());self.assertEqual(target.read_bytes(),payload)
            fresh=subprocess.run([str(ROOT/'build/service-client-driver'),str(app),'snapshot'],capture_output=True,text=True,timeout=10)
            self.assertEqual(fresh.returncode,0,fresh.stderr);self.assertIn('Complete',fresh.stdout)
            r=rpc(17941,u32(1));r.n();r.s();self.assertEqual(r.n(),pid)
    def test_optional_controller_uses_existing_service(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d).resolve();app,service,peer,payload,pid=self.fixture(root,17941)
            result=subprocess.run([str(ROOT/'build/controller-driver'),str(app),str(root/'sample.torrent'),str(root/'tracker.bencode'),'enqueue-exit'],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertIn('Background service connected',result.stdout);time.sleep(2)
            self.assertEqual((app/'downloads/1/generated-sample.bin').read_bytes(),payload);self.assertIsNone(service.poll())
    def test_downloads_work_without_installed_service(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d).resolve();helper=transfer_fixture.Transfers();self.addCleanup(helper.doCleanups);meta,tracker,peer,payload=helper.setup_transfer(root)
            result=subprocess.run([str(ROOT/'build/controller-driver'),str(root/'foreground'),str(meta),str(tracker),'finish'],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertIn('In-app downloads',result.stdout);self.assertEqual((root/'foreground/downloads/1/generated-sample.bin').read_bytes(),payload)
    def test_failed_background_setup_returns_to_in_app_downloads(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d).resolve();helper=transfer_fixture.Transfers();self.addCleanup(helper.doCleanups);meta,tracker,peer,payload=helper.setup_transfer(root);peer.block_delay=.1
            result=subprocess.run([str(ROOT/'build/controller-driver'),str(root/'foreground'),str(meta),str(tracker),'setup-failure'],capture_output=True,text=True,timeout=25)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertIn('In-app downloads',result.stdout);self.assertEqual((root/'foreground/downloads/1/generated-sample.bin').read_bytes(),payload)
    def test_exclusive_owner_and_invalid_protocol(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d).resolve()
            with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
            app,service,peer,payload,pid=self.fixture(root,port)
            duplicate=subprocess.run([str(ROOT/'build/portal-service-host'),str(app),str(port+1)],capture_output=True,text=True,timeout=8)
            self.assertEqual(duplicate.returncode,2)
            with self.assertRaises(RuntimeError):rpc(port,u32(999))
            with self.assertRaises(RuntimeError):rpc(port,u32(1)+b'junk')
            for body in (b'',b'x',u32(0x7fffffff)):
                with socket.create_connection(('127.0.0.1',port),timeout=2) as client:
                    client.sendall(body if len(body)==4 else u32(len(body))+body);client.shutdown(socket.SHUT_WR)
                    try:client.recv(1024)
                    except ConnectionResetError:pass
            self.assertIsNone(service.poll());self.assertEqual(rpc(port,u32(1)).n(),8)
            rpc(port,u32(9));self.assertEqual(service.wait(timeout=8),0)
    def test_pause_resume_commands_are_idempotent(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d).resolve()
            with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
            app,service,peer,payload,pid=self.fixture(root,port)
            game=u32(773)+b''.join(string(s) for s in ('Generated test','','','https://ps4torrent.net/773-test.html',''))
            rpc(port,u32(5)+game+string('generated.torrent')+string('https://ps4torrent.net/index.php?do=download&id=775')+string((root/'sample.torrent').read_bytes()))
            rpc(port,u32(6)+u32(1));rpc(port,u32(6)+u32(1))
            for _ in range(100):
                state=queue(port)[0]['state']
                if state==3:break
                time.sleep(.02)
            self.assertEqual(state,3)
            rpc(port,u32(7)+u32(1));rpc(port,u32(7)+u32(1))
            self.assertIn(queue(port)[0]['state'],(0,1,4))
            rpc(port,u32(8)+u32(1));self.assertEqual(queue(port)[0]['state'],6)
            rpc(port,u32(10)+u32(1));self.assertEqual(queue(port),[])
            rpc(port,u32(5)+game+string('generated.torrent')+string('https://ps4torrent.net/index.php?do=download&id=775')+string((root/'sample.torrent').read_bytes()))
            self.assertEqual(queue(port)[0]['id'],1)
            rpc(port,u32(9));self.assertEqual(service.wait(timeout=8),0)
if __name__=='__main__':unittest.main()
