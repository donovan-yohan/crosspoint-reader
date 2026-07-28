#include "MessageSync.h"

#include <Arduino.h>
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

// Connect to a saved network, last-connected SSID first, within a hard overall
// deadline. Returns true if associated. No scan is issued (begin() finds the AP)
// to keep the window short.
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

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

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
}  // namespace

bool MessageSync::hasUnreadNote() {
  if (!Storage.exists(CURRENT_FRAME)) return false;
  const std::string id = readStagedId();
  if (id.empty()) return true;  // legacy / id-less frame: show (M1 behaviour)
  return id != APP_STATE.messageLastShownId;
}

void MessageSync::markCurrentNoteShown() {
  const std::string id = readStagedId();
  if (id.empty()) return;                            // nothing to dedup on
  if (APP_STATE.messageLastShownId == id) return;    // no change -> skip SD write
  APP_STATE.messageLastShownId = id;
  APP_STATE.saveToFile();
  LOG_DBG("MSYNC", "Marked note shown: %s", id.c_str());
}

void MessageSync::syncBeforeSleep(size_t frameBufferSize) {
  if (!SETTINGS.messageSyncEnabled) return;
  const std::string base = baseUrl();
  if (base.empty()) return;

  if (!connectHeadless()) {
    wifiOff();
    return;
  }

  // Cheap dedup check FIRST: fetch the mailbox's latest id (tiny GET).
  std::string latestId;
  if (!HttpDownloader::fetchUrl(base + SUFFIX_ID, latestId)) {
    LOG_DBG("MSYNC", "latest-id fetch failed");
    wifiOff();
    return;
  }
  latestId = trimId(std::move(latestId));
  if (latestId.empty()) {
    LOG_DBG("MSYNC", "mailbox empty");
    wifiOff();
    return;
  }

  // Skip the frame download when the latest already matches what we've shown or
  // what is already staged.
  if (latestId == APP_STATE.messageLastShownId || latestId == readStagedId()) {
    LOG_DBG("MSYNC", "No new note (latest=%s)", latestId.c_str());
    wifiOff();
    return;
  }

  LOG_INF("MSYNC", "New note %s: downloading frame", latestId.c_str());
  // openFileForWrite uses O_CREAT only (no parent-dir creation); a fresh device
  // that never received an M1 web upload has no /.love-notes yet, so ensure it.
  Storage.ensureDirectoryExists(NOTES_DIR);
  const HttpDownloader::DownloadError err = HttpDownloader::downloadToFile(base + SUFFIX_FRAME, INCOMING_FRAME);
  wifiOff();  // WiFi no longer needed regardless of outcome

  if (err != HttpDownloader::OK) {
    LOG_ERR("MSYNC", "frame download failed (%d)", static_cast<int>(err));
    Storage.remove(INCOMING_FRAME);
    return;
  }

  // Validate exact framebuffer size before promoting; a wrong-sized/interrupted
  // transfer must never replace the last valid frame (backlog invariant #4).
  size_t incomingSize = 0;
  {
    HalFile f;
    if (Storage.openFileForRead("MSYNC", INCOMING_FRAME, f)) incomingSize = f.size();
  }
  if (incomingSize != frameBufferSize) {
    LOG_ERR("MSYNC", "frame size mismatch: %u != %u", static_cast<unsigned>(incomingSize),
            static_cast<unsigned>(frameBufferSize));
    Storage.remove(INCOMING_FRAME);
    return;
  }

  // Promote: replace current.frame, then record its id sidecar.
  Storage.remove(CURRENT_FRAME);
  if (!Storage.rename(INCOMING_FRAME, CURRENT_FRAME)) {
    LOG_ERR("MSYNC", "promote rename failed");
    Storage.remove(INCOMING_FRAME);
    return;
  }
  Storage.writeFile(CURRENT_ID, String(latestId.c_str()));
  LOG_INF("MSYNC", "Staged note %s", latestId.c_str());
}
