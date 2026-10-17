"""Keypad Bridge flasher: a small window that writes the ESP32 firmware to a board over USB.

Run with Python (needs pyserial and esptool):  python flasher.py
or build a stand-alone Windows .exe with build_exe.ps1 (the firmware images are bundled inside it).
"""
import os, queue, sys, threading, time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import serial.tools.list_ports

APP = "Keypad Bridge flasher"
DONE = object()
# flash offsets for a plain ESP32 (esp32dev)
IMAGES = [("0x1000", "bootloader.bin"), ("0x8000", "partitions.bin"), ("0xe000", "boot_app0.bin"), ("0x10000", "firmware.bin")]


def bundle_dir():
    """Folder with the firmware images: next to the exe/script, or inside the PyInstaller bundle."""
    for d in (os.path.join(os.path.dirname(os.path.abspath(sys.argv[0])), "firmware"),
              os.path.join(os.path.dirname(os.path.abspath(sys.argv[0])), "..", "firmware"),
              os.path.join(getattr(sys, "_MEIPASS", ""), "firmware")):
        if d and all(os.path.exists(os.path.join(d, f)) for _, f in IMAGES):
            return os.path.abspath(d)
    return None


class Capture:
    """File-like object that sends esptool's output to the log box."""
    def __init__(self, q): self.q = q
    def write(self, s):
        if s: self.q.put(s)
    def flush(self): pass
    def isatty(self): return False


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP)
        self.resizable(False, False)
        self.q = queue.Queue()
        self.busy = False
        self.fwdir = bundle_dir()
        pad = {"padx": 10, "pady": 4}

        frm = ttk.Frame(self, padding=10)
        frm.grid(sticky="nsew")
        ttk.Label(frm, text="1. Plug the ESP32 into USB, then pick its port:").grid(row=0, column=0, columnspan=3, sticky="w", **pad)
        self.port = tk.StringVar()
        self.ports = ttk.Combobox(frm, textvariable=self.port, width=48, state="readonly")
        self.ports.grid(row=1, column=0, columnspan=2, sticky="we", **pad)
        ttk.Button(frm, text="Refresh", command=self.refresh).grid(row=1, column=2, **pad)

        ttk.Label(frm, text="2. Firmware:").grid(row=2, column=0, sticky="w", **pad)
        self.fwlabel = ttk.Label(frm, text="", foreground="gray")
        self.fwlabel.grid(row=2, column=1, sticky="w", **pad)
        ttk.Button(frm, text="Choose folder...", command=self.choose).grid(row=2, column=2, **pad)

        self.erase = tk.BooleanVar(value=False)
        ttk.Checkbutton(frm, text="Erase everything first (also wipes saved Wi-Fi and settings)", variable=self.erase)\
            .grid(row=3, column=0, columnspan=3, sticky="w", **pad)
        self.noreset = tk.BooleanVar(value=False)
        ttk.Checkbutton(frm, text="Board is already in download mode (I held BOOT and tapped EN)", variable=self.noreset)\
            .grid(row=4, column=0, columnspan=3, sticky="w", **pad)

        self.btn = ttk.Button(frm, text="3. Write firmware", command=self.start)
        self.btn.grid(row=5, column=0, columnspan=3, sticky="we", **pad)
        self.bar = ttk.Progressbar(frm, mode="indeterminate")
        self.bar.grid(row=6, column=0, columnspan=3, sticky="we", **pad)

        self.log = tk.Text(frm, width=78, height=18, state="disabled", font=("Consolas", 9), wrap="word")
        self.log.grid(row=7, column=0, columnspan=3, sticky="nsew", **pad)
        sb = ttk.Scrollbar(frm, command=self.log.yview)
        sb.grid(row=7, column=3, sticky="ns")
        self.log["yscrollcommand"] = sb.set

        self.show_fw()
        self.refresh()
        self.after(100, self.drain)
        self.say("Ready. After flashing, the bridge starts a Wi-Fi network called KeypadBridge-XXXX; "
                 "join it with your phone to set it up.\n")

    # ---- helpers
    def say(self, s):
        self.log["state"] = "normal"
        self.log.insert("end", s)
        self.log.see("end")
        self.log["state"] = "disabled"

    def drain(self):
        try:
            while True:
                s = self.q.get_nowait()
                if s is DONE:                   # worker finished; tkinter widgets must be touched from this thread
                    self.finish()
                    continue
                if s.startswith("\r"):          # esptool progress lines overwrite themselves
                    self.log["state"] = "normal"
                    self.log.delete("end-1l linestart", "end-1c")
                    self.log["state"] = "disabled"
                    s = s.lstrip("\r")
                self.say(s)
        except queue.Empty:
            pass
        self.after(100, self.drain)

    def show_fw(self):
        if self.fwdir:
            v = os.path.join(self.fwdir, "version.txt")
            ver = open(v).read().strip() if os.path.exists(v) else "unknown version"
            mei = getattr(sys, "_MEIPASS", None)
            self.fwlabel["text"] = "%s  (%s)" % (ver, "bundled" if mei and self.fwdir.startswith(os.path.abspath(mei)) else self.fwdir)
        else:
            self.fwlabel["text"] = "not found - choose the firmware folder"

    def choose(self):
        d = filedialog.askdirectory(title="Folder containing bootloader.bin, partitions.bin, boot_app0.bin and firmware.bin")
        if not d: return
        missing = [f for _, f in IMAGES if not os.path.exists(os.path.join(d, f))]
        if missing:
            messagebox.showerror(APP, "That folder is missing: " + ", ".join(missing))
            return
        self.fwdir = d
        self.show_fw()

    def refresh(self):
        found = []
        for p in sorted(serial.tools.list_ports.comports(), key=lambda p: p.device):
            found.append("%s - %s" % (p.device, p.description))
        self.ports["values"] = found
        if found:
            # prefer a USB-serial chip typically found on ESP32 boards
            pick = next((f for f in found if any(k in f for k in ("CP210", "CH340", "CH9102", "USB Serial", "USB-SERIAL"))), found[0])
            self.port.set(pick)
        else:
            self.port.set("")
            self.say("No serial ports found. Plug the board in (use a data cable, not a charge-only one) and press Refresh.\n")

    # ---- flashing
    def start(self):
        if self.busy: return
        if not self.port.get():
            messagebox.showwarning(APP, "Pick the ESP32's port first.")
            return
        if not self.fwdir:
            messagebox.showwarning(APP, "Choose the folder with the firmware files first.")
            return
        port = self.port.get().split(" - ")[0]
        self.busy = True
        self.btn["state"] = "disabled"
        self.bar.start(10)
        self.say("\n==== Writing firmware to %s ====\n" % port)
        threading.Thread(target=self.flash, args=(port,), daemon=True).start()

    def flash(self, port):
        import esptool
        old = sys.stdout, sys.stderr
        sys.stdout = sys.stderr = Capture(self.q)
        ok = False
        try:
            base = ["--chip", "esp32", "--port", port, "--baud", "460800",
                    "--before", "no_reset" if self.noreset.get() else "default_reset", "--after", "hard_reset"]
            if self.erase.get():
                self.q.put("Erasing flash...\n")
                esptool.main(base + ["erase_flash"])
                time.sleep(1)
            args = base + ["write_flash", "-z", "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "detect"]
            for off, f in IMAGES:
                args += [off, os.path.join(self.fwdir, f)]
            esptool.main(args)
            ok = True
        except SystemExit as e:
            self.q.put("\nesptool stopped (%s)\n" % e)
        except Exception as e:
            self.q.put("\nError: %s\n" % e)
        finally:
            sys.stdout, sys.stderr = old
        if ok:
            self.q.put("\n==== Done. The board is restarting with the new firmware. ====\n"
                       "Next: on your phone, join the Wi-Fi network KeypadBridge-XXXX and the setup page opens "
                       "(or browse to http://192.168.4.1).\n")
        else:
            self.q.put("\nFlashing failed. Things to try:\n"
                       " - Wrong port? Unplug the board, press Refresh, see which port disappears, plug it back in.\n"
                       " - Stuck at 'Connecting...'? Hold the BOOT button on the board, tap EN (reset), release BOOT,\n"
                       "   tick 'Board is already in download mode' and press Write firmware again.\n"
                       " - Close any serial monitor or other program using the port.\n"
                       " - Try a different USB cable or port.\n")
        self.q.put(DONE)

    def finish(self):
        self.bar.stop()
        self.btn["state"] = "normal"
        self.busy = False


if __name__ == "__main__":
    App().mainloop()
