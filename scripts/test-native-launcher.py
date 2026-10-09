#!/usr/bin/env python3
"""Isolated tests for the native Windows launcher; no VM or WSL is started."""
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import unittest
import uuid
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location("native_manager", Path(__file__).with_name("manage-native.py"))
MANAGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MANAGER)


class NativeTests(unittest.TestCase):
    def test_readiness_needs_real_tagged_pid(self):
        self.assertIsNone(MANAGER.ready_pid("BOOT=1\nANIM=stopped\nPID=\n"))
        self.assertIsNone(MANAGER.ready_pid("BOOT=1\nANIM=stopped\nPID=1\n"))
        self.assertIsNone(MANAGER.ready_pid("BOOT=1\nANIM=running\nPID=700\n"))
        self.assertEqual(MANAGER.ready_pid("\r\nBOOT=1\r\nANIM=stopped\r\nPID=700\r\n"), "700")

    def test_native_command_is_independent_and_loopback_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            images = root / "images"
            images.mkdir()
            firmware = root / "host/install/share/qemu"
            firmware.mkdir(parents=True)
            (firmware / "efi-virtio.rom").write_bytes(b"rom")
            for name in ("system.img", "userdata.qcow2", "vendor.img"):
                (images / name).write_bytes(b"x" * 4096)
            profile = {"uuid": str(uuid.uuid4()), "adb_port": 5559, "qmp_port": 5560, "console_port": 5561}
            command = MANAGER.command_line(root, root / "run", profile, 4, 6144, False)
            self.assertTrue(command[0].endswith("qemu-system-aarch64.exe"))
            self.assertNotIn("wsl.exe", " ".join(command))
            self.assertNotIn("bash", " ".join(command))
            self.assertIn("tcg,thread=multi", command)
            self.assertIn("virtio-tablet-pci,touchscreen=on", command)
            self.assertIn("sdl,gl=on,show-cursor=on", command)
            self.assertIn("virtio-gpu-gl-pci", " ".join(command))
            # gl=core (native AMD WGL) rendered a black window; keep it selectable but not default.
            self.assertIn("sdl,gl=core,show-cursor=on",
                          MANAGER.command_line(root, root / "run", profile, 4, 6144, False,
                                               "sdl,gl=core,show-cursor=on"))
            software = MANAGER.command_line(root, root / "run", profile, 4, 6144, False,
                                           "sdl,show-cursor=on", "virtio-gpu-pci")
            self.assertIn("virtio-gpu-pci,addr=0x6,xres=1080,yres=2400", software)
            self.assertNotIn("virtio-gpu-gl-pci", " ".join(software))
            self.assertIn("user,id=net0,hostfwd=tcp:127.0.0.1:5559-:5555", command)
            self.assertTrue(any("id=system,readonly=on" in part for part in command))
            self.assertTrue(any("id=vendor,readonly=on" in part for part in command))
            self.assertNotIn("-snapshot", command)
            self.assertIn("-snapshot", MANAGER.command_line(root, root / "run", profile, 4, 6144, True))

    @unittest.skipUnless(os.name == "nt", "Windows process identity API")
    def test_creation_time_prevents_recycled_pid(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            identity = MANAGER.process_identity(os.getpid())
            self.assertTrue(identity["exe"].endswith("python.exe"))
            record = {"pid": os.getpid(), "identity": dict(identity, creation_time=identity["creation_time"] - 1)}
            (work / "launch.json").write_text(json.dumps(record))
            self.assertIsNone(MANAGER.native_pid(work))
            os.kill(os.getpid(), 0)

    def test_qmp_rejects_other_vm_before_action(self):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        observed = []
        errors = []
        def serve():
            try:
                with listener, listener.accept()[0] as connection:
                    connection.settimeout(2)
                    stream = connection.makefile("rb")
                    connection.sendall(b'{"QMP":{}}\n')
                    for response in ({"return": {}}, {"return": {"UUID": str(uuid.uuid4())}}):
                        request = json.loads(stream.readline())
                        observed.append(request["execute"])
                        connection.sendall(json.dumps(response).encode() + b"\n")
            except Exception as error:
                errors.append(error)
        worker = threading.Thread(target=serve)
        worker.start()
        with self.assertRaisesRegex(RuntimeError, "Another VM"):
            MANAGER.qmp(port, {"execute": "quit"}, str(uuid.uuid4()))
        worker.join(5)
        self.assertFalse(worker.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual(observed, ["qmp_capabilities", "query-uuid"])

    def test_stopped_vm_leaves_images_untouched(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            disk = work / "userdata.qcow2"
            disk.write_bytes(b"persist")
            with patch.object(MANAGER, "native_pid", return_value=None), patch.object(MANAGER, "qmp") as endpoint:
                MANAGER.stop(work, work, {})
                endpoint.assert_not_called()
            self.assertEqual(disk.read_bytes(), b"persist")

    @unittest.skipUnless(os.name == "nt", "Windows locking API")
    def test_local_manager_lock_excludes_second_writer(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lock"
            with MANAGER.file_lock(path):
                with self.assertRaisesRegex(RuntimeError, "Another tebox command"):
                    with MANAGER.file_lock(path, timeout=0.2):
                        self.fail("The second writer must not acquire the lock")


if __name__ == "__main__":
    unittest.main()
