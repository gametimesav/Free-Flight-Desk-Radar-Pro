Flash the combined release binary with esptool or PlatformIO at offset 0x0.

Example:
- esptool.py --chip esp32 write_flash 0x0 Free-Flight-Desk-Radar-Pro-v1.5.bin

This release image includes the application firmware and the LittleFS web assets in a single binary.
