#!/usr/bin/env python3
"""Run the ARM64 tebox guest directly on Windows, without WSL or a shell runtime."""
import argparse
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time
import uuid


# adb reserves 5554..5609 for emulator consoles/transports and probes the
# paired port of anything it connects to inside that range.  A QEMU chardev bound
# there (the guest console) is then taken over by the adb server, so the launcher's
# own console connection is refused and Android never appears to stabilize.  Keep all
# three launcher ports outside that range.
DEFAULT_ADB = 5700
DEFAULT_QMP = 5701
DEFAULT_CONSOLE = 5702
ADB_EMULATOR_PORTS = range(5554, 5610)


@contextmanager
def file_lock(path, timeout=10):
    import msvcrt
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as handle:
        if path.stat().st_size == 0:
            handle.write(b"\0")
            handle.flush()
        deadline = time.monotonic() + timeout
        while True:
            handle.seek(0)
            try:
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise RuntimeError(f"Another tebox command still owns {path.name}")
                time.sleep(0.1)
        try:
            yield
        finally:
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)


def process_identity(pid):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        error = ctypes.get_last_error()
        if error in (87, 1168):
            return None
        raise OSError(error, "Cannot inspect the saved native QEMU process")
    try:
        code = wintypes.DWORD()
        if not kernel.GetExitCodeProcess(handle, ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        if code.value != 259:
            return None
        length = wintypes.DWORD(32768)
        path = ctypes.create_unicode_buffer(length.value)
        if not kernel.QueryFullProcessImageNameW(handle, 0, path, ctypes.byref(length)):
            raise ctypes.WinError(ctypes.get_last_error())
        times = [wintypes.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *(ctypes.byref(item) for item in times)):
            raise ctypes.WinError(ctypes.get_last_error())
        return {"exe": os.path.normcase(os.path.abspath(path.value)),
                "creation_time": (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime}
    finally:
        kernel.CloseHandle(handle)


def native_pid(work):
    path = work / "launch.json"
    if not path.is_file():
        return None
    record = json.loads(path.read_text())
    identity = process_identity(record["pid"])
    if identity and identity == record.get("identity"):
        return record["pid"]
    return None


def qmp(port, command, expected_uuid=None):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as connection:
        stream = connection.makefile("rb")
        greeting = json.loads(stream.readline())
        if "QMP" not in greeting:
            raise RuntimeError("The local endpoint is not QEMU QMP")

        def execute(request):
            connection.sendall(json.dumps(request).encode() + b"\n")
            while True:
                line = stream.readline()
                if not line:
                    raise RuntimeError("QMP closed before returning a response")
                response = json.loads(line)
                if "error" in response:
                    raise RuntimeError(str(response["error"]))
                if "return" in response:
                    return response["return"]

        execute({"execute": "qmp_capabilities"})
        if expected_uuid:
            actual = execute({"execute": "query-uuid"}).get("UUID", "").lower()
            if actual != expected_uuid.lower():
                raise RuntimeError("Another VM owns the QMP endpoint; it was not modified")
        return execute(command)


def guest(work, port, command, timeout=15):
    token = "__TEBOX_DONE_" + uuid.uuid4().hex + "__"
    output = bytearray()
    with file_lock(work / "console.lock", timeout=timeout), \
         socket.create_connection(("127.0.0.1", port), timeout=5) as connection:
        connection.sendall(("\n" + command + "\necho " + token + "\n").encode())
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            connection.settimeout(max(0.1, deadline - time.monotonic()))
            try:
                chunk = connection.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            output.extend(chunk)
            if ("\n" + token + "\n").encode() in output.replace(b"\r", b""):
                return output.decode(errors="replace")
    raise RuntimeError("Android console command did not complete before the timeout")


def ready_pid(output):
    lines = output.replace("\r", "").splitlines()
    pids = [line[4:] for line in lines if re.fullmatch(r"PID=[0-9]+", line)]
    if "BOOT=1" in lines and "ANIM=stopped" in lines and len(pids) == 1 and int(pids[0]) > 1:
        return pids[0]
    return None


def one_property(work, port, prop):
    result = guest(work, port, "echo VALUE=$(getprop " + prop + ")", timeout=5)
    values = [line[6:] for line in result.replace("\r", "").splitlines() if line.startswith("VALUE=")]
    return values[-1] if values else "(not ready)"


def read_profile(root):
    path = root / "native-profile.json"
    if path.is_file():
        profile = json.loads(path.read_text())
        uuid.UUID(profile["uuid"])
        return profile
    profile = {"uuid": str(uuid.uuid4()), "adb_port": DEFAULT_ADB,
               "qmp_port": DEFAULT_QMP, "console_port": DEFAULT_CONSOLE}
    path.write_text(json.dumps(profile, indent=2) + "\n")
    return profile


def image_arguments(images):
    result = []
    for name, fmt, readonly in (("system", "raw", True), ("userdata", "qcow2", False), ("vendor", "raw", True)):
        path = images / (name + (".qcow2" if name == "userdata" else ".img"))
        if not path.is_file() or path.stat().st_size < 4096:
            raise RuntimeError(f"Missing prepared native image: {path}")
        if "," in str(path):
            raise RuntimeError("QEMU image paths must not contain commas")
        result += ["-drive", f"if=none,file={path.as_posix()},format={fmt},id={name}" + (",readonly=on" if readonly else ""),
                   "-device", f"virtio-blk-pci,drive={name},serial={name}"]
    return result


DISPLAY_BACKENDS = ("sdl,gl=on,show-cursor=on", "sdl,gl=core,show-cursor=on", "sdl,show-cursor=on",
                    "egl-headless")


def firmware_directory(root, binary):
    for candidate in (root / "host/install/share/qemu", binary.parent / "share/qemu",
                      binary.parent / "share/qemu-firmware"):
        if (candidate / "efi-virtio.rom").is_file():
            return candidate
    raise RuntimeError("Missing efi-virtio.rom; the virtio network device cannot be created")


def command_line(root, work, profile, cpus, memory, snapshot, display=DISPLAY_BACKENDS[0], gpu_device="virtio-gpu-gl-pci", blob=False):
    binary = root / "host/install/bin/qemu-system-aarch64.exe"
    images = root / "images"
    cmdline = ("console=ttyAMA0,115200 earlycon=pl011,0x9000000 ignore_loglevel "
               "androidboot.console=ttyAMA0 androidboot.hardware=ranchu androidboot.selinux=permissive "
               "androidboot.zygote=zygote64 androidboot.init_fatal_reboot_target= rdinit=/init "
               "module.sig_enforce=0 androidboot.qemu_debug=1 printk.devkmsg=on androidboot.qemu_adb=1")
    command = [str(binary), "-name", "Tebox ARM64 - Native Windows", "-uuid", profile["uuid"],
               "-L", str(firmware_directory(root, binary)),
               "-machine", "virt,gic-version=3", "-cpu", "max", "-accel", "tcg,thread=multi",
               "-smp", str(cpus), "-m", str(memory), "-kernel", str(images / "Image"),
               "-initrd", str(images / "initramfs.img"), "-append", cmdline,
               "-qmp", f"tcp:127.0.0.1:{profile['qmp_port']},server=on,wait=off",
               "-device", "virtio-serial-pci", "-chardev",
               f"socket,id=debug,host=127.0.0.1,port={profile['console_port']},server=on,wait=off",
               "-device", "virtconsole,chardev=debug"]
    if snapshot:
        command.append("-snapshot")
    command += image_arguments(images)
    command += ["-device", "virtio-net-pci,netdev=net0", "-netdev",
                f"user,id=net0,hostfwd=tcp:127.0.0.1:{profile['adb_port']}-:5555",
                "-device", gpu_device + ",addr=0x6,xres=1080,yres=2400" + (",blob=on" if blob else ""),
                "-device", "virtio-keyboard-pci", "-device", "virtio-tablet-pci,touchscreen=on",
                "-display", display, "-serial", "file:" + str(work / "serial.log")]
    return command


def start(args, root, work, profile):
    if native_pid(work):
        print("Native ARM64 Android is already running; it was not restarted.", flush=True)
        status(root, work, profile)
        return False
    if any(profile[name] in ADB_EMULATOR_PORTS
           for name in ("adb_port", "qmp_port", "console_port")):
        profile["adb_port"], profile["qmp_port"], profile["console_port"] = \
            DEFAULT_ADB, DEFAULT_QMP, DEFAULT_CONSOLE
        (root / "native-profile.json").write_text(json.dumps(profile, indent=2) + "\n")
        print("Moved the native ports out of the adb emulator port range; "
              "the userdata image and the original WSL runtime are unchanged.", flush=True)
    if args.software_render:
        gpu_device, display = "virtio-gpu-pci", "sdl,show-cursor=on"
    else:
        gpu_device, display = "virtio-gpu-gl-pci", args.display
    command = command_line(root, work, profile, args.cpus, args.memory, args.snapshot, display, gpu_device,
                           args.gpu_blob)
    binary = Path(command[0])
    if not binary.is_file():
        raise RuntimeError(f"Build the native Windows host first: {binary}")
    for name in ("Image", "initramfs.img"):
        if not (root / "images" / name).is_file():
            raise RuntimeError(f"Missing native image: {name}")
    for port in (profile["adb_port"], profile["qmp_port"], profile["console_port"]):
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            try:
                probe.bind(("127.0.0.1", port))
            except OSError:
                raise RuntimeError(f"Local port {port} is unavailable; no other process was stopped")
    stamp = time.strftime("%Y%m%d-%H%M%S") + f"-{time.time_ns() % 1000000:06d}"
    for name in ("qemu.log", "serial.log", "launch.json"):
        old = work / name
        if old.exists():
            old.rename(work / f"{stamp}-{name}")
    env = dict(os.environ, SDL_VIDEODRIVER="windows")
    if not args.software_render and args.present == "gdi":
        env["TEBOX_WIN32_GDI_PRESENT"] = "1"
    else:
        env.pop("TEBOX_WIN32_GDI_PRESENT", None)
    env["PATH"] = str(binary.parent) + os.pathsep + os.path.join(os.environ["SystemRoot"], "System32")
    for name in ("LIBGL_ALWAYS_SOFTWARE", "SDL_OPENGL_LIBRARY",
                 "SDL_OPENGLES_LIBRARY", "LD_LIBRARY_PATH", "DISPLAY", "WAYLAND_DISPLAY"):
        env.pop(name, None)
    if args.host_gl_driver == "auto":
        env.pop("GALLIUM_DRIVER", None)
        env.pop("MESA_D3D12_DEFAULT_ADAPTER_NAME", None)
    else:
        env["GALLIUM_DRIVER"] = args.host_gl_driver
        if args.host_gl_driver == "d3d12" and args.gpu:
            env["MESA_D3D12_DEFAULT_ADAPTER_NAME"] = args.gpu
    with (work / "qemu.log").open("ab") as output:
        process = subprocess.Popen(command, cwd=root, env=env, stdin=subprocess.DEVNULL,
                                   stdout=output, stderr=output,
                                   creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)
    identity = process_identity(process.pid)
    if not identity:
        raise RuntimeError((work / "qemu.log").read_text(errors="replace")[-4000:])
    record = {"pid": process.pid, "identity": identity, "uuid": profile["uuid"],
              "started": time.time(), "cpus": args.cpus, "memory_mib": args.memory,
              "snapshot": args.snapshot, "command": command}
    (work / "launch.json").write_text(json.dumps(record, indent=2) + "\n")
    for _ in range(30):
        if process.poll() is not None:
            raise RuntimeError((work / "qemu.log").read_text(errors="replace")[-4000:])
        try:
            qmp(profile["qmp_port"], {"execute": "query-status"}, profile["uuid"])
            break
        except (OSError, RuntimeError):
            time.sleep(1)
    else:
        raise RuntimeError("Native QEMU did not expose its management endpoint; inspect qemu.log")
    print(f"Native Windows ARM64 Android started: PID {process.pid}, {args.cpus} vCPUs, {args.memory} MiB", flush=True)
    print("Graphics: VirGL over native Windows OpenGL; no WSL process is launched.", flush=True)
    if not args.software_render:
        print("Presentation: " + ("Win32 GDI GPU readback (max 30 fps)" if args.present == "gdi"
                                   else "SDL OpenGL swap"), flush=True)
    print("Userdata: " + ("snapshot; changes discarded on exit" if args.snapshot else "persistent independent qcow2"), flush=True)
    print(f"ADB: 127.0.0.1:{profile['adb_port']} (authenticated, loopback only)", flush=True)
    print(f"Runtime: {root}", flush=True)
    return True


def wait_boot(root, work, profile, seconds):
    deadline = time.monotonic() + seconds
    last_pid = stable_since = None
    while time.monotonic() < deadline:
        if not native_pid(work):
            raise RuntimeError("Native QEMU exited before Android completed startup")
        try:
            output = guest(work, profile["console_port"],
                           "echo BOOT=$(getprop sys.boot_completed); echo ANIM=$(getprop init.svc.bootanim); echo PID=$(pidof system_server)", 5)
            server = ready_pid(output)
            if not server:
                last_pid = stable_since = None
            elif server != last_pid:
                last_pid, stable_since = server, time.monotonic()
            elif time.monotonic() - stable_since >= 20:
                print("Android boot completed; system_server remained stable for 20 seconds.", flush=True)
                return
        except (OSError, RuntimeError):
            last_pid = stable_since = None
        time.sleep(5)
    raise RuntimeError(f"Android did not stabilize within {seconds}s; VM left running for diagnosis")


def status(root, work, profile):
    pid = native_pid(work)
    print(f"Runtime: {root}")
    print(f"Native VM: {'running (PID ' + str(pid) + ')' if pid else 'stopped'}")
    if pid:
        print("QEMU:", qmp(profile["qmp_port"], {"execute": "query-status"}, profile["uuid"])["status"])
        for name, prop in (("Android boot completed", "sys.boot_completed"), ("Guest ABI", "ro.product.cpu.abi"),
                           ("Boot animation", "init.svc.bootanim"), ("ADB service", "init.svc.adbd")):
            try:
                value = one_property(work, profile["console_port"], prop)
            except (OSError, RuntimeError):
                value = "(not ready)"
            print(f"{name}: {value}")
        print(f"ADB: 127.0.0.1:{profile['adb_port']}")


def stop(root, work, profile):
    if not native_pid(work):
        print("Native ARM64 Android is already stopped; no process was terminated.")
        return
    qmp(profile["qmp_port"], {"execute": "query-status"}, profile["uuid"])
    try:
        guest(work, profile["console_port"], "sync", 10)
    except (OSError, RuntimeError):
        print("Guest sync did not respond; requesting QEMU shutdown without deleting any data.")
    qmp(profile["qmp_port"], {"execute": "quit"}, profile["uuid"])
    for _ in range(60):
        if not native_pid(work):
            print("Stopped this native VM. Its userdata and the original WSL runtime were preserved.")
            return
        time.sleep(0.25)
    raise RuntimeError("QEMU has not exited; no unrelated process was terminated")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", nargs="?", choices=("start", "stop", "status", "console"), default="start")
    parser.add_argument("--runtime", type=Path, default=Path(os.environ.get("LOCALAPPDATA", ".")) / "TeboxNative")
    parser.add_argument("--cpus", type=int, choices=range(1, 33), default=4)
    parser.add_argument("--memory", type=int, default=6144)
    parser.add_argument("--gpu", default="AMD Radeon RX 7900 XT",
                        help="Adapter name passed to Mesa's D3D12 driver")
    parser.add_argument("--wait", type=int, default=0)
    parser.add_argument("--snapshot", action="store_true")
    parser.add_argument("--display", choices=DISPLAY_BACKENDS, default=DISPLAY_BACKENDS[0],
                        help="Host display backend; gl=on may use native Windows WGL")
    parser.add_argument("--present", choices=("gdi", "gl"), default="gdi",
                        help="Windows presentation: GPU scanout readback to GDI, or original SDL GL swap")
    parser.add_argument("--software-render", action="store_true",
                        help="Diagnostic: plain virtio-gpu with no host GL (slower, CPU-rendered guest)")
    parser.add_argument("--host-gl-driver", choices=("auto", "d3d12", "llvmpipe"), default="auto",
                        help="Host OpenGL driver for the bundled Mesa opengl32.dll, if present")
    parser.add_argument("--gpu-blob", action="store_true",
                        help="Advertise virtio-gpu blob resources (experimental scanout path)")
    parser.add_argument("--command")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This launcher requires native Windows Python, not WSL.")
    if not 2048 <= args.memory <= 32768 or not 0 <= args.wait <= 1800:
        parser.error("Memory must be 2048..32768 MiB; wait must be 0..1800 seconds.")
    root = args.runtime.expanduser().resolve()
    if not (root / "images/image-manifest.json").is_file():
        parser.error("Import the offline Android image export with prepare-native-runtime.py first.")
    work = root / "run"
    work.mkdir(exist_ok=True)
    launched = False
    with file_lock(work / "launcher.lock"):
        profile = read_profile(root)
        if args.action == "start":
            launched = start(args, root, work, profile)
        elif args.action == "stop":
            stop(root, work, profile)
        elif args.action == "status":
            status(root, work, profile)
        elif args.command:
            print(guest(work, profile["console_port"], args.command, 30))
        else:
            parser.error("console requires --command")
    if launched and args.wait:
        wait_boot(root, work, profile, args.wait)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        raise SystemExit(str(error))
