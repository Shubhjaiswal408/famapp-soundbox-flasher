# FamApp Soundbox: Web Flasher

A **techiesms** project · [YouTube](https://www.youtube.com/@techiesms) · [techiesms.com](https://techiesms.com)

Flash the FamApp Soundbox firmware to a classic **ESP32** straight from Chrome or Edge, using [ESP Web Tools](https://esphome.github.io/esp-web-tools/). No Arduino IDE or drivers needed on most computers.

**Flasher page:** https://shubhjaiswal408.github.io/famapp-soundbox-flasher/

> Hobby project. Not affiliated with or endorsed by FamApp or Trio.

---

## 1. What's in this repository

```
famapp-soundbox-flasher/
├── index.html                         ← the flasher web page
├── manifest.json                      ← tells ESP Web Tools which files go where
├── .nojekyll                          ← stops GitHub Pages from processing files
├── README.md                          ← this guide
└── firmware/
    ├── FamApp_Soundbox.bootloader.bin   → 0x1000
    ├── FamApp_Soundbox.partitions.bin   → 0x8000
    ├── boot_app0.bin                    → 0xe000
    ├── FamApp_Soundbox.bin              → 0x10000
    └── FamApp_Soundbox.merged.bin       → 0x0 (manual flashing only)
```

### Flash offsets (classic ESP32)

| File | Offset |
|---|---|
| `FamApp_Soundbox.bootloader.bin` | `"0x1000"` |
| `FamApp_Soundbox.partitions.bin` | `"0x8000"` |
| `boot_app0.bin` | `"0xe000"` |
| `FamApp_Soundbox.bin` | `"0x10000"` |

> **Classic ESP32 bootloader is at `0x1000`, not `0x0`.** `0x0` is only for ESP32-C3 / C6 / S3. Using `0x0` here gives a board that never boots.

The firmware was built with **Board: ESP32 Dev Module** and **Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)**.

---

## 2. manifest.json

```json
{
  "name": "FamApp Soundbox",
  "version": "1.0.0",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "firmware/FamApp_Soundbox.bootloader.bin", "offset": "0x1000" },
        { "path": "firmware/FamApp_Soundbox.partitions.bin", "offset": "0x8000" },
        { "path": "firmware/boot_app0.bin", "offset": "0xe000" },
        { "path": "firmware/FamApp_Soundbox.bin", "offset": "0x10000" }
      ]
    }
  ]
}
```

- `chipFamily` must be exactly `"ESP32"` for this board.
- Offsets are quoted hex strings. ESP Web Tools passes them to esptool-js, which converts them to numbers.
- `path` is relative to `manifest.json`.

---

## 3. Publish on GitHub Pages

1. **Create a repository** on GitHub, e.g. `famapp-soundbox-flasher`. It must be **Public** for free GitHub Pages.
2. **Upload everything** in this folder, keeping the `firmware/` folder.
   - On the website: *Add file → Upload files*, drag the files and the `firmware` folder, then *Commit changes*.
   - Or with git:
     ```bash
     git init
     git add .
     git commit -m "FamApp Soundbox web flasher"
     git branch -M main
     git remote add origin https://github.com/<you>/famapp-soundbox-flasher.git
     git push -u origin main
     ```
3. **Enable GitHub Pages:** repository → *Settings* → *Pages* → *Source: Deploy from a branch* → Branch **main**, folder **/ (root)** → *Save*.
4. Wait 1–2 minutes. The page is at:
   ```
   https://<your-username>.github.io/famapp-soundbox-flasher/
   ```

---

## 4. Check the files are online

Open each of these in the browser. Each must **download a file or show JSON**, not a 404:

```
https://<you>.github.io/famapp-soundbox-flasher/manifest.json
https://<you>.github.io/famapp-soundbox-flasher/firmware/FamApp_Soundbox.bootloader.bin
https://<you>.github.io/famapp-soundbox-flasher/firmware/FamApp_Soundbox.partitions.bin
https://<you>.github.io/famapp-soundbox-flasher/firmware/boot_app0.bin
https://<you>.github.io/famapp-soundbox-flasher/firmware/FamApp_Soundbox.bin
```

- `manifest.json` should show the JSON above.
- File names are **case-sensitive** on GitHub Pages: `FamApp_Soundbox.bin` ≠ `famapp_soundbox.bin`.

---

## 5. Flash a board

1. Use **Chrome or Edge** on a computer.
2. Plug the ESP32 in with a **data** USB cable.
3. Open the flasher page → **Flash Firmware** → choose the port → **Install**.
4. Keep **Erase device** ticked on the first install.
5. Wait for *Installation complete*, then unplug and replug.

### First-time setup on the box
1. Join the Wi-Fi `FamApp-Soundbox`, password `soundbox`.
2. The setup page opens (or browse to `192.168.4.1`).
3. Enter 2.4 GHz Wi-Fi, the Gmail linked to your FamApp account, and a Gmail **app password** (myaccount.google.com/apppasswords, needs 2-Step Verification).
4. Save. The box restarts and beeps when it's ready.

---

## 6. Browser compatibility

| Browser | Works? |
|---|---|
| Chrome 89+ (Windows, macOS, Linux, ChromeOS) | ✅ |
| Edge 89+ | ✅ |
| Opera | ✅ (Chromium) |
| Firefox, Safari | ❌ no Web Serial |
| Any phone browser | ❌ |

The page must be opened over **https://** (GitHub Pages) or `http://localhost`. Opening `index.html` as a file does not work.

---

## 7. USB drivers

Windows 10/11 usually installs these by itself. If no port shows up, check the chip printed near the USB socket:

| Chip | Driver |
|---|---|
| CP2102 / CP210x | https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers |
| CH340 / CH341 | https://www.wch-ic.com/downloads/CH341SER_EXE.html |

After installing, unplug and replug. The board should appear in *Device Manager → Ports (COM & LPT)*.

---

## 8. Troubleshooting

**"Failed to download manifest"**
- Open the Pages URL, not the local file.
- Open `…/manifest.json` directly. If it's a 404, the file isn't at the repo root or Pages isn't enabled yet.
- Check the JSON is valid (no trailing commas). Paste it into any JSON validator.
- Hard-refresh: `Ctrl + F5` (Windows) / `Cmd + Shift + R` (macOS).

**GitHub Pages 404**
- Settings → Pages must show *Your site is live at …*.
- The repository must be Public.
- `index.html` must be at the repository root, not inside a sub-folder.
- New pages can take 1–10 minutes to appear.

**Connection fails / stuck on "Connecting"**
- Close the Arduino IDE Serial Monitor and any other app using the port.
- Enter bootloader mode manually: **hold BOOT, press and release EN (RST), release BOOT**, then click *Flash Firmware* again.
- Try another cable and a USB port directly on the computer.
- If the amplifier is wired, unplug its VIN while flashing. Weak USB power makes the board reset.

**Old files after an update (browser cache)**
- Hard-refresh with `Ctrl + F5`, or open the page in an Incognito window.
- Bump `"version"` in `manifest.json` whenever you upload new firmware.

**It flashed but doesn't start**
- Press EN/RST once.
- `SETUP MODE` on the OLED means the settings were erased: do the first-time setup.

**ESP32-S3 boards**
- This firmware is for the classic ESP32 only. An ESP32-S3 needs its own build (`chipFamily: "ESP32-S3"`, bootloader at `0x0`).
- S3 boards with native USB may need: hold **BOOT**, plug in USB, release BOOT, then flash.

---

## 9. Manual flashing (no browser)

With [esptool](https://docs.espressif.com/projects/esptool/) installed (`pip install esptool`):

**Single file:**
```bash
esptool.py --chip esp32 --port COM5 --baud 921600 write_flash 0x0 firmware/FamApp_Soundbox.merged.bin
```

**Separate files:**
```bash
esptool.py --chip esp32 --port COM5 --baud 921600 write_flash 0x1000 firmware/FamApp_Soundbox.bootloader.bin 0x8000 firmware/FamApp_Soundbox.partitions.bin 0xe000 firmware/boot_app0.bin 0x10000 firmware/FamApp_Soundbox.bin
```

Replace `COM5` with your port (`/dev/ttyUSB0` on Linux, `/dev/cu.usbserial-*` on macOS). Add `erase_flash` first for a completely clean install.

---

## 10. Updating the firmware

1. Rebuild in the Arduino IDE (*Sketch → Export Compiled Binary*) with the same board settings.
2. Replace the files in `firmware/`, keeping the **same names**.
3. Increase `"version"` in `manifest.json` and the badge in `index.html`.
4. Commit and push. GitHub Pages updates within a minute or two.

---

## Common mistakes

- Bootloader at `0x0` on a classic ESP32: it must be `0x1000`.
- Wrong `chipFamily` (e.g. `"ESP32-C3"` for an ESP32 board): ESP Web Tools refuses to flash.
- File names in `manifest.json` that don't match the real files (including upper/lower case).
- Opening `index.html` from your computer instead of the https Pages URL.
- Charge-only USB cables.
- Leaving the Serial Monitor open while flashing.
