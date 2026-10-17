# Flash a built firmware when the ESP32 will not enter download mode by itself.
# 1. Build first:  pio run
# 2. Hold BOOT, tap EN, release BOOT (the board is now waiting in download mode).
# 3. Run:  .\flash_noreset.ps1 [COM port]
param([string]$Port = "COM6")
$b = "$PSScriptRoot\.pio\build\esp32dev"
$app0 = "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"
python -m esptool --chip esp32 --port $Port --baud 460800 --before no_reset --after hard_reset write_flash -z `
  0x1000 "$b\bootloader.bin" 0x8000 "$b\partitions.bin" 0xe000 $app0 0x10000 "$b\firmware.bin"
