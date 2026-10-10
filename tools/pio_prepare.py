"""PlatformIO pre-build dependency preparation.

The application is an ESP8266_RTOS_SDK 3.4 project, so MQTT and LittleFS are
IDF components under components/, not Arduino/PlatformIO libraries.

Dependencies are pinned deliberately for reproducible long-running firmware.
"""
from pathlib import Path
import csv
import os
import shutil
import subprocess
import sys
from SCons.Script import COMMAND_LINE_TARGETS

Import("env")  # type: ignore  # provided by PlatformIO/SCons

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
COMPONENTS = PROJECT_DIR / "components"
SDKCONFIG = PROJECT_DIR / ("sdkconfig.%s" % env.subst("$PIOENV"))


def configure_webfs_geometry():
    with (PROJECT_DIR / "partitions.csv").open(newline="", encoding="utf-8") as source:
        for row in csv.reader(source):
            if not row or row[0].strip().startswith("#"):
                continue
            if len(row) < 5:
                raise RuntimeError("invalid partition table row")
            if row[1].strip() == "app":
                start = int(row[3].strip(), 0)
                size = int(row[4].strip(), 0)
                if (start % 0x100000) + size > 0x100000:
                    raise RuntimeError("OTA partition crosses ESP8266 1 MB startup flash window")
            if row[0].strip() != "webfs":
                continue
            if row[1].strip() != "data":
                raise RuntimeError("webfs partition must be a data partition")
            start = int(row[3].strip(), 0)
            size = int(row[4].strip(), 0)
            if start % 4096 or size % 4096 or start + size > 0x400000:
                raise RuntimeError("invalid webfs partition geometry")
            # The ESP8266 PlatformIO filesystem builder converts mapped
            # addresses back to flash offsets; ESP-IDF has no FS linker symbols.
            mapped = 0x40200000 + start
            env.Replace(FS_START=mapped, FS_END=mapped + size,
                        FS_PAGE=256, FS_BLOCK=4096,
                        WEBFS_OFFSET=hex(start), WEBFS_SIZE=str(size))
            return
    raise RuntimeError("webfs partition not found in partitions.csv")

DEPS = [
    {
        "name": "littlefs",
        "path": COMPONENTS / "littlefs" / "littlefs",
        "check": COMPONENTS / "littlefs" / "littlefs" / "lfs.c",
        "url": "https://github.com/littlefs-project/littlefs.git",
        "ref": "v2.11.3",
    },
]


def run(cmd, cwd=None):
    print("[pio_prepare]", " ".join(str(x) for x in cmd))
    subprocess.check_call(cmd, cwd=str(cwd) if cwd else None)


def ensure_git():
    if shutil.which("git") is None:
        raise RuntimeError(
            "Git is required for the first PlatformIO build so pinned IDF "
            "components can be fetched. Install Git and rerun the build."
        )


def fetch(dep):
    if dep["check"].exists():
        return
    ensure_git()
    target = dep["path"]
    if target.exists():
        shutil.rmtree(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    run([
        "git", "clone", "--depth", "1", "--branch", dep["ref"],
        dep["url"], str(target)
    ])
    if not dep["check"].exists():
        raise RuntimeError("Dependency %s was fetched but is incomplete" % dep["name"])


SDKCONFIG.touch(exist_ok=True)
configure_webfs_geometry()

if "uploadfs" in COMMAND_LINE_TARGETS:
    platform = env.PioPlatform()
    esptool = Path(platform.get_package_dir("tool-esptoolpy")) / "esptool.py"
    mkfs = Path(platform.get_package_dir("tool-mklittlefs")) / (
        "mklittlefs.exe" if os.name == "nt" else "mklittlefs")
    env.Replace(
        UPLOAD_PROTOCOL="custom",
        UPLOADCMD=(
            '"$PYTHONEXE" "$PROJECT_DIR/tools/upload_webfs.py" '
            '"$UPLOAD_PORT" "$SOURCE" '
            '"%s" "%s" "$WEBFS_OFFSET" "$WEBFS_SIZE"'
        ) % (esptool, mkfs),
    )

for dep in DEPS:
    fetch(dep)

run([sys.executable, PROJECT_DIR / "tools" / "embed_web_assets.py"])
print("[pio_prepare] ESP8266 RTOS component dependencies ready")
