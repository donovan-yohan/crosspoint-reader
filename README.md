# CrossPoint Reader

[![Fund contributors](https://img.shields.io/badge/%F0%9F%91%91_Fund_contributors-royalty.dev-BB953A?style=for-the-badge&labelColor=1a1a1a)](https://app.royalty.dev/crosspoint-reader/crosspoint-reader)

CrossPoint is open-source e-reader firmware - community-built, fully hackable, free forever. It's maintained by a growing community of developers and readers who believe your device should do what you want - not what a manufacturer decided for you.

**Now running on:** ESP32C3-based Xteink [X4](https://www.xteink.com/products/xteink-x4) and [X3](https://www.xteink.com/products/xteink-x3).

![CrossPoint Reader running on Xteink device](./docs/images/cover.jpg)

> If you're planning to buy an Xteink device, consider purchasing an **X3/X4 Developer Edition** through https://crosspointreader.com. CrossPoint receives a small share of each sale, helping fund development costs.

---

> ### 💌 This fork adds: Lovenote messenger
>
> This is a fork of CrossPoint. Everything below is upstream CrossPoint and still applies — the fork adds one thing on
> top: the reader can **receive notes, books and wallpapers from a phone**, including when the phone is nowhere near it.
> The newest note becomes the reader's sleep screen for exactly one sleep, then your usual wallpaper comes back.
>
> The phone half is a separate app, [**Lovenote**](https://github.com/donovan-yohan/send-to-x4-mobile-app/tree/messenger)
> (branch `messenger` — that repository's default branch is the unmodified upstream app).
> With the feature switched off, this firmware behaves exactly like upstream.
>
> **→ [Lovenote messenger](#lovenote-messenger-this-fork)** for what it does, what to set, and how to build it.

## What can CrossPoint do?

- **Reader engine**: EPUB 2/3 rendering with embedded-style option, image handling, hyphenation, kerning, chapter navigation, footnotes, bookmarks, dictionary lookups ([StarDict](docs/dictionary.md)), go-to-percent, auto page turn, orientation control, focus reading, KOReader progress sync and more. 

- **Various formats**: native handling for `.epub`, `.xtc/.xtch`, `.txt`, and `.bmp`.

- **Screenshots.**

- **Custom fonts**: install your favorite fonts on the SD card.

- **Tilt page turn (X3 only)**.

- **Library workflow**: folder browser, hidden-file toggle, long-press delete, recent books, SD-cache management.

- **Wireless workflows**:
  
  - File transfer web UI
  - EPUB Optimizer
  - Web settings UI/API (edit many device settings from browser)
  - WebSocket fast uploads
  - WebDAV handler
  - AP mode (hotspot) and STA mode (join existing Wi-Fi), both with QR helpers
  - Calibre wireless connect flow
  - OPDS browser with saved servers (up to 8), search, pagination, and direct download
  - OTA update checks and installs from GitHub releases

- **Customization**: multiple themes (Classic, Lyra, Lyra Extended, RoundedRaff), sleep screen modes, front/side button remapping, status bar controls, power-button behavior, refresh cadence, and more.

- **Localization**: 24 UI languages and counting. RTL support.

### Coming soon:

- More themes.

- Much more! stay tuned.

---

## USB-locked devices (Xteink Unlocker)

Some Xteink units purchased from third-party stores (e.g. AliExpress) ship with USB flashing locked from the factory.
If your device is locked, you will need to use the **Xteink Unlocker** tool available at
https://crosspointreader.com/#unlock-tool before you can flash CrossPoint.

**You do not need this tool if you bought your device directly from xteink.com.** Those units are not locked.

**Not sure if your device is locked?** Power it on, connect the USB-C cable, and try flashing via the web flasher first (see
[Install firmware](#install-firmware) below). If the browser's serial device picker does not show your device, try a different
USB port or browser before assuming the device is locked. Only reach for the unlocker if the device still doesn't appear.

> ### ⚠️ WARNING: READ THIS BEFORE USING THE UNLOCKER ⚠️
> 
> **The only officially supported firmwares in the unlock tool are CrossPoint and CrossInk.**
> 
> Flashing any other firmware on a USB-locked device may **permanently brick the device** or leave it **permanently
> stuck on that firmware with no recovery path**. Once USB flashing is re-locked, your only way back is via OTA, and if
> the firmware you flashed doesn't support OTA, **there is no way out**.

## Install firmware

### Web installer (recommended)

1. Connect your device to your computer via USB-C and wake/unlock the device
2. Go to https://crosspointreader.com/#flash-tools, select device (X3 or X4), and choose an official CrossPoint release.

### Web installer (specific version)

1. Connect your device to your computer via USB-C and wake/unlock the device
2. Download a `firmware.bin` from [Releases](https://github.com/crosspoint-reader/crosspoint-reader/releases), local build, or continuous integration artifact.
3. Go to https://crosspointreader.com/#flash-tools, select device (X3 or X4), click "Custom .bin" and upload a `firmware.bin`.

### Revert to Official Firmware

To revert to the official firmware, you can also flash the latest official firmware using https://crosspointreader.com/#flash-tools.

### Command line

1. Install [`esptool`](https://github.com/espressif/esptool):

```bash
pip install esptool
```

2. Download `firmware.bin` from the [releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases).
3. Connect your device via USB-C.
4. Find the device port. On Linux, run `dmesg` after connecting. On macOS:

```bash
log stream --predicate 'subsystem == "com.apple.iokit"' --info
```

5. Flash:

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

Adjust `/dev/ttyACM0` to match your system.

### Manual

See [Development quick start](#development-quick-start) below.

---

## Custom SD-card fonts

Convert your own TTF/OTF files into `.cpfont` files that load from the SD card. No firmware reflash is needed.

1. Go to https://crosspointreader.com/fonts and open the "SD-card font builder" form.
2. Upload up to four styles (regular, bold, italic, bold-italic), set the family name, point sizes, and Unicode range.
3. Download the generated `.cpfont` files.
4. Copy them to your SD card under `/fonts/YourFont/` (or `/.fonts/YourFont/` to hide the folder).
5. Select the font on the device from the font settings.

Conversion runs the firmware repo's `lib/EpdFont/scripts/fontconvert_sdcard.py` script unmodified, so output matches a local host build.

---

## Lovenote messenger (this fork)

Lovenote is a two-person love-note messenger built on top of CrossPoint. One phone composes a note — a photo, some text, or
a doodle — and it turns up on the reader as its sleep screen. It also delivers epubs and wallpapers over the same path.

**A note is the sleep screen, once.** A note never renders live and never interrupts: no banner, no toast, no unread badge,
nothing to dismiss. You set the reader down, the panel paints, and the note is what is on the panel. Pick it back up and
you are in your book again, as normal. Set it down a second time and your configured wallpaper is back. That reversion is
keyed on the note id, so a note gets exactly one turn on the panel no matter how many times you sleep and wake.

The phone half lives in its own repository: [**Lovenote**](https://github.com/donovan-yohan/send-to-x4-mobile-app/tree/messenger)
(Expo / React Native, MIT), on branch **`messenger`** — that repository's default branch is the unmodified upstream app, so
follow the branch link rather than the bare repo URL. One phone is the **host** — paired with the reader, allowed to change
its permanent state — and a second phone can be a **client** that only sends. The reader does not care which; it just
receives.

Everything here is additive: no upstream behaviour is removed or changed, and with **Message Sync** off none of it ever
brings the radio up on its own.

### What the firmware gains

- **Note delivery on the sleep screen**: a staged frame at `/.love-notes/current.frame` takes the sleep image at the next
  sleep-entry, once, then the configured wallpaper returns (`src/network/MessageSync.cpp`, `SleepActivity`).

- **A mailbox client**: the reader pulls from a small capability-URL server on its own schedule. There are no acks and no
  per-device cursor — the server publishes a manifest, the reader diffs it and decides. Two readers can share one mailbox
  and converge independently.

- **Two unattended sync windows**: one at sleep-entry, and a silent zero-UI check on wake. Both are off unless you turn
  them on, both are hard-bounded, and both leave the radio off when they finish. See
  [Sync windows and battery](#sync-windows-and-battery).

- **Two new modes on the File Transfer screen**, alongside Join a Network / Calibre Wireless / Create Hotspot:
  - **Mailbox Sync** — drain the mailbox now over a saved Wi-Fi network.
  - **Sync with App** — the reader raises its own access point, your phone joins it, and the phone becomes the mailbox's
    front door. It can relay over cellular, or serve what it has already queued with **no internet at all**. The web
    server is deliberately *not* started for this mode, so nothing on the reader is writable from that link.

- **Books**: epubs arrive in `/books` and are readable the moment a sync ends. Fetched one at a time, resumed across
  windows with HTTP `Range` until the bytes match the manifest exactly. A book that leaves `/books` (you finished it, and
  the reader moved it to `/read`) is never re-downloaded — diffing is by id, never by filename. Nothing here ever deletes
  a readable book (`src/network/BookSync.cpp`).

- **Wallpapers**: the sleep-screen wallpaper rides the same mailbox, so it no longer requires being on the same LAN as the
  reader. Validated as a parseable BMP before it is applied, because the sleep renderer has no user in front of it
  (`src/network/WallpaperSync.cpp`).

- **Wi-Fi handover from the phone**: a Wi-Fi password is the single worst thing to type on e-ink. While your phone is on
  the reader's own AP in Sync with App, it can hand a network over the link that is already up (`GET /cp-wifi`, then a
  `DELETE` acknowledgement). Credentials are only ever *added*, never substituted, and the pickup is refused outright on
  the saved-network transport — a mailbox host on the internet has no business handing the reader a network.

- **A per-device AP passphrase** for Sync with App: WPA2, minted on the reader, editable, and it survives a settings
  reset.

### How a note reaches the reader

Three routes, and the reader treats all three as the same thing arriving:

1. **Direct, over your LAN** — phone and reader on the same Wi-Fi, with the reader sitting in File Transfer mode. The app
   streams the frame straight onto the SD card over the existing web server's WebSocket upload path. Instant, no server
   involved, but it needs you to be home and the reader to be awake on that screen.

2. **Mailbox** — a tiny capability-URL server that holds the newest note (plus queued books and wallpapers) until the
   reader asks for it. This is what makes sending from anywhere work: the phone publishes whenever, the reader collects on
   its own sync windows. Run it as a Cloudflare Worker or self-host the node server; both ship in the app repository.

3. **Peer link** — *Sync with App*. The reader raises its own AP, the phone joins as a peer, and the phone either proxies
   the mailbox over cellular or serves items straight from its own outbox. The zero-internet route.

Route 2 is the one the unattended windows use. Routes 1 and 3 are things you do on purpose, standing next to the reader.

### Reader settings you have to set

| Setting | Where | What it is |
| --- | --- | --- |
| **Message Sync** | On device: **Settings → System** | The master switch for the two unattended windows (sleep-entry sync and the wake check). Off by default. The two manual modes work regardless of this toggle. |
| **Message sync URL** | Browser only: **File Transfer → web Settings page** (API key `messageSyncUrl`) | Your mailbox base URL, e.g. `https://mailbox.example.com/m/<box-id>`. The firmware appends the contract's own paths to it. Deliberately not on the on-device Settings screen — it is a long URL and e-ink is a terrible keyboard. The app writes it for you during pairing. |
| **Sync with App passphrase** | On device: **Settings → System** (first row) | The WPA2 passphrase for the reader's own AP in Sync with App. 8–63 printable ASCII characters. Save it **empty** to have the reader mint a fresh one at the next AP session. It is shown on the panel, with a join QR code, while the mode is running. |

Both unattended windows need the toggle **and** a non-empty URL; either one missing and the reader simply does nothing,
which is why each refusal names itself in the serial log rather than failing silently.

> **Know what the transfer hotspot exposes.** The mailbox URL is a capability URL — the `/m/<box-id>` path is the only
> thing protecting the notes and books in your mailbox — and it is currently returned unmasked by `GET /api/settings`,
> which answers anybody who has joined the open File Transfer hotspot. The AP passphrase and the KOReader password *are*
> masked there. Treat File Transfer mode as something you switch on in a room you control. The reasoning, and what it
> would take to mask the URL too, is written up at the `messageSyncUrl` entry in `src/SettingsList.h`.

### Sync windows and battery

The design constraint is that the radio is the expensive thing on this device, so it comes up for **seconds at a time**
and is never left on behind your back.

- **At sleep-entry.** The panel paints the sleep screen *first*, then — if Message Sync is on — the radio comes up: at most
  6 s to associate with a saved network, a TLS handshake, and one ~100-byte manifest read. With nothing new waiting, that
  is the whole cost, a couple of seconds, and the radio is off again. The note pass is capped at 10 s on its own; the
  whole window, including a book or wallpaper transfer riding the same association, is hard-capped at ~28 s
  (`SLEEP_SYNC_WINDOW_MS` in `src/main.cpp`). That cap is only approached when there are actually bytes to move. Wi-Fi is
  off when the call returns on every path: success, failure, timeout, no credentials, nothing new.

- **On wake.** A note published while the reader was asleep is collected by a silent, zero-UI check on the next wake that
  lands at the launcher — never when a book is one keypress away, because Wi-Fi and EPUB rendering must not be resident at
  once on a chip with ~380 KB of RAM. It stages the frame; the frame takes the panel at the next sleep-entry. One
  wake→sleep cycle of lag, deliberately accepted as the price of never interrupting you.

- **In the manual modes.** Mailbox Sync and Sync with App hold the radio up and keep the device awake for as long as they
  run, polling every 4 s, with a hard 30-minute session cap so they cannot outlive a user who walked away. They are a
  "leave it on the table for a minute" thing, not a background state.

One honest caveat, documented in the code: on an **https** mailbox the wolfSSL handshake caps itself internally at ~15 s
per attempt and nothing outside it can shorten that, so a host that accepts the TCP connection and then stalls the
handshake can overrun the budgets above by that much. Everything else — DNS, TCP connect, headers, body, stalls — is
clamped.

### Build and flash this fork

Same toolchain and prerequisites as [Development quick start](#development-quick-start) — this fork changes no build
requirements.

```bash
git clone --recursive https://github.com/donovan-yohan/crosspoint-reader
cd crosspoint-reader

# if cloned without --recursive:
git submodule update --init --recursive

# build + flash over USB-C, device awake and unlocked
pio run --target upload
```

`default_envs = default` in `platformio.ini`, so no `-e` is needed; `pio run -e default` builds without flashing. The
artifact lands at `.pio/build/default/firmware.bin`, which you can flash with `esptool` instead if you prefer:

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 .pio/build/default/firmware.bin
```

Adjust `/dev/ttyACM0` to match your system.

Two things to know before you flash:

- **This fork is not on the web flasher.** https://crosspointreader.com serves official CrossPoint releases only. Build
  locally, or flash a local `firmware.bin` through the flasher's "Custom .bin" option.
- **USB-locked devices still apply.** If your unit shipped with USB flashing locked, read
  [USB-locked devices](#usb-locked-devices-xteink-unlocker) — including the warning — before doing anything.

### Branches

- **`develop`** — the trunk of this fork, and what you want to build. The messenger work is merged here, on top of a
  current upstream base.
- **`messenger`** — the feature branch the work arrived on, kept for history.
- **`upstream`** — the remote pointing at the real project, so the fork can be resynced:

```bash
git remote add upstream https://github.com/crosspoint-reader/crosspoint-reader.git
git fetch upstream
git merge upstream/develop
```

### Further reading

- [**Lovenote app repository**](https://github.com/donovan-yohan/send-to-x4-mobile-app/tree/messenger) (branch
  `messenger`; the repo's default branch `main` is the unmodified upstream app) — the phone app, the mailbox server
  (Cloudflare Worker and a self-hosted node variant), and the reader-link native module.
- [**`docs/SETUP.md`**](https://github.com/donovan-yohan/send-to-x4-mobile-app/blob/messenger/docs/SETUP.md) in that
  repository — the full two-repo walkthrough: build the app, stand up a mailbox, pair the reader, send the first note.
- [**`docs/xteink/mailbox-books-contract.md`**](https://github.com/donovan-yohan/send-to-x4-mobile-app/blob/messenger/docs/xteink/mailbox-books-contract.md)
  — the wire contracts, if you want to write your own client or server: `latest.txt` / `current.frame` for notes,
  `books.txt` / `books/{id}`, `wallpaper.txt` / `wallpaper/{id}`, and the peer-link paths `/cp-proxy` and `/cp-wifi`.
  Section 3A is the lock-screen model in full.

Questions or bugs about the messenger belong on [this fork's issue tracker](https://github.com/donovan-yohan/crosspoint-reader/issues),
not upstream's — none of it is CrossPoint's to support.

---

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md)
- [Web server endpoints](./docs/webserver-endpoints.md)
- [Project scope](./SCOPE.md)
- [Contributing docs](./docs/contributing/README.md)
- [Touch and UI development](./docs/contributing/touch-and-ui.md) - FreeInkUI components for new screens, the touch bridge for existing ones, and build envs for the non-Xteink touch devices

---

## Development quick start

### Prerequisites

- [pioarduino](https://github.com/pioarduino/pioarduino) or VS Code + pioarduino plugin
- Python 3.8+
- `clang-format` 21
- USB-C cable supporting data transfer

### Setup

```bash
git clone --recursive https://github.com/crosspoint-reader/crosspoint-reader
cd crosspoint-reader

# if cloned without --recursive:
git submodule update --init --recursive
```

### Nix/NixOS

Nix/NixOS users can enter the development shell with either `nix develop` (flakes) or `nix-shell`:

```bash
nix develop -f nix
# or
nix-shell nix
```

To flash a connected ESP32-C3 device, enable PlatformIO's udev rules in your NixOS configuration:

```nix
services.udev.packages = with pkgs; [ platformio-core.udev ];
```

After rebuilding the system configuration, reconnect the device or reload udev rules.

### Build / flash / monitor

```bash
pio run --target upload
```

### Contributor pre-PR checks

```bash
./bin/clang-format-fix
pio check -e default
pio run -e default
```

### Debugging

After flashing the new features, it’s recommended to capture detailed logs from the serial port.

First, make sure all required Python packages are installed:

```python
python3 -m pip install pyserial colorama matplotlib
```

After that run the script:

```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

---

## Internals

CrossPoint Reader is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the
cache. This cache directory exists at `.crosspoint` on the SD card. The structure is as follows:

```text
.crosspoint/
├── epub_<hash>/         # one directory per book, named by content hash
│   ├── progress.bin     # reading position (chapter, page, etc.)
│   ├── cover.bmp        # generated cover image
│   ├── book.bin         # metadata: title, author, spine, TOC
│   ├── css_rules.cache  # parsed CSS rule cache
│   ├── img_*            # rendered image cache files
│   └── sections/        # per-chapter layout cache
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── settings.json        # device settings
├── state.json           # resume/runtime state
└── recent.json          # recent books list
```

Removing `/.crosspoint` clears all cached metadata and forces a full regeneration on next open. Book deletes, overwrites, and moves done through the firmware or web UI clear or re-key matching caches; manual SD-card edits may leave stale cache directories behind.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

---

## Contributing

Contributions are welcome. If you're new to the codebase, start with the [contributing docs](./docs/contributing/README.md). For things to work on, check the [ideas discussion board](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas) — leave a comment before starting so we don't duplicate effort.

Everyone here is a volunteer, so please be respectful and patient. For governance and community expectations, see [GOVERNANCE.md](./GOVERNANCE.md).

---

## Community forks

One of the best things about open source is that anyone can take the code in a different direction. If you need something outside CrossPoint's [scope](./SCOPE.md), check out the community forks:

- [CrossInk](https://github.com/uxjulia/CrossInk) — Typography and reading tracking: Bionic Reading (bolds word stems to create fixation points), guide dots between words, improved paragraph indents, and replaces the default fonts with ChareInk/Lexend/Bitter.

- [papyrix-reader](https://github.com/bigbag/papyrix-reader) — Adds FB2 and MD format support. Actively maintained with Arabic script support. Custom themes via SD card.

- ~~[crosspet](https://github.com/trilwu/crosspet) — A Vietnamese fork that adds a Tamagotchi-style virtual chicken that grows based on your reading milestones (pages read, streaks, care). Also: Flashcards, Weather, Pomodoro timer, and mini-games.~~ (Unmaintained)

- [crosspoint-reader-cjk](https://github.com/aBER0724/crosspoint-reader-cjk) — Purpose-built for Chinese, Japanese, and Korean reading.

- [inx](https://github.com/obijuankenobiii/inx) — Completely reimagines the user interface with tabbed navigation.

- ~~[PlusPoint](https://github.com/ngxson/pluspoint-reader) — custom JS apps support.~~ (Unmaintained)

- [crosspoint-reader-papers3](https://github.com/juicecultus/crosspoint-reader-papers3) — Crosspoint port for M5Stack Paper S3. 

- [t5s3-reader](https://github.com/ShallowGreen123/t5s3-reader) — Crosspoint port for LilyGo T5 ePaper S3 / T5S3 4.7-inch e-paper device.

**Note:** Many of these features will make their way into CrossPoint over time. We maintain a slower pace to ensure rock-solid stability and squash bugs before they reach your device.

Want to build your own device? Be sure to check out the [de-link](https://github.com/iandchasse/de-link) project.

---

CrossPoint Reader is **not affiliated with Xteink or any device manufacturer**.

Huge shoutout to [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader), which inspired this project.
