# Builds KeypadBridgeFlasher.exe (single file, firmware images bundled inside) into the folder above.
# Needs Python 3 with:  pip install pyserial esptool pyinstaller
# Run after `pio run` and `.\update_firmware.ps1` in the folder above.
# Build scratch goes to %TEMP% because OneDrive-synced folders lock files mid-build.
param([string]$Python = "python")
$fw = Join-Path $PSScriptRoot "..\firmware"
if (-not (Test-Path (Join-Path $fw "firmware.bin"))) { Write-Error "No firmware images in $fw - run ..\update_firmware.ps1 first"; exit 1 }
$tmp = Join-Path $env:TEMP "kbflasher-build"
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
& $Python -m PyInstaller --noconfirm --onefile --windowed `
  --name KeypadBridgeFlasher `
  --distpath (Join-Path $tmp "dist") --workpath (Join-Path $tmp "work") --specpath $tmp `
  --add-data "$(Resolve-Path $fw);firmware" `
  --hidden-import serial.tools.list_ports `
  (Join-Path $PSScriptRoot "flasher.py")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$out = Join-Path $PSScriptRoot "..\KeypadBridgeFlasher.exe"
Copy-Item (Join-Path $tmp "dist\KeypadBridgeFlasher.exe") $out -Force
Write-Host "Built: $(Resolve-Path $out)"
