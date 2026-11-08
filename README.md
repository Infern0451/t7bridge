# T7 Bridge: Y-Connect for iPhone on the Yamaha Ténéré 700

A small ESP32-S3 board that sits between your bike and your iPhone, so the
Ténéré 700's TFT shows your music and calls again, every time you turn the key.

| On the TFT | From the handlebar |
|---|---|
| Track, artist, album, play/pause | Play/pause, next, previous, volume up/down |
| Caller name or number | Answer, decline, hang up |
| Missed-call notice | |
| Phone battery and clock | |

No Yamaha app needed day to day. Reconnects by itself. Updates over Wi-Fi from
your phone. Sealed 3D-printable case included.

**Not included:** navigation, messages, weather.

> **Not affiliated with, endorsed by, or supported by Yamaha.** This is a
> hobby project shared as-is, with no warranty. You install it at your own
> risk. It doesn't modify the bike: unplug it and everything is as before.

---

## Why this exists

On iPhone, the Y-Connect pairing works once and then the bike never
reconnects. The cause is at the Bluetooth level between iOS and the bike's
connectivity unit (CCU): right after an iPhone reconnects, the CCU goes
silent and the link drops. No iPhone app can change that, because iOS
controls the parts of the connection that break.

The bridge avoids the problem by never letting them talk directly. The CCU
connects to the bridge (the way an Android phone would), and the iPhone
connects to the bridge (as an accessory, like a smartwatch). The bridge
translates between the two.

```
 Ténéré 700 CCU  <--Bluetooth-->  T7 Bridge (ESP32-S3)  <--Bluetooth-->  iPhone
   (Yamaha protocol)                                       (Apple media + call services)
```

---

## What you need

- **ESP32-S3 DevKitC-1 board with 16 MB flash** (the N16R8 or N16 version).
  About $10-15. 8 MB versions don't have room for over-the-air updates.
  Pin headers aren't needed (and make the case bigger).
- A **USB-C data cable** for flashing, and a **short USB-C cable with a
  straight plug** for the bike.
- **Power** on the bike (see [Powering the bridge](#powering-the-bridge)).
- The **Yamaha Y-Connect app** and your Yamaha account, once, to teach the
  bridge your bike's pairing code. Delete the app afterwards.
- Optional: the [3D-printed case](enclosure/) (PETG), two zip ties, 3M VHB tape.

Tested on: 2025 Ténéré 700, iPhone 17 Pro Max (iOS 27), iPad mini.
Other Yamaha models with the same connectivity unit may work, but haven't
been tried. Please report results.

---

### Tested by riders

| Bike | iPhone | Board | Notes |
|---|---|---|---|
| 2025 Ténéré 700 | iPhone 17 Pro Max, iOS 27 | ESP32-S3 DevKitC N16R8 | Developer's bike |
| 2026 Ténéré 700 | iPhone 14 Pro, iOS 27 | ESP32-S3 DevKitC | Flashed from scratch, no issues |

Tried it on another bike or iPhone? Open an issue or comment on the Reddit
thread and it'll be added here.

---

## Setup

### 1. Flash the firmware

**Easiest: from your browser.** Open the
[**T7 Bridge web installer**](https://infern0451.github.io/t7bridge/) in
**Chrome or Edge on a computer** (Safari and phones can't do this). Plug the
board into the computer by its port labelled **USB**, click **Install**, and
pick the board from the list.

If the board isn't listed, or the install fails: hold the **BOOT** button,
tap **RST**, let go of BOOT, and try again. Brand-new boards often need this
once. When it's done, tap **RST**.

Prefer the command line? See [Building from source](#building-from-source).

### 2. Teach it your bike (learn mode)

The bike only accepts a new phone (or bridge) that knows the bike's 6-digit
pairing code. The code comes from Yamaha's servers through the Y-Connect app,
so the bridge borrows it: for a moment it pretends to be your bike, and the
app hands the code over.

1. **Power the bridge near the bike and turn the ignition on.** Within a few
   seconds the bridge reads your bike's ID from its Bluetooth name. (The bridge
   doesn't connect to the bike in this step.)
2. **Turn the ignition off**, and take the bridge and your phone **away from
   the bike** (another room is ideal). Power it from a laptop or power bank.
   The bridge is now advertising as your bike.
3. **In the Y-Connect app**, sign in and pair your motorcycle as usual. The
   app connects to the bridge and should report a successful connection. If
   the app thinks the bike is already paired, remove it in the app and pair
   again. If the app asks you to switch the bike on, don't: just keep it away
   from the bike.
4. **Delete the Y-Connect app.** If your phone's Bluetooth settings list
   `YCCU_...`, tap ⓘ → **Forget This Device**. (Otherwise the iPhone keeps
   trying to grab the bike and gets in the bridge's way.)
5. **Back at the bike, turn the ignition on.** The bridge pairs with the bike
   by itself, in about 10 seconds. The TFT shows **App** and a battery level.

You can watch progress on the bridge's setup page: join the Wi-Fi network
**t7bridge-update** (password `t7bridge-update`) and open
**http://192.168.4.1**. This Wi-Fi stays on until the bridge knows your
pairing code. If you already know the code (for example from an Android
capture), you can type it in there instead of using the app.

### 3. Pair your iPhone

1. iPhone: **Settings → Bluetooth**. Under *Other Devices*, tap
   **t7bridge**, then **Pair**.
2. When iOS asks to **allow t7bridge to receive your notifications**, tap
   **Allow**. This is what gives the TFT caller ID.
3. Play some music. The TFT shows the track.

On the TFT, press **Home → Media → Home** once to give the joystick to audio
control.

### 4. Install it on the bike

- Put the board in the [case](enclosure/) and mount it with VHB tape. Behind
  the TFT works well. Keep metal away from the board's antenna end (opposite
  the USB plug).
- Connect power (below). The bridge starts when the bike does.

---

## Powering the bridge

The bridge needs 5 V over USB-C and draws about 0.3-0.5 W.

| Option | Good for | Watch out for |
|---|---|---|
| **The bike's USB-C port** | Simplest, nothing to wire | Make sure the port switches off with the ignition (with the bike off, a phone shouldn't charge from it). If it stays on, the bridge drains the battery. On the tested 2025 T7 it switches off. |
| **Hardwired:** fuse tap on a switched (ignition) circuit + 12 V-to-5 V USB converter | Permanent install | Use a switched circuit and a fuse. A quality hardwired USB car charger works as the converter. |
| **Power bank** | Testing, or no usable port | You switch it on and off yourself. Some banks turn themselves off at very low current. |

---

## Everyday use

- Turn the key: the TFT shows **App** within a few seconds.
- Music: whatever is playing on the iPhone shows on the TFT.
- Incoming call: the TFT shows the caller with Answer / Decline. During a call,
  End Call hangs up.
- Outgoing calls don't show on the TFT (iOS gives accessories no way to hang
  them up), so the TFT stays on media.

---

## Updating over Wi-Fi

New versions can be installed from your phone, no computer needed:

1. Download the new **`t7bridge.bin`** from the
   [Releases page](https://github.com/Infern0451/t7bridge/releases) to your
   phone (Files app).
2. Bike on, bridge connected. Press **volume down 5 times quickly**. The TFT
   shows **UPDATE MODE** (your phone volume also goes down 5 steps).
3. Join the Wi-Fi network **t7bridge-update** (password `t7bridge-update`).
   If iOS warns there's no internet, stay connected anyway.
4. In Safari, open **http://192.168.4.1**, choose `t7bridge.bin`, tap
   **Upload**, and wait for "Update installed".
5. The bridge restarts and reconnects to the bike and your iPhone by itself.

The update Wi-Fi switches off after 10 minutes. The bridge refuses files that
aren't T7 Bridge firmware, checks the file is complete, and if a new version
fails to start it goes back to the previous one automatically. Your pairings
are kept.

---

## Starting over / new bike

Hold the board's **BOOT** button for **5 seconds** (or use **Forget bike** on
the setup page). The bridge forgets the bike and all Bluetooth pairings and
restarts in learn mode. Also forget **t7bridge** on the iPhone, then repeat
setup steps 2 and 3.

---

## Troubleshooting

Logs: connect the board's **USB** port to a computer and open a serial monitor
(115200 baud), or run `idf.py monitor`. The log never shows your pairing code.

| Problem / log says | What to try |
|---|---|
| Setup page says "Looking for your bike" | Ignition on, bridge within a few metres of the bike. Or type the S/N from the CCU label (14 characters) on the setup page. |
| Y-Connect never connects in learn mode | Make sure the real bike is off or far away. Remove the bike in the app and pair again. Send the log: everything the app sends is recorded. |
| `app sent the RECONNECT form` | The app thinks it's already paired. Remove the bike in the app and pair again. |
| `the CCU ignored our pairing code` | The stored code is probably wrong: hold BOOT 5 s and redo learn mode. |
| Stuck on `scanning for YCCU_...` | Bike not advertising, or your iPhone grabbed it: delete Y-Connect and forget `YCCU_...` on the iPhone. |
| `t7bridge` not listed on the iPhone | Toggle iPhone Bluetooth off/on. The bridge stops advertising `t7bridge` while in learn mode. |
| No caller ID | Settings → Bluetooth → ⓘ next to t7bridge → allow notifications. Or forget t7bridge and pair again. |
| `iPhone encryption failed` | Forget t7bridge on the iPhone and pair again. |
| After a restart with the bike on, the first connection attempt times out | Normal: the bike is still holding the old link. The second attempt works. |

Still stuck? Open an [issue](https://github.com/Infern0451/t7bridge/issues)
with the log.

---

## Building from source

Uses [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v6.0.

```sh
. ~/esp/esp-idf/export.sh        # in every new terminal
cd t7bridge
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor     # Ctrl+] exits the monitor
```

Settings are under `idf.py menuconfig` → **T7 Bridge** (placeholder battery
level, test track, update Wi-Fi password and time limit, optional starting
credentials). If you publish your own builds, change the update Wi-Fi password.

Host tests for the protocol code (no board needed): `make -C test/host`.

To make the single-file image the web installer uses:
`cd build && python -m esptool --chip esp32s3 merge-bin -o t7bridge-full.bin @flash_args`

| File | What it does |
|---|---|
| `main/yamaha_proto.c` | Yamaha protocol: session-init, status frames, buttons, timing. Plain C, host-tested. |
| `main/apple_proto.c` | Apple media (AMS), notification (ANCS) and time parsing, call tracking. Plain C, host-tested. |
| `main/ccu_link.c` | Bluetooth link to the bike: find, pair, session-init. |
| `main/iphone_link.c` | Bluetooth link to the iPhone: advertise, pair, media/calls/battery/time. |
| `main/learn_mode.c` | Learn mode (poses as the bike for Y-Connect), BOOT-button reset. |
| `main/creds.c` | Stores the bike ID, pairing code and the bridge's own token. |
| `main/ota_update.c` | Setup / update Wi-Fi and web page, firmware checks, rollback. |
| `main/bridge.c` | Shared state, frame schedule, button routing. |
| `enclosure/` | Case generator and STL files. |
| `docs/` | Web installer (GitHub Pages). |

---

## How it works (short version)

- **Bike side:** the bike's CCU offers a serial-style Bluetooth service. After
  pairing, the bridge sends a 60-byte session-init frame (bike ID, pairing
  code, and a token the bridge makes up), the CCU acknowledges it, and from
  then on the bridge sends status frames every 1-2 s (media, call, time,
  battery) and receives handlebar button presses.
- **iPhone side:** the bridge uses Apple's standard accessory services:
  Apple Media Service for music and remote control, Apple Notification Center
  Service for calls (with Answer / Decline actions), plus battery and current
  time.
- **Learn mode:** with no pairing code stored, the bridge advertises under the
  bike's Bluetooth name with the same service. Y-Connect connects, sends its
  session-init frame, and the bridge keeps the code.

---

## Credits

Built by a Ténéré 700 rider who wanted his dash back, with a lot of AI help:

- **ChatGPT** (OpenAI) helped diagnose why iPhones fail and wrote the first
  bridge on a Raspberry Pi.
- **Claude** (Anthropic) rewrote it for the ESP32-S3 when the Pi's SD card
  died, added learn mode and Wi-Fi updates, and designed the case.

Project lead and bike-side testing: [Infern0451](https://github.com/Infern0451).

## License

Firmware and code: [MIT](LICENSE). Case design (`enclosure/`):
[CC BY-SA 4.0](enclosure/LICENSE).

Yamaha, Ténéré and Y-Connect are trademarks of Yamaha Motor Co., Ltd. iPhone
and iOS are trademarks of Apple Inc. This project is independent and not
affiliated with either company.
