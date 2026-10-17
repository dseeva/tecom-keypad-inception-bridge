# Tecom keypad ↔ Inner Range Inception bridge (ESP32)

Turns a Tecom / Challenger **TS0804 LCD keypad** into a keypad for an **Inner Range Inception** controller.
An ESP32 pretends to be the keypad's panel on its RS485 bus and talks to Inception's REST API over Wi-Fi.

- Arm and disarm areas, open or lock doors, from the keypad (`[slot] #`, code, `#` or `*`).
- PIN codes checked against Inception users (or a local list).
- Live screen text with date, time, open inputs, faults and area states.
- All 24 keypad LEDs driven by Inception areas or inputs, solid or flashing.
- Everything is set up from a web page: first boot opens a setup Wi-Fi network with a captive portal,
  after that the page is on your home network at `http://keypad-bridge.local` (the name is configurable).

> **The keypad must be disconnected from any real Challenger panel.** The bridge *transmits* on the bus
> as if it were the panel. Two panels on one bus will fight, and the real panel may raise tampers or faults.

## 1. Parts

| Part | Notes |
|---|---|
| ESP32 dev board | "ESP32 DevKit" / NodeMCU-32S style with a USB port (the `esp32dev` board in PlatformIO) |
| RS485 module, **3.3 V capable** | Two kinds work: an **auto-direction** module (4 pins: VCC GND TXD RXD, usually MAX3485 / MAX13487) or a **MAX485 module with DE/RE pins** (set `BUS_DE_PIN` in `src/shared.h`, see §3.3) |
| Tecom TS0804 keypad | with its own 12 V supply |
| 12 V DC supply for the keypad | the keypad draws ~100 mA; a plug pack is fine |
| USB cable | data cable (not charge-only) for flashing |
| A few jumper wires | |

A multimeter is strongly recommended for the first hookup (see §3.2).

## 2. Flash

### 2.1 The easy way: the Windows flasher (no tools to install)

1. Download `KeypadBridgeFlasher.exe` from the [Releases](../../releases) page (or build it, see below).
   The firmware is bundled inside it.
2. Plug the ESP32 into USB with a **data** cable. Windows normally installs the driver itself; if no new COM port
   appears, install the driver for your board's USB chip (CP210x from Silicon Labs, or CH340).
3. Run the flasher, pick the board's port (it preselects the likely one), press **Write firmware**.
4. If it sits at `Connecting....` and gives up: hold the **BOOT** button, tap **EN**, release BOOT, tick
   *Board is already in download mode* and press Write firmware again.

The flasher is `flasher/flasher.py` (Python + tkinter + esptool). `flasher/build_exe.ps1` builds the exe with
PyInstaller after `update_firmware.ps1` has copied a fresh build into `firmware/`. Windows SmartScreen may warn
about an unsigned exe; "More info → Run anyway".

### 2.2 With PlatformIO (to build from source)

Install [PlatformIO](https://platformio.org/install/cli) (or the VS Code extension), then in this folder:

```
pio run                                     # compile
pio run -t upload --upload-port COM6        # flash over USB (use your port: COMx on Windows, /dev/ttyUSB0 on Linux)
pio device monitor -p COM6                  # watch the log at 115200 baud
```

If the upload sits at `Connecting....` the board is not entering download mode by itself: hold **BOOT**,
tap **EN** (reset), release BOOT, then run `pio run -t upload` again straight away. On Windows the
`flash_noreset.ps1` script does the same flash without touching the reset line:
`.\flash_noreset.ps1 COM6` after a `pio run`.

Later updates can be installed from the web page (**Firmware and backup → Upload firmware**) using
`.pio/build/esp32dev/firmware.bin`, no USB needed.

## 3. Wiring

### 3.1 Which keypad terminals

The TS0804 has a terminal block with **+12V**, **0V** (ground), **D+** and **D−** (the RS485 LAN pair).
Some keypads label the pair **LAN+ / LAN−** or **A / B**. If the labels are missing, the bus pair is the two
terminals that are *not* the supply pair; the supply pair is where the 12 V wires from the old panel went.

### 3.2 Before connecting anything: meter check

An earlier RS485 module burnt out during a bench test, most likely because the module's power pins ended
up on the keypad's 12 V, or the module's supply and ground were swapped. Take two minutes with a meter:

1. Power the keypad from its 12 V supply **only**. Nothing else connected.
2. Meter on DC volts, **black probe on keypad 0V**. Measure each terminal with the red probe:
   - **+12V**: about 12 to 14 V.
   - **D+** and **D−**: each between about **1 and 4 V** when idle. If either reads near 12 V, you have the wrong terminal, stop.
3. Power the ESP32 from USB. With the black probe still on keypad 0V and **no wire yet between the two**,
   touch the red probe to an ESP32 **GND** pin. Expect close to 0 V (a few hundred mV is fine).
   Several volts means the two supplies are not referenced together; use the same plug board / power strip,
   or power the keypad from a separate isolated plug pack, and measure again.
4. Check the RS485 module's **VCC** and **GND** pins against its silk-screen before plugging it in.
   VCC must go to the ESP32's **3V3** pin (or 5V only if the module is marked 5 V *and* its RO/RXD output is
   3.3 V tolerant). **Never** connect the module's VCC to the keypad's +12V.

### 3.3 Connections

Do this with **everything unpowered**. Double-check each wire against the table before powering up.

**Auto-direction module (4 pins):**

| RS485 module | ESP32 | Keypad |
|---|---|---|
| VCC | 3V3 | |
| GND | GND | 0V (run one wire from ESP32 GND to keypad 0V) |
| RXD (or RO) | GPIO16 | |
| TXD (or DI) | GPIO17 | |
| A | | D+ |
| B | | D− |

**MAX485 module with DE and RE pins:** same as above, plus join **DE** and **RE** together and wire them
to a spare GPIO (GPIO4 is a good choice), then set `#define BUS_DE_PIN 4` in `src/shared.h` and rebuild.

The keypad's +12V and 0V go to the 12 V supply. **The ESP32 is powered by USB, never by the 12 V.**

Notes:
- If the keypad never answers (the web page shows polls but 0 replies), swap A and B. Some modules have the
  labels the wrong way round; swapping them cannot hurt anything.
- Use a twisted pair for A/B if the run is more than a metre.
- The keypad takes about 10 s after power-on before it talks. Don't press `*` on its start-up screen.
- The bus idles at mark (D+ higher than D−). Wired backwards, the ESP32 reads its own poll back as `a8 f3 21 00`.

## 4. First-time setup (captive portal)

1. Power the ESP32 (keypad can stay unplugged for now).
2. On a phone or laptop, join the open Wi-Fi network **`KeypadBridge-XXXX`** (XXXX = the last digits of the
   board's MAC address, also printed in the serial log). The setup page opens by itself; if it does not,
   browse to **http://192.168.4.1**.
3. In **Wi-Fi and device**, press **Scan**, pick your home network, type its password, optionally change the
   **Device name**, and press **Save Wi-Fi**. The bridge restarts and joins your network.
4. Reconnect your phone to your home Wi-Fi and open **http://keypad-bridge.local** (or the name you chose).
   If `.local` names don't resolve on your network, the IP address is shown in the serial log and on the
   keypad screen for 10 s after it connects; your router's device list will also show it under the hostname.

If the saved network can't be reached for 30 s (new router, wrong password) the setup network comes back
on while it keeps trying, so you can always get in. Holding the **BOOT** button for 5 s wipes the
Wi-Fi settings and restarts in setup mode.

## 5. Settings page guide

**Status** shows exactly what the keypad screen shows, poll/reply counters (replies should climb once the
keypad is wired), the Inception connection, Wi-Fi, address and time. The log is under the fold.

**Wi-Fi and device**
- *Device name*: hostname and `.local` address. Letters, numbers and dashes.
- *Time zone*: POSIX string, used for `{date}` and `{time}` on the screen. Defaults to Sydney/Melbourne.
- *Settings page password*: optional. When set, the browser asks for user `admin` and this password.
  Recommended if other people use your Wi-Fi: the page holds your Inception login.

**Inception**
- *Address*: the Inception controller's IP or hostname, e.g. `192.168.1.50` (or `https://…` if you use HTTPS).
- *API username / password*: an Inception **user with "Web page / REST API" access**. In Inception's web UI:
  Configuration → Users → add a user, give it a password, and allow the areas and doors it may control.
  Press **Test connection** to check before saving.

**Screen**: three message templates, `|` splits the two lines, placeholders are filled in live:
`{date}` `{time}` `{area}` (slot 1's area name) `{state}` `{unsealed}` (open inputs, cycled one at a time
when they don't fit) `{faults}` `{areas}` (every area slot and its state).

**Codes**
- *PIN check*: **Inception user PIN codes** asks Inception whether a user has that PIN (the user must have a
  PIN set in Inception). **PIN codes listed below** uses the local comma-separated list instead.
- *12-key keypad*: in the default mode, code then `#` arms and code then `*` disarms. In toggle mode `#`
  flips the area and `*` clears.
- *Door slots*: whether `code #` on a door slot **opens** the door (momentary unlock, re-locks by itself) or
  **unlocks** it until `code *` locks it again.

**Keypad areas and doors**: eight slots. Assign an Inception **area** or **door** to each. On the keypad,
`3 #` selects slot 3, then `code #` acts on it. A code with no slot selected uses slot 1. The selection
times out after 20 s.

**LEDs**: pick what drives each of the 24 LEDs (COMMS, RAS, DGP, BATTERY, ACCESS, 24HR, SECURE, TAMPER and
areas 1–16). An area source lights solid when armed and flashes in alarm or entry/exit delay; an input source
lights while the input is active, solid or flashing as chosen. Inputs placed on COMMS/RAS/DGP/BATTERY also
trigger the fault message. **LED test** steps through every LED bit so you can map them.

**Firmware and backup**: upload a new `firmware.bin`, paste a `bridge_config.json` from the PC bridge to
import its settings, or restart.

## 6. Where settings are stored

In the ESP32's flash (LittleFS, `/config.json`), including the Wi-Fi and Inception passwords. Nothing in
this repository holds a secret; `.gitignore` also blocks any `config.json` or `secrets*` file you might create.
To wipe everything: `pio run -t erase` then flash again.

## 7. Protocol notes (Tecom Challenger RAS LAN)

- 4800 baud, 8N1, plain RS485. Packet `5e DEST CMD [LEN DATA…] CHK`, `CHK = ~(XOR of every byte after 5e)`.
  Keypad address `0x10`.
- Poll `5e 90 00 6f` → `00` (no key) or `07 N k1..kN CHK` (queued key presses). The panel must ack with
  `5e 10 10 ff` or the keypad stops sending after 10 keys.
- Key codes: `0x30–0x39` digits, `3a` MENU (`*`), `3b` ENTER (`#`), `3c` NEXT, `3d` CLEAR, `3e` ON, `3f` OFF.
- Screen `5e 10 01 LEN 0b <ASCII> CHK` (`0b` clear/home, `0a` new line, 40 columns × 2 lines).
- LEDs `5e 10 11 07 d0..d6 CHK`, 2 bits per LED (`01` steady, `10` flashing): d1 COMMS RAS DGP BATTERY,
  d2 ACCESS 24HR SECURE TAMPER, d3–d6 areas 1–16.
- Every ~160 ms cycle ends with `5e a1 00 80 de`, as the real panel does.

## 8. Source layout

| File | What |
|---|---|
| `src/main.cpp` | boot, Wi-Fi / setup network, mDNS, BOOT button |
| `src/keypad.cpp` | RS485 bus, polling, key handling, screen and LED packets |
| `src/inception.cpp` | Inception REST API in its own task: states, arm/disarm, doors, PIN check |
| `src/web.cpp`, `src/page.h` | settings page, JSON API, captive portal, OTA upload |
| `src/config.cpp` | settings load/save in flash |
| `src/shared.h` | pin assignments and shared declarations |
| `firmware/` | pre-built images (bootloader, partitions, boot_app0, firmware) and their version |
| `flasher/` | the Windows flasher GUI and its build script |
| `update_firmware.ps1`, `flash_noreset.ps1` | copy a fresh build into `firmware/`; flash a board already in download mode |

## License

MIT, see `LICENSE`.
