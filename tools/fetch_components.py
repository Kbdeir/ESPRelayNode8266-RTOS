#!/usr/bin/env python3
from pathlib import Path
import shutil, subprocess
ROOT = Path(__file__).resolve().parents[1]
DEPS = [
    (ROOT/'components/mqtt', 'https://github.com/espressif/esp-mqtt.git', 'ESP-MQTT_FOR_IDF_3.1', 'CMakeLists.txt'),
    (ROOT/'components/littlefs/littlefs', 'https://github.com/littlefs-project/littlefs.git', 'v2.11.3', 'lfs.c'),
]
if shutil.which('git') is None:
    raise SystemExit('Git is required.')
for path,url,ref,check in DEPS:
    if (path/check).exists():
        print(f'{path.name}: already present')
        continue
    if path.exists(): shutil.rmtree(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.check_call(['git','clone','--depth','1','--branch',ref,url,str(path)])
print('Dependencies ready.')
