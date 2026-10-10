"""Safely update web pages in the ESP8266 RTOS LittleFS partition."""

import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path


FLASH_SIZE = 0x400000
PARTITION_TABLE = 0x8000
PROJECT_DIR = Path(__file__).resolve().parent.parent


def run(args, cwd=None):
    print("[upload_webfs]", " ".join(str(arg) for arg in args), flush=True)
    subprocess.run([str(arg) for arg in args], cwd=cwd, check=True)


def serial_port(requested):
    if requested:
        return requested
    from serial.tools import list_ports

    ports = list(list_ports.comports())
    if len(ports) != 1:
        raise RuntimeError("specify the board with --upload-port COM3")
    print("[upload_webfs] using", ports[0].device, ports[0].description)
    return ports[0].device


def check_partition_table(flash, offset, size):
    for pos in range(PARTITION_TABLE, PARTITION_TABLE + 0x1000, 32):
        magic, kind, subtype, start, length, label, _ = struct.unpack_from(
            "<HBBII16sI", flash, pos)
        if magic != 0x50AA:
            continue
        if label.split(b"\0", 1)[0] == b"webfs":
            if kind != 1 or subtype != 0x40 or start != offset or length != size:
                raise RuntimeError("board webfs partition does not match partitions.csv")
            return
    raise RuntimeError("board has no matching webfs partition; no flash written")


def file_hash(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_updated_image(mkfs, existing, built, output, size, work):
    existing, built, output = existing.resolve(), built.resolve(), output.resolve()
    if all(byte == 0xff for byte in existing.read_bytes()):
        shutil.copyfile(built, output)
        print("[upload_webfs] blank webfs; using built image")
        return

    files = work / "files"
    files.mkdir()
    run([mkfs, "-u", "files", "-p", 256, "-b", 4096, "-s", size, existing],
        cwd=work)
    protected = {}
    for path in files.rglob("*"):
        if path.is_file():
            protected[path.relative_to(files)] = file_hash(path)
    for path in (PROJECT_DIR / "data").rglob("*"):
        if path.is_file():
            destination = files / path.relative_to(PROJECT_DIR / "data")
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            protected.pop(destination.relative_to(files), None)
    run([mkfs, "-c", "files", "-p", 256, "-b", 4096, "-s", size, output],
        cwd=work)
    verified = work / "verified"
    verified.mkdir()
    run([mkfs, "-u", "verified", "-p", 256, "-b", 4096, "-s", size, output],
        cwd=work)
    for name, digest in protected.items():
        candidate = verified / name
        if not candidate.is_file() or file_hash(candidate) != digest:
            raise RuntimeError("existing file changed during repack: %s" % name)
    print("[upload_webfs] preserved %u existing file(s)" % len(protected))


def main():
    if len(sys.argv) != 7:
        raise RuntimeError("expected port, built image, esptool, mklittlefs, offset, size")
    _, requested_port, built_name, esptool_name, mkfs_name, offset_text, size_text = sys.argv
    offset, size = int(offset_text, 0), int(size_text, 0)
    built, esptool, mkfs = map(Path, (built_name, esptool_name, mkfs_name))
    if not built.is_file() or built.stat().st_size != size:
        raise RuntimeError("built LittleFS image size does not match webfs partition")
    if not esptool.is_file() or not mkfs.is_file():
        raise RuntimeError("esptool or mklittlefs is missing")
    port = serial_port(requested_port)
    backup_dir = PROJECT_DIR / ".pio" / "flash-backups"
    backup_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    backup = backup_dir / ("flash-%s-%s.bin" % (port, stamp))
    base = [sys.executable, esptool, "--chip", "esp8266", "--port", port,
            "--baud", "115200", "--before", "default_reset"]
    print("[upload_webfs] close any serial monitor before continuing")
    run(base + ["--after", "no_reset", "read_flash", "0x0",
                hex(FLASH_SIZE), backup])
    flash = backup.read_bytes()
    if len(flash) != FLASH_SIZE:
        raise RuntimeError("incomplete flash backup; no flash written")
    print("[upload_webfs] full flash backup:", backup)
    print("[upload_webfs] SHA-256:", hashlib.sha256(flash).hexdigest())
    check_partition_table(flash, offset, size)

    with tempfile.TemporaryDirectory(prefix="webfs-", dir=str(backup_dir)) as temp:
        work = Path(temp)
        existing = work / "existing.bin"
        existing.write_bytes(flash[offset:offset + size])
        updated = work / "updated.bin"
        make_updated_image(mkfs, existing, built, updated, size, work)
        if updated.stat().st_size != size:
            raise RuntimeError("new filesystem image has wrong size")
        run(base + ["--after", "hard_reset", "write_flash", "-z",
                    hex(offset), updated])
    print("[upload_webfs] upload complete; backup kept at", backup)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print("[upload_webfs] ERROR:", error, file=sys.stderr)
        sys.exit(1)
