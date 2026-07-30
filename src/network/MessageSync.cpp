#include "MessageSync.h"

#include <Arduino.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "WifiCredentialStore.h"
#include "network/HttpDownloader.h"

namespace {
constexpr char NOTES_DIR[] = "/.love-notes";
constexpr char CURRENT_FRAME[] = "/.love-notes/current.frame";
constexpr char CURRENT_ID[] = "/.love-notes/current.id";
constexpr char INCOMING_FRAME[] = "/.love-notes/incoming.frame";

// Suffixes appended to SETTINGS.messageSyncUrl (mailbox base, trailing '/'
// stripped). Server contract: GET base + "/latest.txt" -> tiny plain-text body
// = latest message id (empty body means "no note"); GET base + "/current.frame"
// -> raw framebuffer bytes (exactly the panel buffer size).
constexpr char SUFFIX_ID[] = "/latest.txt";
constexpr char SUFFIX_FRAME[] = "/current.frame";

// Hard fail-fast budget for the entire WiFi connect phase. Sleep is delayed at
// most this long (only when enabled); unreachable networks bail sooner on
// WL_CONNECT_FAILED / WL_NO_SSID_AVAIL.
constexpr uint32_t CONNECT_DEADLINE_MS = 6000;
constexpr uint32_t STATUS_POLL_MS = 100;
constexpr size_t MAX_ID_LEN = 128;

std::string trimId(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
  if (start > 0) s.erase(0, start);
  if (s.size() > MAX_ID_LEN) s.resize(MAX_ID_LEN);
  return s;
}

std::string readStagedId() {
  char buf[MAX_ID_LEN + 1] = {};
  const size_t n = Storage.readFileToBuffer(CURRENT_ID, buf, sizeof(buf));
  if (n == 0) return std::string();
  return trimId(std::string(buf));
}

std::string baseUrl() {
  std::string base = SETTINGS.messageSyncUrl;
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base;
}

void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Radio setup shared by the blocking (Path A) and stepped (Path B) connects.
// No scan is issued -- begin() finds the AP -- to keep the window short.
void wifiBeginSta() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
}

// Connect to a saved network, last-connected SSID first, within a hard overall
// deadline. Returns true if associated.
bool connectHeadless() {
  WIFI_STORE.loadFromFile();
  const auto& creds = WIFI_STORE.getCredentials();
  if (creds.empty()) {
    LOG_DBG("MSYNC", "No saved WiFi credentials");
    return false;
  }

  std::vector<const WifiCredential*> order;
  order.reserve(creds.size());
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  const WifiCredential* lastCred = last.empty() ? nullptr : WIFI_STORE.findCredential(last);
  if (lastCred) order.push_back(lastCred);
  for (const auto& c : creds) {
    if (!lastCred || c.ssid != lastCred->ssid) order.push_back(&c);
  }

  wifiBeginSta();
  delay(100);

  const uint32_t deadline = millis() + CONNECT_DEADLINE_MS;
  for (const auto* c : order) {
    if (static_cast<int32_t>(millis() - deadline) >= 0) break;
    LOG_DBG("MSYNC", "Trying saved network: %s", c->ssid.c_str());
    if (c->password.empty()) {
      WiFi.begin(c->ssid.c_str());
    } else {
      WiFi.begin(c->ssid.c_str(), c->password.c_str());
    }
    while (static_cast<int32_t>(millis() - deadline) < 0) {
      const wl_status_t st = WiFi.status();
      if (st == WL_CONNECTED) {
        LOG_INF("MSYNC", "Connected to %s", c->ssid.c_str());
        return true;
      }
      if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) break;  // fail fast -> next candidate
      delay(STATUS_POLL_MS);
    }
    WiFi.disconnect(true, false);
  }
  LOG_DBG("MSYNC", "No saved network reachable within budget");
  return false;
}

// --- The three staging phases, shared verbatim by both arrival paths --------
// Every note download, on either path, goes through incoming -> validate ->
// promote. That is not boilerplate here: the bytes only ever land in
// INCOMING_FRAME, so a check abandoned at any byte leaves current.frame -- the
// thing the next sleep-entry is going to blit -- bit-for-bit intact.

enum class Probe : uint8_t { Failed, Empty, UpToDate, NewNote };

// Cheap dedup probe: one tiny GET of the mailbox's latest id. Requires WiFi.
Probe probeLatest(const std::string& base, std::string& latestIdOut) {
  std::string latestId;
  if (!HttpDownloader::fetchUrl(base + SUFFIX_ID, latestId)) {
    LOG_DBG("MSYNC", "latest-id fetch failed");
    return Probe::Failed;
  }
  latestId = trimId(std::move(latestId));
  if (latestId.empty()) {
    LOG_DBG("MSYNC", "mailbox empty");
    return Probe::Empty;
  }
  // The staged id is the ONLY dedup left, and it answers exactly one question:
  // "do we already hold these bytes". Under the lock-screen model a note is not
  // consumed by being looked at, so there is no persisted "last shown" id --
  // which also means a note the user glanced at can still come back as the lock.
  if (latestId == readStagedId()) {
    LOG_DBG("MSYNC", "No new note (latest=%s)", latestId.c_str());
    return Probe::UpToDate;
  }
  latestIdOut = std::move(latestId);
  return Probe::NewNote;
}

// Download the frame to INCOMING_FRAME. Requires WiFi.
HttpDownloader::DownloadError downloadIncoming(const std::string& base) {
  // openFileForWrite uses O_CREAT only (no parent-dir creation); a fresh device
  // that never received an M1 web upload has no /.love-notes yet, so ensure it.
  Storage.ensureDirectoryExists(NOTES_DIR);
  return HttpDownloader::downloadToFile(base + SUFFIX_FRAME, INCOMING_FRAME);
}

// Validate the staged bytes and promote them. Pure SD work -- callers turn WiFi
// off first. Returns true iff current.frame now holds a new note.
bool promoteIncoming(const std::string& latestId, size_t frameBufferSize) {
  // The size gate is the only integrity signal (no hash, same as books), and it
  // is device-correct only because frameBufferSize comes from the live panel:
  // 52272 B on the X3 792x528 panel, 48000 B on the X4 800x480.
  size_t incomingSize = 0;
  {
    HalFile f;
    if (Storage.openFileForRead("MSYNC", INCOMING_FRAME, f)) incomingSize = f.size();
  }
  if (incomingSize != frameBufferSize) {
    LOG_ERR("MSYNC", "frame size mismatch: %u != %u", static_cast<unsigned>(incomingSize),
            static_cast<unsigned>(frameBufferSize));
    Storage.remove(INCOMING_FRAME);
    return false;
  }

  // Promote: replace current.frame, then record its id sidecar. The sidecar
  // write must stay AFTER a successful rename -- an id recorded for a frame that
  // isn't there marks the note fetched and it is never retried.
  Storage.remove(CURRENT_FRAME);
  if (!Storage.rename(INCOMING_FRAME, CURRENT_FRAME)) {
    LOG_ERR("MSYNC", "promote rename failed");
    Storage.remove(INCOMING_FRAME);
    return false;
  }
  Storage.writeFile(CURRENT_ID, String(latestId.c_str()));
  LOG_INF("MSYNC", "Staged note %s", latestId.c_str());
  return true;
}

// --- Path B state machine ---------------------------------------------------
// B2 from section 3A: stepped from the main loop() instead of a third task. A
// third task would cost another >= 8 KB stack against the ~50 KB free heap a
// reading session leaves, on top of the WiFi stack; stepping costs nothing and
// keeps the <= 6 s connect off the input path. Only the two short HTTP transfers
// block, and only when there is actually a note to fetch.
//
// Because the machine is stepped from the same task that dispatches input, a
// cancel can only ever land BETWEEN steps -- never mid-transfer. That is what
// makes "WiFi is down before the reader is constructed" a structural guarantee
// rather than a race, and it is why downloadToFile needs no cancel flag here.
enum class WakeStep : uint8_t { Idle, Settle, Connect, Probe, Download };

struct WakeCheck {
  WakeStep step = WakeStep::Idle;
  size_t frameSize = 0;
  uint32_t deadline = 0;     // millis() deadline for the whole check
  uint32_t settleUntil = 0;  // mirrors connectHeadless's post-disconnect settle
  std::string base;
  std::string latestId;
  std::string lastSsid;  // by name, not by pointer: the store can be reloaded
  size_t nextIdx = 0;
  bool triedLast = false;
  bool attemptInFlight = false;
};
WakeCheck wake;

// Whole-check budget: section 3A prices the window at "connect <= 6 s + one TLS
// handshake, 8 s worst case", and requires the caller to enforce it against
// millis() rather than leaning on the 60 s per-socket-op timeout.
constexpr uint32_t WAKE_DEADLINE_MS = 8000;

// Best-effort wall-clock throttle. The *structural* throttle is the real one:
// at most one check per wake, and only on the launcher branch -- enforced by the
// single beginWakeCheck() call site. This adds a second guard for the user who
// wakes to the launcher repeatedly in a short span, and it deliberately fails
// OPEN: HalClock exposes hour+minute only, and only when an RTC is present
// (lib/hal/HalClock.h:25-29), so the stamp is absent on some units and wraps at
// midnight. It suppresses a check only when the elapsed minutes are unambiguous.
constexpr int16_t WAKE_THROTTLE_MINUTES = 30;

bool wakeThrottleAllows() {
  uint8_t hour = 0, minute = 0;
  if (!halClock.isAvailable() || !halClock.getTime(hour, minute)) return true;  // no clock -> fail open
  const int16_t nowMinute = static_cast<int16_t>(hour) * 60 + static_cast<int16_t>(minute);
  const int16_t last = APP_STATE.messageCheckMinuteOfDay;
  if (last >= 0 && last < 1440) {
    const int16_t elapsed = static_cast<int16_t>(nowMinute - last);
    // A negative delta means the clock wrapped past midnight (or was set back):
    // ambiguous, so allow the check rather than guess.
    if (elapsed >= 0 && elapsed < WAKE_THROTTLE_MINUTES) {
      LOG_DBG("MSYNC", "Wake check throttled (%d min since last)", static_cast<int>(elapsed));
      return false;
    }
  }
  // Stamp on arming, not on success: a failed connect costs the same radio time
  // as a successful one, so it must throttle the next wake too. This is the only
  // SD write the wake path incurs, and only on units that have an RTC.
  APP_STATE.messageCheckMinuteOfDay = nowMinute;
  APP_STATE.saveToFile();
  return true;
}

void wakeFinish(const char* why) {
  if (wake.step == WakeStep::Idle) return;
  wifiOff();
  wake = WakeCheck{};  // back to Idle, and releases the held strings
  LOG_DBG("MSYNC", "Wake check finished (%s)", why);
}

// One polled connect step. 1 = associated, 0 = still trying, -1 = give up.
int wakeConnectStep() {
  if (WiFi.status() == WL_CONNECTED) {
    LOG_INF("MSYNC", "Wake check connected");
    return 1;
  }

  if (wake.attemptInFlight) {
    const wl_status_t st = WiFi.status();
    if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {  // fail fast -> next candidate
      WiFi.disconnect(true, false);
      wake.attemptInFlight = false;
    }
    return 0;
  }

  const auto& creds = WIFI_STORE.getCredentials();
  const WifiCredential* next = nullptr;
  if (!wake.triedLast) {
    wake.triedLast = true;
    if (!wake.lastSsid.empty()) next = WIFI_STORE.findCredential(wake.lastSsid);
  }
  while (next == nullptr && wake.nextIdx < creds.size()) {
    const WifiCredential* candidate = &creds[wake.nextIdx++];
    if (!wake.lastSsid.empty() && candidate->ssid == wake.lastSsid) continue;  // already tried first
    next = candidate;
  }
  if (next == nullptr) return -1;

  LOG_DBG("MSYNC", "Wake check trying saved network: %s", next->ssid.c_str());
  if (next->password.empty()) {
    WiFi.begin(next->ssid.c_str());
  } else {
    WiFi.begin(next->ssid.c_str(), next->password.c_str());
  }
  wake.attemptInFlight = true;
  return 0;
}
}  // namespace

bool MessageSync::syncBeforeSleep(size_t frameBufferSize, const LinkUpHook& whileLinkUp) {
  if (!SETTINGS.messageSyncEnabled) return false;
  const std::string base = baseUrl();
  if (base.empty()) return false;

  if (!connectHeadless()) {
    wifiOff();
    return false;
  }

  // The note half runs to completion first, unconditionally: it is tiny and
  // latency-sensitive, and it must never queue behind a book.
  bool staged = false;
  std::string latestId;
  if (probeLatest(base, latestId) == Probe::NewNote) {
    LOG_INF("MSYNC", "New note %s: downloading frame", latestId.c_str());
    const HttpDownloader::DownloadError err = downloadIncoming(base);
    if (err != HttpDownloader::OK) {
      LOG_ERR("MSYNC", "frame download failed (%d)", static_cast<int>(err));
      Storage.remove(INCOMING_FRAME);
    } else {
      // Promoted while the radio is still up. That is pure SD work either way,
      // and doing it here rather than after the teardown is what lets the hook
      // see the final "a new note is staged" answer.
      staged = promoteIncoming(latestId, frameBufferSize);
    }
  }

  // Everything else that needs this window happens here, on the link the connect
  // above already paid for. Bounded by the hook itself -- see BookSync::syncOnLink.
  if (whileLinkUp) whileLinkUp(base, staged);

  wifiOff();  // no longer needed, regardless of either half's outcome
  return staged;
}

void MessageSync::beginWakeCheck(size_t frameBufferSize) {
  if (wake.step != WakeStep::Idle) return;
  if (!SETTINGS.messageSyncEnabled) return;
  std::string base = baseUrl();
  if (base.empty()) return;

  WIFI_STORE.loadFromFile();
  if (WIFI_STORE.getCredentials().empty()) {
    LOG_DBG("MSYNC", "No saved WiFi credentials");
    return;
  }
  if (!wakeThrottleAllows()) return;

  wake = WakeCheck{};
  wake.step = WakeStep::Settle;
  wake.frameSize = frameBufferSize;
  wake.base = std::move(base);
  wake.lastSsid = WIFI_STORE.getLastConnectedSsid();
  wake.deadline = millis() + WAKE_DEADLINE_MS;
  wake.settleUntil = millis() + 100;
  wifiBeginSta();
  LOG_DBG("MSYNC", "Wake check armed");
}

void MessageSync::stepWakeCheck() {
  if (wake.step == WakeStep::Idle) return;
  if (static_cast<int32_t>(millis() - wake.deadline) >= 0) {
    wakeFinish("deadline");
    return;
  }

  switch (wake.step) {
    case WakeStep::Settle:
      if (static_cast<int32_t>(millis() - wake.settleUntil) < 0) return;
      wake.step = WakeStep::Connect;
      return;

    case WakeStep::Connect: {
      const int result = wakeConnectStep();
      if (result < 0) {
        wakeFinish("no network");
      } else if (result > 0) {
        wake.step = WakeStep::Probe;
      }
      return;
    }

    case WakeStep::Probe: {
      // One tiny GET plus the single TLS handshake. Blocking, but short.
      std::string latestId;
      if (probeLatest(wake.base, latestId) != Probe::NewNote) {
        wakeFinish("nothing new");
        return;
      }
      wake.latestId = std::move(latestId);
      wake.step = WakeStep::Download;
      return;
    }

    case WakeStep::Download: {
      LOG_INF("MSYNC", "Wake check: new note %s, downloading frame", wake.latestId.c_str());
      const HttpDownloader::DownloadError err = downloadIncoming(wake.base);
      // Radio off before the SD promote and before control returns to the
      // launcher: WiFi and a chapter build must never be resident at once.
      wifiOff();
      if (err != HttpDownloader::OK) {
        LOG_ERR("MSYNC", "wake frame download failed (%d)", static_cast<int>(err));
        Storage.remove(INCOMING_FRAME);
      } else {
        promoteIncoming(wake.latestId, wake.frameSize);
      }
      wakeFinish("done");
      return;
    }

    default:
      wakeFinish("bad state");
      return;
  }
}

void MessageSync::cancelWakeCheck() {
  if (wake.step == WakeStep::Idle) return;
  // Safe at any byte: the bytes only ever went to INCOMING_FRAME, so
  // current.frame is untouched and the note is simply re-fetched next time.
  Storage.remove(INCOMING_FRAME);
  wakeFinish("cancelled");
}

bool MessageSync::wakeCheckActive() { return wake.step != WakeStep::Idle; }

bool MessageSync::loadStagedNote(uint8_t* buffer, size_t bufferSize) {
  if (buffer == nullptr || bufferSize == 0) return false;

  HalFile file;
  if (!Storage.openFileForRead("MSYNC", CURRENT_FRAME, file)) return false;
  const size_t stagedSize = file.size();
  if (stagedSize != bufferSize) {
    LOG_ERR("MSYNC", "staged note size mismatch: %u != %u", static_cast<unsigned>(stagedSize),
            static_cast<unsigned>(bufferSize));
    file.close();
    return false;
  }
  const size_t bytesRead = file.read(buffer, bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    LOG_ERR("MSYNC", "staged note read short: %u of %u", static_cast<unsigned>(bytesRead),
            static_cast<unsigned>(bufferSize));
    return false;
  }
  return true;
}
