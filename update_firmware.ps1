# Copies the images from the last `pio run` into firmware\ (what the flasher and the release zip ship).
$b = Join-Path $PSScriptRoot ".pio\build\esp32dev"
$dst = Join-Path $PSScriptRoot "firmware"
$app0 = "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"
if (-not (Test-Path "$b\firmware.bin")) { Write-Error "No build found - run 'pio run' first"; exit 1 }
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item "$b\bootloader.bin", "$b\partitions.bin", "$b\firmware.bin", $app0 $dst
$ver = (Select-String -Path (Join-Path $PSScriptRoot "src\shared.h") -Pattern 'FW_VERSION "([^"]+)"').Matches[0].Groups[1].Value
"Keypad Bridge firmware $ver  (built $(Get-Date -Format 'yyyy-MM-dd'))" | Set-Content (Join-Path $dst "version.txt")
Get-ChildItem $dst | Select-Object Name, Length
