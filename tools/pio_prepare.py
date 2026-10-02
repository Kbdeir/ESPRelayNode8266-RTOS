"""PlatformIO pre-build dependency preparation.

The application is an ESP8266_RTOS_SDK 3.4 project, so MQTT and LittleFS are
IDF components under components/, not Arduino/PlatformIO libraries.

Dependencies are pinned deliberately for reproducible long-running firmware.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys

Import("env")  # type: ignore  # provided by PlatformIO/SCons

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
COMPONENTS = PROJECT_DIR / "components"
SDKCONFIG = PROJECT_DIR / ("sdkconfig.%s" % env.subst("$PIOENV"))

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

for dep in DEPS:
    fetch(dep)

run([sys.executable, PROJECT_DIR / "tools" / "embed_web_assets.py"])
print("[pio_prepare] ESP8266 RTOS component dependencies ready")
