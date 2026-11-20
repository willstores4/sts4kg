"""Resume safety and peer history, with generated files and local peers only."""
import hashlib
import os
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path
from test_torrent_transfer import ROOT, Server, enc, exact

BLOCK = 16384


class ResumePeer:
    # BaseRequestHandler's constructor invokes handle immediately.
    def __init__(self, request, address, server):
        self.request, self.server = request, server
        self.handle()

    def handle(self):
        try:
            s = self.server
            self.request.settimeout(5)
            hs = exact(self.request, 68)
            s.connections += 1
            connection = s.connections
            if hs[28:48] != s.ihash:
                return
            self.request.sendall(hs[:48] + b'-RESUME-123456789012')
            count = (len(s.payload) + s.plen - 1) // s.plen
            bf = bytes([255]) * ((count + 7) // 8)
            self.request.sendall(struct.pack('>I', len(bf) + 1) + b'\x05' + bf + struct.pack('>IB', 1, 1))
            while True:
                n = struct.unpack('>I', exact(self.request, 4))[0]
                m = exact(self.request, n)
                if m and m[0] == 6:
                    piece, off, length = struct.unpack('>III', m[1:])
                    s.requests.append((connection, piece, off))
                    time.sleep(s.delay)
                    data = s.payload[piece * s.plen + off:piece * s.plen + off + length]
                    self.request.sendall(struct.pack('>IBII', len(data) + 9, 7, piece, off) + data)
        except (EOFError, OSError):
            pass


def checkpoint(meta):
    data = Path(str(meta) + '.resume').read_bytes()
    assert data[:8] == b'NULLRS02' and hashlib.sha1(data[:-20]).digest() == data[-20:]
    plen, count, total, nb, np = struct.unpack_from('>IIQII', data, 28)
    bits = data[72:72 + count]
    start = 72 + count
    blocks = [struct.unpack_from('>II', data, start + 28 * i) for i in range(nb)]
    start += nb * 28
    peers = [data[start + i * 20:start + (i + 1) * 20] for i in range(np)]
    return plen, total, bits, blocks, peers


class Resume(unittest.TestCase):
    def fixture(self, folder, plen=262144, length=786555, delay=.015):
        payload = (bytes(range(251)) * (length // 251 + 1))[:length]
        info = {'name': 'resume.bin', 'piece length': plen, 'length': len(payload),
                'pieces': b''.join(hashlib.sha1(payload[i:i + plen]).digest()
                                   for i in range(0, len(payload), plen))}
        server = Server(('127.0.0.1', 0), ResumePeer)
        server.payload, server.plen = payload, plen
        server.ihash, server.connections = hashlib.sha1(enc(info)).digest(), 0
        server.requests, server.delay = [], delay
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        meta, tracker = folder / 'test.torrent', folder / 'tracker'
        # Private avoids any external DHT bootstrap during these tests.
        info['private'] = 1
        server.ihash = hashlib.sha1(enc(info)).digest()
        meta.write_bytes(enc({'announce': 'http://127.0.0.1:1/announce', 'info': info}))
        endpoint = socket.inet_aton('127.0.0.1') + struct.pack('>H', server.server_address[1])
        tracker.write_bytes(enc({'interval': 1800, 'peers': endpoint}))
        return meta, tracker, server, payload

    def command(self, meta, tracker, dest, stop=0, stop_ms=0):
        return [str(ROOT / 'build/torrent_driver'), str(meta), str(dest), str(tracker), str(stop), str(stop_ms)]

    def run_engine(self, meta, tracker, dest, stop=0, stop_ms=0, compat=False):
        env = dict(os.environ, PORTAL_TORRENT_LOG='1', PORTAL_TORRENT_PHASE_LOG='1')
        if compat:
            env['PORTAL_TORRENT_FORCE_COMPAT'] = '1'
        return subprocess.run(self.command(meta, tracker, dest, stop, stop_ms),
                              env=env, capture_output=True, text=True, timeout=20)

    def assert_ok(self, result, code=0):
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)

    def test_fast_index_skips_sparse_tail_but_rehashes_saved_piece(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory).resolve()
            meta, tr, server, payload = self.fixture(p)
            self.assert_ok(self.run_engine(meta, tr, p / 'out', stop=server.plen), 2)
            with (p / 'out/resume.bin').open('r+b') as f:
                f.truncate(len(payload))
            result = self.run_engine(meta, tr, p / 'out')
            self.assert_ok(result)
            self.assertIn(f'File verification: resume index, {server.plen} bytes read', result.stderr)
            self.assertIn('Verifying saved pieces: 100%', result.stderr)
            self.assertEqual((p / 'out/resume.bin').read_bytes(), payload)

    def test_modified_saved_piece_is_rejected_and_redownloaded(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory).resolve()
            meta, tr, server, payload = self.fixture(p)
            self.assert_ok(self.run_engine(meta, tr, p / 'out', stop=server.plen), 2)
            with (p / 'out/resume.bin').open('r+b') as f:
                f.write(b'CORRUPTED')
            before = server.connections
            result = self.run_engine(meta, tr, p / 'out')
            self.assert_ok(result)
            self.assertIn('1 saved pieces rejected', result.stderr)
            self.assertIn(0, [piece for c, piece, _ in server.requests if c > before])
            self.assertEqual((p / 'out/resume.bin').read_bytes(), payload)

    def test_missing_damaged_or_wrong_destination_index_uses_full_scan(self):
        for damage in ('missing', 'truncated', 'checksum', 'destination'):
            with self.subTest(damage=damage), tempfile.TemporaryDirectory() as directory:
                p = Path(directory).resolve()
                meta, tr, server, payload = self.fixture(p)
                self.assert_ok(self.run_engine(meta, tr, p / 'out', stop=server.plen), 2)
                path = Path(str(meta) + '.resume')
                if damage == 'missing':
                    path.unlink()
                elif damage == 'truncated':
                    path.write_bytes(path.read_bytes()[:80])
                elif damage == 'checksum':
                    data = bytearray(path.read_bytes()); data[72] ^= 1; path.write_bytes(data)
                dest = p / ('different' if damage == 'destination' else 'out')
                result = self.run_engine(meta, tr, dest)
                self.assert_ok(result)
                self.assertIn('File verification: full scan', result.stderr)
                self.assertIn('Checking files: 100% (full scan)', result.stderr)
                self.assertEqual((dest / 'resume.bin').read_bytes(), payload)

    def test_pause_preserves_unfinished_blocks_and_uses_cached_peer(self):
        for compat in (False, True):
            with self.subTest(compat=compat), tempfile.TemporaryDirectory() as directory:
                p = Path(directory).resolve()
                meta, tr, server, payload = self.fixture(p, plen=1048576, length=1048576, delay=.03)
                result = self.run_engine(meta, tr, p / 'out', stop_ms=330, compat=compat)
                self.assert_ok(result, 2)
                _, _, bits, blocks, _ = checkpoint(meta)
                self.assertEqual(sum(bits), 0)
                self.assertGreater(len(blocks), 0)
                before = server.connections
                tr.write_bytes(enc({'interval': 1800, 'peers': b''}))
                result = self.run_engine(meta, tr, p / 'out', compat=compat)
                self.assert_ok(result)
                self.assertIn(f'{len(blocks)} blocks /', result.stderr)
                self.assertIn('1 productive peers ready', result.stderr)
                requests = {(piece, off // BLOCK) for c, piece, off in server.requests if c > before}
                self.assertTrue(set(blocks).isdisjoint(requests), (blocks, requests))
                self.assertEqual((p / 'out/resume.bin').read_bytes(), payload)

    def test_corrupt_unfinished_block_is_discarded(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory).resolve()
            meta, tr, server, payload = self.fixture(p, plen=1048576, length=1048576, delay=.03)
            self.assert_ok(self.run_engine(meta, tr, p / 'out', stop_ms=330), 2)
            piece, block = checkpoint(meta)[3][0]
            with (p / 'out/resume.bin').open('r+b') as f:
                f.seek(piece * server.plen + block * BLOCK); f.write(b'BAD BLOCK')
            result = self.run_engine(meta, tr, p / 'out')
            self.assert_ok(result)
            self.assertIn('1 missing or corrupt blocks discarded', result.stderr)
            self.assertEqual((p / 'out/resume.bin').read_bytes(), payload)

    def test_invalid_partial_layout_is_rejected_even_with_valid_checksum(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory).resolve()
            meta, tr, server, payload = self.fixture(p, plen=1048576, length=1048576, delay=.03)
            self.assert_ok(self.run_engine(meta, tr, p / 'out', stop_ms=330), 2)
            path = Path(str(meta) + '.resume')
            data = bytearray(path.read_bytes())
            # Claim an out-of-bounds block, then recompute the container checksum.
            struct.pack_into('>I', data, 72 + 1 + 4, 0xffffffff)
            data[-20:] = hashlib.sha1(data[:-20]).digest()
            path.write_bytes(data)
            result = self.run_engine(meta, tr, p / 'out')
            self.assert_ok(result)
            self.assertIn('Resume index damaged or belongs to different files', result.stderr)
            self.assertIn('File verification: full scan', result.stderr)
            self.assertEqual((p / 'out/resume.bin').read_bytes(), payload)

    def test_expired_peer_history_is_ignored_without_losing_saved_blocks(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory).resolve()
            meta, tr, server, payload = self.fixture(p, plen=1048576, length=1048576, delay=.03)
            self.assert_ok(self.run_engine(meta, tr, p / 'out', stop_ms=330), 2)
            path = Path(str(meta) + '.resume')
            data = bytearray(path.read_bytes())
            nb = struct.unpack_from('>I', data, 44)[0]
            peer_start = 72 + 1 + nb * 28
            struct.pack_into('>I', data, peer_start + 8, int(time.time()) - 8 * 86400)
            data[-20:] = hashlib.sha1(data[:-20]).digest()
            path.write_bytes(data)
            result = self.run_engine(meta, tr, p / 'out')
            self.assert_ok(result)
            self.assertIn('0 productive peers ready, 0 cooling down, 1 expired', result.stderr)
            self.assertIn(f'{nb} blocks /', result.stderr)
            self.assertEqual((p / 'out/resume.bin').read_bytes(), payload)

    def test_failed_peer_retains_cooldown_across_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory).resolve()
            meta, tr, server, payload = self.fixture(p, plen=1048576, length=1048576, delay=.03)
            dead = socket.socket(); dead.bind(('127.0.0.1', 0)); port = dead.getsockname()[1]; dead.close()
            tr.write_bytes(enc({'interval': 1800, 'peers': socket.inet_aton('127.0.0.1') + struct.pack('>H', port) +
                               socket.inet_aton('127.0.0.1') + struct.pack('>H', server.server_address[1])}))
            self.assert_ok(self.run_engine(meta, tr, p / 'out', stop_ms=400), 2)
            failed = [peer for peer in checkpoint(meta)[4] if struct.unpack_from('>H', peer, 4)[0] == port][0]
            self.assertGreater(failed[6], 0)
            self.assertGreater(struct.unpack_from('>I', failed, 12)[0], time.time())
            listener = socket.socket(); listener.bind(('127.0.0.1', port)); listener.listen(); listener.settimeout(.1)
            self.addCleanup(listener.close)
            result = self.run_engine(meta, tr, p / 'out')
            self.assert_ok(result)
            self.assertIn('1 cooling down', result.stderr)
            with self.assertRaises(socket.timeout):
                listener.accept()
            self.assertEqual((p / 'out/resume.bin').read_bytes(), payload)

    def test_periodic_partial_checkpoint_survives_process_kill(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory).resolve()
            meta, tr, server, payload = self.fixture(p, plen=2097152, length=2097152, delay=.05)
            process = subprocess.Popen(self.command(meta, tr, p / 'out'), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + 9
                blocks = []
                while time.monotonic() < deadline:
                    path = Path(str(meta) + '.resume')
                    if path.exists():
                        blocks = checkpoint(meta)[3]
                        if blocks:
                            break
                    time.sleep(.03)
                self.assertTrue(blocks, 'no periodic checkpoint was published')
                process.kill(); process.wait(timeout=3)
            finally:
                if process.poll() is None:
                    process.kill(); process.wait()
            server.delay = 0
            result = self.run_engine(meta, tr, p / 'out')
            self.assert_ok(result)
            self.assertIn(f'{len(blocks)} blocks /', result.stderr)
            self.assertEqual((p / 'out/resume.bin').read_bytes(), payload)


if __name__ == '__main__':
    unittest.main()
