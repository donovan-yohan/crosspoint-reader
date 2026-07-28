# Firmware Milestone 2 — Backlog (messenger branch, priority order)

Fork of CrossPoint. **M1 done** (`MessageDisplayActivity` shows `/.love-notes/current.frame`; verified on X3, branch `messenger`, commit 14af658). All transport is **WiFi** (no BLE). **RAM is tight** (~50 KB free heap; framebuffer ~51 KB) → WiFi + render must be sequential; `WiFi.mode(WIFI_OFF)` + free before EPUB render.

## 1. Wake-window sync (HIGHEST) — notes appear when the book opens
On every wake from deep sleep: silently join saved WiFi, briefly start server (or pull from host/mailbox), fetch + render any pending note, WiFi off. Kills the manual transfer-mode dance.
- **Seam:** `main.cpp` `setup()` after `SETTINGS.loadFromFile()` (~309), before activity routing (~359) — same hook region as M1's current.frame check.
- **WiFi:** reuse `WifiSelectionActivity` connect path (`WiFi.mode(WIFI_STA)` + `WiFi.begin` with saved creds); bounded connect timeout (millis deadline).
- **Fetch:** (a) briefly start `CrossPointWebServer` and let host push, OR (b) **PULL** from host/mailbox (`HttpDownloader` GET). Pull is cleaner for "silent on wake."
- **Render:** if a pending `/.love-notes/current.frame` is present → `goToMessage()` (M1 activity).
- **CRITICAL:** `WiFi.mode(WIFI_OFF)` + free before opening a book (RAM). Bounded + safe fallback to normal boot on timeout/no-net.
- Gate behind a `messageSyncEnabled` setting.

## 2. Accept overwrite on upload — remove the delete-then-upload workaround
WS upload handler rejects existing files (both senders currently delete-then-upload).
- **Seam:** `src/network/CrossPointWebServer.cpp:710-713` — `if (Storage.exists(filePath)) { state.error = "File already exists: "... }`. Change to overwrite: `Storage.remove(filePath)` before write (or drop the existence check for the upload path). Low-risk, high-value.

## 3. Server during reading sessions (optional, toggleable) — instant LAN push
Keep WiFi + web server up while the reader is awake/in-use so a host on the same LAN pushes instantly. Battery trade-off → user setting (default off).
- **Seam:** reader activity + `CrossPointWebServerActivity`; add a toggle in `SettingsList`. Watch heap (server + reader coexistence — test).

## 4. In-session note render — show a note arriving mid-read
Display/notify a new frame while someone is reading (not only next wake).
- **Seam:** on upload completion for `/.love-notes/current.frame` (WS write-complete in `CrossPointWebServer.cpp`) → notify the active reader → `goToMessage()` (or a dismissable toast). Depends on #3 (server up during reading).

## Invariants to preserve (from spec)
- No pending note = normal wake; net failure = normal wake after timeout; never block library/reader startup.
- Reading progress + `.crosspoint` data untouched; notes stay in `/.love-notes/`.
- Existing OTA, SD-update, and bootloop escape (hold **UP+POWER** at boot → SD recovery) intact.
- A malformed/interrupted transfer cannot replace the last valid frame.
