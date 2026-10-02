SmartConfig-AI LittleFS web root.

PlatformIO filesystem source directory.
Core pages included in this port:
- config.html
- LiveReadings.html
- RelayConfig.html
- Timer1.html
- Files.html
- Backup.html
- coredump.html
- FirmwareMaintenance.html

The firmware also contains recovery copies of these pages and seeds them only if
missing, so a firmware flash remains recoverable even before the LittleFS image
is uploaded.
