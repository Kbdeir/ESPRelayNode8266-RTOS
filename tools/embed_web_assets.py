"""Regenerate firmware-embedded web assets from data/*.html."""

import json
from pathlib import Path


PROJECT_DIR = Path(__file__).resolve().parent.parent
ASSETS = (
    ("sc_config_html", "config.html"),
    ("sc_live_html", "LiveReadings.html"),
    ("sc_relay_html", "RelayConfig.html"),
    ("sc_inputs_html", "Input_Relays_Map.html"),
    ("sc_temperature_html", "AdvancedTemperatureConf.html"),
    ("sc_timer_html", "Timer1.html"),
    ("sc_timer_saved_html", "savetimer.html"),
    ("sc_files_html", "Files.html"),
    ("sc_backup_html", "Backup.html"),
    ("sc_coredump_html", "coredump.html"),
    ("sc_ota_html", "FirmwareMaintenance.html"),
)


def main() -> None:
    declarations = []
    for symbol, filename in ASSETS:
        content = (PROJECT_DIR / "data" / filename).read_text(encoding="utf-8")
        declarations.append(
            f"const char {symbol}[] = {json.dumps(content, ensure_ascii=False)};"
        )
    output = PROJECT_DIR / "components" / "sc_webfs" / "web_assets.c"
    generated = "\n".join(declarations) + "\n"
    if not output.exists() or output.read_text(encoding="utf-8") != generated:
        output.write_text(generated, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
