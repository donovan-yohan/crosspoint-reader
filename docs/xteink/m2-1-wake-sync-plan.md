# M2 #1 — Wake-Window Sync — Implementation Plan (READ-ONLY investigation)

Fork: `messenger` branch. Goal: on every real wake from deep sleep, silently
join saved WiFi → fetch a pending love-note frame (serve-on-wake **or**
pull-on-wake) → stage it at `/.love-notes/current.frame` → tear WiFi down and
reclaim RAM → let the existing M1 hook render it on top of the normal boot.
Bounded timeout, safe fallback to normal boot, gated by a `messageSyncEnabled`
setting.

This doc is analysis + seams only. Nothing here has been built.

---

## 1. Sleep / wake mechanics

**Deep sleep is a full chip reset; every wake cold-boots through `setup()`.**
There is no resume-in-place path.

- Sleep entry: `src/main.cpp:195` `enterDeepSleep()` → `powerManager.startDeepSleep(gpio)`
  (`lib/hal/HalPowerManager.cpp:60`) → `freeink::PowerManager::deepSleepUntilPowerButton()`.
  Before sleeping it already tears WiFi down: `src/main.cpp:218-221`
  (`WiFi.disconnect(true); WiFi.mode(WIFI_OFF)`).
- Wake is detected via `esp_sleep_get_wakeup_cause()` inside
  `HalGPIO::getWakeupReason()` — `lib/hal/HalGPIO.cpp:368-388`. It returns a
  `WakeupReason` enum (`lib/hal/HalGPIO.h:96`): `PowerButton`, `AfterFlash`,
  `AfterUSBPower`, `Other`.
- `setup()` reads that reason at `src/main.cpp:330` and routes on it in the
  switch at `331-349`:
  - `PowerButton` (335): verifies press duration; a too-short press calls
    `startDeepSleep()` and **never returns** (goes back to sleep).
  - `AfterUSBPower` (339): immediately `startDeepSleep()` — **never returns**.
  - `AfterFlash` / `Other` / default: fall through and continue boot.
- Recovery escape (UP+POWER held) is detected at `354-368` (only when
  `wakeupReason == PowerButton`), and routes to the SD firmware picker at `420-423`.
- **M1 love-note hook:** `src/main.cpp:317-320`. `showLoveNote =
  Storage.exists("/.love-notes/current.frame")`; if set, `goToMessage()` queues
  a deferred **Push** (currentActivity is null here, so
  `ActivityManager::pushActivity` just stores it — `ActivityManager.cpp:227-231`,
  `252-260`). The routing block (`384-447`) then `replaceActivity`-launches
  Home/Reader as the base, and the first `activityManager.loop()` processes the
  Push so MessageDisplay lands on top, Back returns to the base.
  `showLoveNote` also suppresses the boot splash at `411` and `416`.

Net: the wake-sync phase belongs in `setup()` **after** the going-back-to-sleep
gates (so we never sync when about to re-sleep) and the frame check at 317 must
run **after** the sync writes the frame.

## 2. WiFi lifecycle — reuse verdict

**Do NOT drive `WifiSelectionActivity` headlessly. Extract its connect core
into a ~30-line headless helper.** The saved-credential connect logic already
exists and is UI-free at its heart, but the class is welded to the Activity /
render-task / `mappedInput` / state-machine machinery (`requestUpdate()`,
`RenderLock`, `WifiSelectionState`), which is exactly the surface we want to
avoid before the display is even initialized.

Reusable primitives (call directly):
- `WifiCredentialStore` (`src/WifiCredentialStore.cpp`): `loadFromFile()`,
  `getLastConnectedSsid()`, `findCredential(ssid)`, `getCredentials()`.
  Passwords are XOR/base64-obfuscated on disk, plaintext in memory. `WIFI_STORE`
  singleton.
- The connect sequence to replicate (from `WifiSelectionActivity::attemptConnection()`
  `WifiSelectionActivity.cpp:345-373` and `tryAutoConnectCredential` `279-295`):
  ```
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.begin(ssid, pass);   // or begin(ssid) for open
  // poll WiFi.status()==WL_CONNECTED against a millis() deadline
  ```
  Try `getLastConnectedSsid()` first, then iterate `getCredentials()`. No scan
  is strictly required (begin() will find the AP), which keeps the window short.
- Bounded timeout is idiomatic: the activity already uses
  `AUTO_CONNECTION_TIMEOUT_MS` / `CONNECTION_TIMEOUT_MS` millis deadlines
  (`checkConnectionStatus()` `375-460`). Reuse the same pattern with a hard
  overall budget (see §7).

WiFi OFF + RAM reclaim: `WiFi.disconnect(true); WiFi.mode(WIFI_OFF)` (mode NULL)
is the established teardown (`main.cpp:218-221`). **But see §5/§7:** the
codebase's real convention after any WiFi session is a `silentRestart()` to
defragment the heap, not just mode-off.

## 3. Serve path — reuse verdict

**Fully reusable, already proven to coexist with the framebuffer.**
`CrossPointWebServer` (`src/network/CrossPointWebServer.h`) is instantiable
directly: `webServer.reset(new CrossPointWebServer()); webServer->begin();`
then poll `webServer->handleClient()` (see `CalibreConnectActivity::startWebServer`
`CalibreConnectActivity.cpp:73-93` — the leanest existing caller). Stop with
`webServer->stop()`.

- mDNS is a two-liner: `MDNS.end(); MDNS.begin("crosspoint");` (`restartMdns`,
  `CrossPointWebServerActivity.cpp:43-50`; `CalibreConnectActivity.cpp:77-81`).
- The server writes uploads to SD with its own 4 KB buffer
  (`UploadState`/`FontUploadState`, `CrossPointWebServer.h:33-49`) — it does
  **not** touch the framebuffer. Host pushes the frame to
  `/.love-notes/current.frame` (this is the M1 transfer flow today).
- Headless bounded window: run a `handleClient()` loop for a fixed millis budget
  (Calibre uses `MAX_ITERATIONS=80` per pass, `CalibreConnectActivity.cpp:114`),
  exit early once the target frame lands, else exit on timeout. No UI needed.
- **Caveat:** serve-on-wake requires the host to be awake and reachable during
  the wake window and to know when to push — it is "reader briefly reachable,"
  not "reader fetches." It also needs a longer, less deterministic window than a
  single GET, which costs battery and wake latency.

## 4. Pull path — reuse verdict

**Fully reusable and the cleaner fit for "silent on wake."**
`HttpDownloader` (`src/network/HttpDownloader.h`) is a static utility:
```
HttpDownloader::downloadToFile(url, destPath, progress=nullptr,
                               cancelFlag=nullptr, username="", password="");
```
(`HttpDownloader.cpp:261-296`). It:
- picks transport from the URL scheme — plain `http://` for a LAN mailbox, `https://`
  verified against the CA bundle / wolfSSL for a public mailbox (`runGetSecure`,
  `HttpDownloader.cpp:112-231`);
- follows redirects, supports Basic auth, streams in 1 KB chunks;
- on any failure or zero bytes, **removes the dest file** (`285-293`) so a failed
  pull never leaves a partial file.

Recommended usage for the invariant "a malformed/interrupted transfer cannot
replace the last valid frame" (backlog invariant #4): download to a **temp path**
(e.g. `/.love-notes/incoming.frame`), then verify `file.size() == display
buffer size` before promoting it to `current.frame` (remove+rename). Note the
render side is already defensive: `MessageDisplayActivity::onEnter`
(`MessageDisplayActivity.cpp:24-29`) rejects a wrong-sized frame and `finish()`es
safely — but that is after it would have overwritten the old frame, so the
temp-then-promote step is what actually protects the last good note.

A "is there a note?" probe before downloading the full frame (a small HTTP
HEAD/GET to a status URL, or a `204 No Content`) avoids a ~48 KB transfer on
every wake. `HttpDownloader::fetchUrl(url, std::string&)` (`241-251`) covers the
probe.

## 5. RAM budget — coexistence + the real risk

Framebuffer facts (`freeink-sdk/.../FreeInkDisplay.h`): the C3 is
single-buffer, framebuffer ~48 KB, **heap-allocated** at `display.begin()`
(there is no PSRAM). Reading sessions leave only ~50 KB free heap
(`platformio.ini:49`). The EPUB reader's chapter layout **borrows the
framebuffer's own storage** as scratch (`lendBuildStorage`/`returnBuildStorage`,
`FreeInkDisplay.h:271-285`) precisely because the heap can't spare another 48 KB.

Coexistence during the sync window:
- **Serve/pull + framebuffer already coexist in production.** File-transfer mode
  (`CrossPointWebServerActivity`) and Calibre mode run WiFi + `WebServer` +
  `WebSocketsServer` with the framebuffer allocated. So the sync window itself
  fits.
- **The frame render is cheap.** `MessageDisplayActivity` reads the staged frame
  *directly into the existing framebuffer* — no second allocation
  (`MessageDisplayActivity.cpp:14-30`). It can render on very little free heap.
- **The real risk is heap *fragmentation*, not peak.** WiFi+TLS churn fragments
  the PSRAM-less heap so badly that a later chapter build OOMs — this is exactly
  why every WiFi activity today ends with `silentRestart()` (a full reboot) in
  its `onExit`: `CrossPointWebServerActivity.cpp:99-107`,
  `CalibreConnectActivity.cpp:57-61`. A plain `WiFi.mode(WIFI_OFF)` does **not**
  defragment.

**Conclusion:** WiFi never needs to coexist with the reader's 48 KB build
scratch — the two phases are naturally sequential (sync writes to SD, reader
reads later). The safe way to bridge them is to **reboot after the sync
window** (§7 Option A), matching the codebase's own convention, rather than
trying to continue into the reader on a post-WiFi heap in the same boot.

## 6. Settings — the 3-edit pattern

Persistence is automatic: `CrossPointSettings::toJson`/`fromJson` iterate
`getSettingsList()` and read/write any entry that has a `valuePtr` (bool/enum)
or a `stringOffset` (char[]) — `CrossPointSettings.cpp:63-167`. So a new toggle
needs no serializer code.

Edit 1 — field, `src/CrossPointSettings.h` (near the other feature toggles,
~line 254 `fadingFix`):
```cpp
uint8_t messageSyncEnabled = 0;            // wake-window love-note sync (default off)
char    messageSyncHost[96] = "";          // optional: pull URL / mailbox base (empty = serve mode)
```

Edit 2 — list entry, `src/SettingsList.h` (System block, ~line 305-315):
```cpp
SettingInfo::Toggle(StrId::STR_MESSAGE_SYNC, &CrossPointSettings::messageSyncEnabled,
                    "messageSyncEnabled", StrId::STR_CAT_SYSTEM),
// host URL: category-less String so it persists + is web-editable but stays off
// the on-device Settings screen (same trick as opdsDownloadFolder, line 319):
SettingInfo::String(StrId::STR_MESSAGE_SYNC_HOST, &SETTINGS.messageSyncHost[0],
                    sizeof(SETTINGS.messageSyncHost), "messageSyncHost"),
```
`Toggle`/`String` factories: `src/activities/settings/SettingsActivity.h:70` and `:113`.

Edit 3 — i18n. Strings are generated, not hand-written: add keys to
`lib/I18n/translations/english.yaml` (e.g. `STR_MESSAGE_SYNC: "Message sync"`,
`STR_MESSAGE_SYNC_HOST: "Sync host"`) and run `scripts/gen_i18n.py`, which
regenerates `lib/I18n/I18nKeys.h` (the `StrId` enum) and `lib/I18n/I18nStrings.cpp`.
(`I18nStrings.h` is auto-generated — "DO NOT EDIT".) The host-URL key only needs a
string if it is ever surfaced in UI; the web UI can label it from the `key`.

## 7. The concrete plan — smallest correct slice

### Shared spine (both variants)
A new headless module, e.g. `src/network/WakeSync.{h,cpp}` exposing one entry
point: `bool WakeSync::run();` (returns true if it staged/updated a frame).
Internally:
1. `WIFI_STORE.loadFromFile()`; if no credentials → return false (normal wake).
2. Headless connect (§2 core) with a **hard overall deadline** (recommend
   ~6–8 s total: e.g. 5 s connect budget + a couple seconds for the fetch). On
   timeout/failure → `WiFi.mode(WIFI_OFF)` → return false.
3. **Variant-specific fetch** (below), writing to `/.love-notes/incoming.frame`.
4. Validate `size == display.getBufferSize()`; on success remove+rename to
   `/.love-notes/current.frame` (protects the last-good frame, invariant #4).
5. `WiFi.disconnect(true); WiFi.mode(WIFI_OFF)`.

Shared vs different:
- **Shared:** WiFi connect/teardown, the temp→validate→promote staging, the
  deadline discipline, the settings gate.
- **Pull-specific:** one `HttpDownloader::downloadToFile(SETTINGS.messageSyncHost + "/current.frame", tmp)`
  (optionally a `fetchUrl` "any note?" probe first). Cleanest; deterministic;
  shortest window.
- **Serve-specific:** `MDNS.begin("crosspoint")` + `new CrossPointWebServer()` +
  a bounded `handleClient()` loop until the frame arrives or the deadline
  fires, then `stop()`. Reuses §3 wholesale; needs a live host and a longer
  window.

Recommendation: **ship pull-on-wake first** (item §4) — it is the smaller,
fully-deterministic slice and matches "silent on wake." Keep the serve entry
point behind the same gate as a follow-up (empty `messageSyncHost` ⇒ serve).

### Insertion into `setup()` — the seam
Two edits in `src/main.cpp`:

1. **Relocate** the M1 frame check. Move `src/main.cpp:317-320`
   (`showLoveNote` + `goToMessage()`) to sit **immediately after** the wake-sync
   block below. It must run after the sync stages the frame; `goToMessage()`
   only queues a deferred Push, so its exact position before the routing block
   (384-447) does not otherwise matter.

2. **Insert the wake-sync block** after the recovery-mode detection
   (after `src/main.cpp:368`, before the version log at `371`), i.e. after all
   the "go back to sleep" gates (which never return) and after recovery
   detection (which we skip syncing for):
   ```cpp
   if (SETTINGS.messageSyncEnabled && !recoveryFirmwareMode && !g_wakeSyncDone) {
     if (WakeSync::run()) { /* frame staged; fall through to M1 check */ }
   }
   ```

### RAM-safe ordering — reboot after sync (Option A, recommended)
Because a post-WiFi heap is fragmented (§5), do **not** continue straight into
the reader in the same boot. Instead, after a successful (or timed-out) sync,
`silentRestart()` (`src/SilentRestart.h`, `main.cpp:139`) to get a pristine
heap. Guard against a sync→reboot→sync loop with an `RTC_NOINIT` flag, exactly
like the existing `silentRebootMagic` (`main.cpp:116-118, 285-289`): set
"just-synced, skip-sync" before the restart, read-and-clear it at the top of the
next `setup()`. On the fresh boot the sync is skipped, the M1 check at (relocated)
317 finds `/.love-notes/current.frame`, and normal routing renders it on a clean
heap. Deep-sleep wake is itself a clean boot, so the only added cost is one
extra reboot when (and only when) a sync actually ran.

- **Option B (no reboot):** sync → `WiFi.mode(WIFI_OFF)` → continue routing in the
  same boot. Simpler, saves ~2–3 s, but the reader's chapter build then runs on a
  post-WiFi heap — the exact fragmentation `silentRestart()` was introduced to
  avoid. Acceptable only if love-notes are shown over Home (no big EPUB build) or
  if heap headroom is measured to be safe. Flagged as the risk to validate.

Ordering summary (Option A): **gates → recovery detect → [sync: connect →
fetch to temp → validate → promote → WiFi OFF] → silentRestart → (next boot)
skip sync → M1 frame check → route → render.**

### Invariants preserved
- No pending note / no creds / net fail / timeout → `WakeSync::run()` returns
  false, WiFi off, boot continues exactly as today. **Never blocks startup**
  beyond the bounded deadline (and only when `messageSyncEnabled`).
- Going-back-to-sleep wakes (`AfterUSBPower`, too-short power press) never reach
  the sync block (their gates `startDeepSleep()` and never return).
- Recovery escape (UP+POWER → SD update) runs before sync and is excluded from
  it → bootloop escape intact.
- Temp→validate→promote means a malformed/interrupted transfer cannot replace
  the last valid `current.frame` (invariant #4). Reading progress and
  `.crosspoint` data are untouched; notes stay in `/.love-notes/`.
- OTA / SD-update paths are on separate seams and unaffected.

### Risks to watch
1. **Heap fragmentation if Option B is chosen** — measure free heap after a real
   sync before opening a large EPUB, or take Option A.
2. **Wake latency / battery** — every gated wake now pays a WiFi connect. Keep
   the deadline tight; consider a "note available?" probe (pull) to skip the
   48 KB transfer when nothing is pending.
3. **Serve variant window** — needs a reachable host during the window; less
   deterministic than pull. Prefer pull for v1.
4. **Silent phase before display init** — sync runs before `setupDisplayAndFonts`
   (382), so it is genuinely invisible (panel holds the sleep frame). Good, but
   means no on-screen "syncing…" feedback; acceptable for "silent on wake."

---

## REDESIGN 2026-07-28 — sleep-sync (NOT wake-blocking) + dedup

**Product constraints (user):** reading must NEVER wait for network (no connect in wake->read path); notes must NOT interrupt mid-read (**item #4 in-session render CUT** — bad UX; #3 server-during-reading drops to near-zero value); always connect through the paired host device, fail-fast, don't waste time on failed connections; never double-up / reshow a note.

**New shape:**
- **SYNC AT SLEEP:** move connect+pull to the pre-deep-sleep path (after the sleep screen renders, before `esp_deep_sleep`). User has walked away -> latency hidden. Fail-fast short deadline; unreachable -> skip silently.
- **RENDER AT WAKE (instant):** existing M1 hook renders pre-staged `current.frame`, GATED by dedup. **No network at wake.**
- **HEAP win:** deep sleep = full reset, so wake cold-boots pristine heap. The `silentRestart`/RTC-skip-flag from the wake-version is **no longer needed — drop it.** Simpler + safer.
- **DEDUP by message ID:**
  - Each note has an ID (from mailbox). Persist `lastShownId` (APP_STATE/NVS); store staged note's ID (sidecar `/.love-notes/current.id` or in state).
  - Sleep-sync does a CHEAP "latest id" check first (reuse HTTP GET; ETag/HEAD or a tiny `/latest` JSON); if `== lastShownId` (or `== staged id`) -> **skip the download entirely**. Only download+stage genuinely new IDs (temp->validate size==getBufferSize()->promote).
  - Wake: render `current.frame` ONLY if its id `!= lastShownId`; after show/dismiss set `lastShownId = id`. (Also fixes M1 re-showing every wake.)
- **Settings:** `messageSyncEnabled` + `messageSyncUrl` (mailbox pull endpoint) — 3-edit pattern. Connection = saved WiFi (home OR partner hotspot); expected primary = the paired host device.

**Seams:** sync at sleep entry (`main.cpp enterDeepSleep ~218` / `SleepActivity`); dedup-gate the M1 wake hook (`~317-320`). Reuse `WifiCredentialStore` connect core + `HttpDownloader` + temp/validate/promote.

**Mailbox API contract (server-side, to build):** cheap "latest message id for reader X" (ETag/HEAD or small JSON) + a frame download URL. Reader-reachable (Tailscale Funnel).
