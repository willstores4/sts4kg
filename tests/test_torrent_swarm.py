"""Multi-peer engine behaviour against local generated content only (127.0.0.1, no public network)."""
import hashlib, os, socket, socketserver, struct, subprocess, tempfile, threading, time, unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit
from test_torrent_transfer import ROOT, enc, exact

PLEN = 32768
PAYLOAD = bytes((i * 17 + i // 11) % 256 for i in range(100123))
NPIECES = (len(PAYLOAD) + PLEN - 1) // PLEN
INFO = {'name': 'generated-sample.bin', 'length': len(PAYLOAD), 'piece length': PLEN,
        'pieces': b''.join(hashlib.sha1(PAYLOAD[i:i + PLEN]).digest() for i in range(0, len(PAYLOAD), PLEN))}
IHASH = hashlib.sha1(enc(INFO)).digest()

def compact(port): return socket.inet_aton('127.0.0.1') + struct.pack('>H', port)
def message(mid, body=b''): return struct.pack('>IB', len(body) + 1, mid) + body
def bitfield(pieces):
    bf = bytearray((NPIECES + 7) // 8)
    for i in pieces: bf[i >> 3] |= 0x80 >> (i & 7)
    return bytes(bf)

def serve_pieces(sock, cfg, stats):
    """After the handshake: advertise, optionally gossip PEX, unchoke after a delay, then answer requests."""
    pieces = cfg.get('pieces', range(NPIECES))
    if pieces: sock.sendall(message(5, bitfield(pieces)))
    if cfg.get('pex') is not None:
        sock.sendall(message(20, b'\x00' + enc({'m': {'ut_pex': 1}, 'v': 'local-test'})))
        sock.sendall(message(20, b'\x01' + enc({'added': cfg['pex'], 'added.f': b'\x00' * (len(cfg['pex']) // 6)})))
    if not pieces:
        while True: exact(sock, struct.unpack('>I', exact(sock, 4))[0])
    time.sleep(cfg.get('unchoke_delay', 0))
    sock.sendall(message(1))
    stats['unchoked_at'] = time.monotonic()
    while True:
        size = struct.unpack('>I', exact(sock, 4))[0]; m = exact(sock, size)
        if m and m[0] == 6:
            piece, offset, length = struct.unpack('>III', m[1:13])
            if piece not in pieces: continue
            start = piece * PLEN + offset; data = PAYLOAD[start:start + length]
            stats['served'] += 1
            sock.sendall(struct.pack('>I', len(data) + 9) + b'\x07' + struct.pack('>II', piece, offset) + data)

class SwarmPeer(socketserver.BaseRequestHandler):
    def handle(self):
        cfg, stats, s = self.server.cfg, self.server.stats, self.request
        try:
            s.settimeout(40); hs = exact(s, 68)
            if cfg.get('mode') == 'garbage': s.sendall(b'\x16\x03\x01' + os.urandom(80)); return
            if hs[28:48] != IHASH: return
            reserved = b'\x00' * 5 + b'\x10' + b'\x00\x00' if cfg.get('pex') is not None else b'\x00' * 8
            s.sendall(b'\x13BitTorrent protocol' + reserved + IHASH + os.urandom(20))
            stats['handshakes'] += 1
            serve_pieces(s, cfg, stats)
        except (EOFError, OSError): pass

class PeerServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True; daemon_threads = True
    def handle_error(self, *a): pass

class Swarm(unittest.TestCase):
    def peer(self, **cfg):
        server = PeerServer(('127.0.0.1', 0), SwarmPeer); server.cfg = cfg
        server.stats = {'served': 0, 'handshakes': 0, 'unchoked_at': None}
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        return server

    def tracker(self, peers=b'', chunked=False, failure=None, on_announce=None):
        calls = []
        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def log_message(self, *a): pass
            def do_GET(self):
                q = parse_qs(urlsplit(self.path).query, keep_blank_values=True); calls.append(q)
                if on_announce: on_announce(q)
                body = enc({'failure reason': failure} if failure else {'interval': 1800, 'complete': 1, 'incomplete': 0, 'peers': peers})
                self.send_response(200); self.send_header('Content-Type', 'text/plain'); self.send_header('Connection', 'close')
                if chunked:
                    self.send_header('Transfer-Encoding', 'chunked'); self.end_headers()
                    for i in range(0, len(body), 7): part = body[i:i + 7]; self.wfile.write(b'%x\r\n%s\r\n' % (len(part), part))
                    self.wfile.write(b'0\r\n\r\n')
                else:
                    self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler); server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        return f'http://127.0.0.1:{server.server_address[1]}/announce', calls

    def engine(self, folder, announce, timeout=60):
        meta = folder / 'sample.torrent'; meta.write_bytes(enc({'announce': announce, 'info': INFO}))
        # The tracker file does not exist, so the app's HTTP callback fails: peers must come from the engine's own client.
        env = dict(os.environ, PORTAL_TORRENT_LOG='1')
        return subprocess.run([str(ROOT / 'build/torrent_driver'), str(meta), str(folder / 'out'), str(folder / 'missing-tracker'), '0'],
                              capture_output=True, text=True, timeout=timeout, env=env)

    def assertLog(self, text, r):
        if text not in r.stderr: self.fail(f'{text!r} not in engine log ({len(r.stderr)} bytes); tail:\n' + r.stderr[-4000:])
        self.assertLess(len(r.stderr), 200000, 'engine log is unexpectedly large')

    def assertComplete(self, folder, r):
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr[-4000:])
        self.assertEqual((folder / 'out/generated-sample.bin').read_bytes(), PAYLOAD)

    def test_slow_unchoke_survives_alongside_broken_peers(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d).resolve()
            closed = socket.socket(); closed.bind(('127.0.0.1', 0)); refused_port = closed.getsockname()[1]; closed.close()
            silent = socket.socket(); silent.bind(('127.0.0.1', 0)); silent.listen(8); self.addCleanup(silent.close)
            garbage = self.peer(mode='garbage')
            slow = self.peer(unchoke_delay=12)  # the old engine gave up on a peer after ~10s
            peers = compact(refused_port) + compact(silent.getsockname()[1]) + compact(garbage.server_address[1]) + compact(slow.server_address[1])
            url, calls = self.tracker(peers, chunked=True)
            began = time.monotonic(); r = self.engine(p, url)
            self.assertComplete(p, r)
            self.assertGreaterEqual(time.monotonic() - began, 12)
            self.assertGreater(slow.stats['served'], 0)
            self.assertTrue(calls and 'info_hash' in calls[0] and calls[0]['compact'] == ['1'] and calls[0]['event'] == ['started'])
            self.assertLog('(direct HTTP): 4 peers listed, 4 new', r)
            self.assertLog('connection refused', r)
            self.assertLog('not a plain BitTorrent handshake', r)
            self.assertLog('unchoked us after 1', r)

    def test_combines_partial_seeds(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d).resolve()
            first = self.peer(pieces=[0, 1]); second = self.peer(pieces=[2, 3])
            url, _ = self.tracker(compact(first.server_address[1]) + compact(second.server_address[1]))
            r = self.engine(p, url); self.assertComplete(p, r)
            self.assertGreater(first.stats['served'], 0); self.assertGreater(second.stats['served'], 0)

    def test_peer_exchange_finds_more_peers(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d).resolve()
            seed = self.peer()
            gossip = self.peer(pieces=[], pex=compact(seed.server_address[1]))
            url, _ = self.tracker(compact(gossip.server_address[1]))
            r = self.engine(p, url); self.assertComplete(p, r)
            self.assertGreater(seed.stats['served'], 0)
            self.assertLog('PEX gave 1 new peers', r)

    def test_accepts_incoming_peer(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d).resolve(); stats = {'served': 0, 'unchoked_at': None}
            def seed_to(port):
                try:
                    s = socket.create_connection(('127.0.0.1', port), timeout=40)
                    s.sendall(b'\x13BitTorrent protocol' + b'\x00' * 8 + IHASH + os.urandom(20))
                    hs = exact(s, 68)
                    if hs[28:48] == IHASH: serve_pieces(s, {}, stats)
                except (EOFError, OSError): pass
            def announce(q):
                if not stats.get('started'):
                    stats['started'] = True; threading.Thread(target=seed_to, args=(int(q['port'][0]),), daemon=True).start()
            url, calls = self.tracker(b'', on_announce=announce)
            r = self.engine(p, url); self.assertComplete(p, r)
            self.assertGreater(stats['served'], 0)
            self.assertLog('Listening for incoming peers on TCP port', r)
            self.assertLog(', incoming', r)

    def test_tracker_rejection_is_reported(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d).resolve(); url, _ = self.tracker(failure='torrent not registered')
            meta = p / 'sample.torrent'; meta.write_bytes(enc({'announce': url, 'info': INFO}))
            proc = subprocess.Popen([str(ROOT / 'build/torrent_driver'), str(meta), str(p / 'out'), str(p / 'missing'), '0'],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, env=dict(os.environ, PORTAL_TORRENT_LOG='1'))
            lines = []; seen = threading.Event()
            def read():
                for line in proc.stderr:
                    lines.append(line)
                    if 'FAILED (rejected: torrent not registered)' in line: seen.set()
            threading.Thread(target=read, daemon=True).start()
            try: self.assertTrue(seen.wait(15), ''.join(lines))
            finally: proc.kill(); proc.wait(); proc.stderr.close()

if __name__ == '__main__': unittest.main()
