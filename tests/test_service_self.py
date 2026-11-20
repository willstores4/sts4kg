import contextlib
import io
import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_package import verify_daemon_self

class DaemonSelf(unittest.TestCase):
    def image(self):
        data=bytearray(512);data[:4]=b'\x4f\x15\x3d\x1d';struct.pack_into('<H',data,24,2)
        data[96:100]=b'\x7fELF';struct.pack_into('<H',data,96+56,1)
        struct.pack_into('<QQ',data,224,0x3100000000000002,1)
        return data
    def test_accepts_homebrew_daemon_format(self):
        with contextlib.redirect_stdout(io.StringIO()):verify_daemon_self(self.image())
    def test_rejects_system_exec_format(self):
        data=self.image();struct.pack_into('<Q',data,232,8)
        with self.assertRaisesRegex(RuntimeError,'homebrew fake'):verify_daemon_self(data)
    def test_rejects_wrong_authentication_id(self):
        data=self.image();struct.pack_into('<Q',data,224,0x3800000000000011)
        with self.assertRaisesRegex(RuntimeError,'daemon PAID'):verify_daemon_self(data)
    def test_rejects_truncated_header(self):
        with self.assertRaises(RuntimeError):verify_daemon_self(self.image()[:240])

if __name__=='__main__':unittest.main()
