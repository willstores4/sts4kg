"""Exercise denied descriptor-mode changes with real localhost TCP/UDP I/O."""
import os
import subprocess
import tempfile
import time
from pathlib import Path
from unittest import mock
import test_torrent_swarm as swarm
from test_torrent_transfer import ROOT, enc


class CompatibilitySwarm(swarm.Swarm):
    def setUp(self):
        forced = mock.patch.dict(os.environ, PORTAL_TORRENT_FORCE_COMPAT='1')
        forced.start()
        self.addCleanup(forced.stop)

    def engine(self, *args, **kwargs):
        result = super().engine(*args, **kwargs)
        self.assertLog('compatibility mode enabled', result)
        self.assertLog('compatibility connect completed', result)
        self.assertLog('First payload block:', result)
        return result

    test_accepts_incoming_peer = None

    def test_compatibility_uses_outgoing_peers_only(self):
        # Compatibility mode intentionally uses outgoing connections only.
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder).resolve()
            seed = self.peer()
            url, _ = self.tracker(swarm.compact(seed.server_address[1]))
            result = self.engine(p, url)
            self.assertComplete(p, result)
            self.assertLog('Incoming listener disabled in compatibility mode', result)

    def timed_pause(self, folder, announce, delay_ms):
        meta = folder / 'pause.torrent'
        meta.write_bytes(enc({'announce': announce, 'info': swarm.INFO}))
        began = time.monotonic()
        result = subprocess.run(
            [str(ROOT / 'build/torrent_driver'), str(meta), str(folder / 'out'),
             str(folder / 'missing'), '0', str(delay_ms)],
            env=dict(os.environ, PORTAL_TORRENT_LOG='1'), capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertLess(time.monotonic() - began, 3, result.stderr)
        self.assertLog('Swarm stop completed after', result)
        return result

    def test_pause_with_silent_peer(self):
        with tempfile.TemporaryDirectory() as folder:
            seed = self.peer(unchoke_delay=30)
            url, _ = self.tracker(swarm.compact(seed.server_address[1]))
            result = self.timed_pause(Path(folder).resolve(), url, 1000)
            self.assertLog('handshake ok', result)
