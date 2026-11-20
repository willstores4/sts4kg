"""Local delayed/batched peers validate pipelining and speed before verification."""
import hashlib
import os
from pathlib import Path
import socket
import socketserver
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from test_torrent_transfer import ROOT, enc, exact

PAYLOAD = bytes((i * 7 + i // 31) % 256 for i in range(1024 * 1024))


class BatchedPeer(socketserver.BaseRequestHandler):
    def handle(self):
        s = self.request
        pending = []
        try:
            s.settimeout(5)
            hs = exact(s, 68)
            if hs[28:48] != self.server.info_hash:
                return
            s.sendall(hs[:48] + b'-SPEED0-123456789012')
            s.sendall(struct.pack('>IBB', 2, 5, 0xff) + struct.pack('>IB', 1, 1))
            while True:
                size = struct.unpack('>I', exact(s, 4))[0]
                msg = exact(s, size)
                if msg and msg[0] == 6:
                    pending.append(struct.unpack('>III', msg[1:13]))
                    self.server.largest_batch = max(self.server.largest_batch, len(pending))
                    if len(pending) >= self.server.batch:
                        for piece, offset, length in pending:
                            start = piece * self.server.plen + offset
                            data = PAYLOAD[start:start + length]
                            time.sleep(self.server.delay)
                            s.sendall(struct.pack('>IBII', len(data) + 9, 7, piece, offset) + data)
                        pending.clear()
        except (EOFError, OSError):
            pass


class PeerServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class Speed(unittest.TestCase):
    def fixture(self, folder, batch, plen, delay):
        info = {'name': 'throughput.bin', 'length': len(PAYLOAD), 'piece length': plen,
                'pieces': b''.join(hashlib.sha1(PAYLOAD[i:i + plen]).digest()
                                   for i in range(0, len(PAYLOAD), plen))}
        server = PeerServer(('127.0.0.1', 0), BatchedPeer)
        server.info_hash = hashlib.sha1(enc(info)).digest()
        server.batch, server.plen, server.delay, server.largest_batch = batch, plen, delay, 0
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        meta, tracker = folder / 'sample.torrent', folder / 'tracker'
        meta.write_bytes(enc({'announce': 'http://127.0.0.1:1/announce', 'info': info}))
        compact = socket.inet_aton('127.0.0.1') + struct.pack('>H', server.server_address[1])
        tracker.write_bytes(enc({'interval': 1800, 'peers': compact}))
        return meta, tracker, server

    def test_starts_with_sixteen_requests_before_any_reply(self):
        for compat in (False, True):
            with self.subTest(compat=compat), tempfile.TemporaryDirectory() as folder:
                p = Path(folder).resolve()
                meta, tracker, server = self.fixture(p, batch=16, plen=262144, delay=0)
                env = dict(os.environ, PORTAL_TORRENT_LOG='1')
                if compat:
                    env['PORTAL_TORRENT_FORCE_COMPAT'] = '1'
                result = subprocess.run([str(ROOT / 'build/torrent_driver'), str(meta), str(p / 'out'), str(tracker), '0'],
                                        env=env, capture_output=True, text=True, timeout=8)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((p / 'out/throughput.bin').read_bytes(), PAYLOAD)
                self.assertEqual(server.largest_batch, 16)

    def test_ui_reports_rate_before_large_piece_is_verified(self):
        for compat in (False, True):
            with self.subTest(compat=compat), tempfile.TemporaryDirectory() as folder:
                p = Path(folder).resolve()
                meta, tracker, _ = self.fixture(p, batch=1, plen=len(PAYLOAD), delay=.03)
                env = dict(os.environ, PORTAL_TORRENT_LOG='1')
                if compat:
                    env['PORTAL_TORRENT_FORCE_COMPAT'] = '1'
                result = subprocess.run([str(ROOT / 'build/queue_driver'), str(p / 'app'), str(meta), str(tracker), 'rate'],
                                        env=env, capture_output=True, text=True, timeout=8)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('Rate before first verified piece:', result.stdout)
