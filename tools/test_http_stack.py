"""Check HTTP and MQTT save stacks against the observed flash-compaction path.

Run after `pio run -e huzzah_rtos`. This catches the known stack regression;
device watermarks must still cover other LittleFS states and interrupt paths.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / ".pio" / "build" / "huzzah_rtos"

SAVE_HANDLERS = (
    "apply", "applyrelay", "apply_inputs", "apply_temperature",
    "timer_save", "timer_save_form", "automation_rule_save",
    "automation_remote_save", "backup_restore", "reset_config",
)

# Decoded from the Config-save stack-canary backtrace, excluding apply itself.
COMPACTION_PATH = (
    ("sc_config.c", "sc_config_replace"),
    ("sc_config.c", "persist_file"),
    ("sc_littlefs.c", "sc_lfs_rename"),
    ("lfs.c", "lfs_rename"),
    ("lfs.c", "lfs_dir_commit"),
    ("lfs.c", "lfs_dir_orphaningcommit"),
    ("lfs.c", "lfs_dir_relocatingcommit"),
    ("lfs.c", "lfs_dir_compact"),
    ("lfs.c", "lfs_dir_traverse"),
    ("lfs.c", "lfs_dir_commit_commit"),
    ("lfs.c", "lfs_dir_commitattr"),
    ("lfs.c", "lfs_dir_commitprog"),
    ("lfs.c", "lfs_bd_prog"),
    ("lfs.c", "lfs_bd_flush"),
    ("sc_littlefs.c", "pr"),
    ("partition.c", "esp_partition_write"),
    ("spi_flash.c", "spi_flash_write"),
    ("spi_flash.c", "spi_flash_check_wr_protect"),
    ("spi_flash.c", "special_flash_read_status"),
)

HTTP_DISPATCH_ALLOWANCE = 512
INTERRUPT_ALLOWANCE = 256
FILESYSTEM_ALLOWANCE = 512
MIN_HEADROOM = 512


def reports():
    frames = {}
    paths = list(BUILD.rglob("*.su"))
    if not paths:
        raise AssertionError("No stack-usage reports; build huzzah_rtos first")
    for path in paths:
        if "bootloader" in path.parts:
            continue
        for line in path.read_text().splitlines():
            location, size, kind = line.split("\t")
            filename, _, _, name = location.rsplit(":", 3)
            key = (Path(filename).name, name.split(".", 1)[0])
            assert kind == "static" or "bounded" in kind, (
                f"Unbounded stack frame: {location}"
            )
            frames[key] = max(frames.get(key, 0), int(size))
    return frames


def mqtt_save_budget(frames, source):
    call = re.search(r'xTaskCreate\(\s*pubtask\s*,\s*"mqpub"\s*,\s*(\w+)', source)
    assert call, "MQTT publisher task creation not found"
    argument = call.group(1)
    if argument.isdecimal():
        stack = int(argument)
    else:
        constant = re.search(rf"^#define\s+{re.escape(argument)}\s+(\d+)\s*$",
                             source, re.MULTILINE)
        assert constant, f"Cannot resolve MQTT stack size: {argument}"
        stack = int(constant.group(1))
    # The timing setter replaces sc_config_replace in the decoded save chain.
    save_path = sum(frames[key] for key in COMPACTION_PATH
                    if key != ("sc_config.c", "sc_config_replace"))
    used = (frames[("sc_mqtt.c", "pubtask")]
            + frames[("sc_config.c", "sc_config_set_relay_timing")] + save_path)
    remaining = stack - used - INTERRUPT_ALLOWANCE - FILESYSTEM_ALLOWANCE
    assert remaining >= MIN_HEADROOM, (
        f"mqpub: stack {stack}, save frames {used}, budgeted headroom "
        f"{remaining}; need {MIN_HEADROOM} bytes"
    )
    return stack, used, remaining


def main():
    frames = reports()
    web = (ROOT / "components/sc_web/sc_web.c").read_text()
    stack = int(re.search(r"c\.stack_size\s*=\s*(\d+)", web).group(1))
    compaction = sum(frames[key] for key in COMPACTION_PATH)
    allowance = (compaction + HTTP_DISPATCH_ALLOWANCE + INTERRUPT_ALLOWANCE
                 + FILESYSTEM_ALLOWANCE)
    largest = max(frames[("sc_web.c", name)] for name in SAVE_HANDLERS)
    # Verify this guard rejects the size that triggered the reported panic.
    assert largest + allowance + MIN_HEADROOM > 7168
    for name in SAVE_HANDLERS:
        frame = frames[("sc_web.c", name)]
        remaining = stack - frame - allowance
        assert remaining >= MIN_HEADROOM, (
            f"{name}: HTTP stack {stack}, frame {frame}, compaction/allowances "
            f"{allowance}, remaining {remaining}; need {MIN_HEADROOM} bytes"
        )
        print(f"{name}: frame={frame}, budgeted headroom={remaining} bytes")
    print(f"PASS: HTTP stack={stack}, decoded compaction frames={compaction} bytes")
    mqtt = (ROOT / "components/sc_mqtt/sc_mqtt.c").read_text()
    stack, used, remaining = mqtt_save_budget(frames, mqtt)
    print(f"PASS: MQTT publisher stack={stack}, save frames={used}, "
          f"budgeted headroom={remaining} bytes")
    old = re.sub(r'(xTaskCreate\(\s*pubtask\s*,\s*"mqpub"\s*,\s*)\w+',
                 r'\g<1>1792', mqtt, count=1)
    try:
        mqtt_save_budget(frames, old)
    except AssertionError:
        print("PASS: guard rejects the previous 1792-byte MQTT stack")
    else:
        raise AssertionError("Guard accepted the overflowing MQTT stack")


if __name__ == "__main__":
    main()
