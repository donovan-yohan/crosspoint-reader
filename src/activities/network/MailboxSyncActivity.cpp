#include "MailboxSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_random.h>

#include <cstdio>
#include <string_view>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/BookSync.h"
#include "network/HttpDownloader.h"
#include "network/MessageSync.h"
#include "network/PeerProbe.h"
#include "network/WallpaperSync.h"
#include "util/QrUtils.h"
#include "util/TaskWatchdog.h"

using MailboxSync::CYCLE_DRAIN_BUDGET_MS;
using MailboxSync::MAX_ITEMS_PER_CYCLE;
using MailboxSync::MAX_WIFI_BODY_BYTES;
using MailboxSync::PEER_DISCOVERY_MS;
using MailboxSync::PEER_FAST_ATTEMPTS;
using MailboxSync::PEER_RETRY_MS;
using MailboxSync::PHONE_JOIN_WAIT_MS;
using MailboxSync::POLL_ACTIVE_MS;
using MailboxSync::POLL_IDLE_MS;
using MailboxSync::POLL_WARM_CYCLES;
using MailboxSync::POLL_WARM_MS;
using MailboxSync::PSK_MAX_BYTES;
using MailboxSync::PSK_MIN_BYTES;
using MailboxSync::WIFI_PICKUP_MAX_MISSES;
using MailboxSync::SESSION_CAP_MS;
using MailboxSync::SSID_MAX_BYTES;
using MailboxSync::Transport;
using MailboxSync::WIFI_PICKUP_BUDGET_MS;
using MailboxSync::WIFI_SHARE_PATH;

namespace {
// The SSID the app's WifiNetworkSpecifier matches on, deliberately the same
// string the shipped transfer-mode AP uses (CrossPointWebServerActivity.cpp:23):
// the phone side has one network name to know about, not two.
constexpr const char* AP_SSID = "CrossPoint-Reader";
constexpr uint8_t AP_CHANNEL = 1;
// The whole candidate space PeerProbe sweeps is derived from this: four leases.
constexpr uint8_t AP_MAX_CONNECTIONS = 4;

// PER-DEVICE AP PASSPHRASE -- appendix A3's second required fix.
//
// The shipped transfer-mode AP is open (AP_PASSWORD is a compile-time nullptr),
// and A3 is explicit that an open AP is not good enough here even with the
// boxId-free health probe in front of the capability URL: a station that answers
// /cp-proxy can still impersonate the forwarder and serve the reader a frame or an
// epub of its choosing, bounded only by the staging size gates against a manifest
// from that same station. A PSK is what stops an uninvited station occupying a
// lease at all.
//
// MINTED ONCE PER DEVICE, THEN REUSED FOREVER, and that is a bug fix rather than a
// preference: the first cut generated a fresh passphrase on every activity entry,
// which invalidated the app's saved "Reader AP password" on every single session.
// The phone would refuse to auto-join a network whose PSK had silently changed and
// the user had to retype the panel every sync -- the save-once pairing model the
// app is built around, dead. The AP is a stable piece of this device's identity, so
// the passphrase has to be too.
//
// NOT DERIVED FROM THE MAC, which is the obvious way to get stability for free and
// is worse than useless: the AP's BSSID is in every beacon, so a MAC-derived
// passphrase is computable by anyone in radio range (PeerProbe.h says the same).
// Random once, stored once, is the only shape that is both stable and unguessable.
//
// STORED IN APP_STATE (state.json on the SD card). The threat this defends against
// is a station squatting a DHCP lease on a 30-minute foreground AP; someone holding
// the card in their hand has the books, the notes and the mailbox URL already, so
// storage plaintext is not the weak link. It rides the same store as
// messageLastDisplayedId and gets the same one-write-when-it-changes treatment.
//
// Minted AFTER WiFi.mode(WIFI_AP) has powered the radio: esp_fill_random is a true
// hardware RNG only while WiFi or BT is enabled, and a PSK from the pseudo-random
// fallback would defeat the point. That is why devicePsk() is called from inside
// startPhoneApLink() and not at boot -- and it also means a device whose owner only
// ever syncs over a saved network never mints one at all.
constexpr size_t PSK_LEN = 10;  // 10 chars over a 32-symbol alphabet = 50 bits.
// Unambiguous alphabet: no 0/O, no 1/I/L, so a passphrase read off e-ink and typed
// into a phone cannot fail on a glyph. 32 symbols exactly, so the rejection-free
// mask below is uniform.
constexpr char PSK_ALPHABET[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ#";
static_assert(sizeof(PSK_ALPHABET) - 1 == 32, "PSK alphabet must be exactly 32 symbols for a uniform 5-bit draw");
// WPA2 passphrase bounds. PSK_LEN sits inside them by construction; these exist to
// judge a value read back off the card, which a user can hand-edit, a truncated
// write can corrupt, and -- since the settings surfaces landed -- a user can type.
// A too-short PSK would make softAP() fail outright, so a stored value that is out
// of bounds is treated as "not minted" rather than trusted.
//
// Aliases, not a second opinion: the settings entry that lets a user set this value
// checks the SAME two numbers, so "what the editor accepts" and "what the AP will
// actually run with" cannot drift apart.
constexpr size_t PSK_MIN_LEN = CrossPointState::MAILBOX_AP_PSK_MIN_LEN;
constexpr size_t PSK_MAX_LEN = CrossPointState::MAILBOX_AP_PSK_MAX_LEN;
static_assert(PSK_LEN >= PSK_MIN_LEN && PSK_LEN <= PSK_MAX_LEN, "generated PSK must be a legal WPA2 passphrase");

std::string generatePsk() {
  uint8_t raw[PSK_LEN];
  esp_fill_random(raw, sizeof(raw));
  std::string psk;
  psk.reserve(PSK_LEN);
  for (const uint8_t b : raw) psk.push_back(PSK_ALPHABET[b & 0x1F]);
  return psk;
}

// The device's AP passphrase: the stored one if there is a usable one, otherwise a
// freshly minted one, persisted before it is ever shown so the value on the panel
// and the value on the card can never disagree.
//
// The passphrase is also USER-SETTABLE, from Settings > System on the device and
// from the web settings UI (key "mailboxApPsk"). Neither editor mints: they write a
// value or clear it, and clearing it lands exactly here -- an empty stored value is
// out of bounds, so the next AP session mints a fresh one. That is the whole
// "regenerate" story, and it is why the join panel itself still has no button: the
// mint belongs to the session that needs the radio up, not to a settings screen.
//
// The save is the only SD write this mode adds, and it happens on exactly one
// session in the life of the device. If it fails, the passphrase is still used for
// THIS session -- a sync the user is standing in front of is worth more than the
// pairing convenience -- and the next session simply mints again.
std::string devicePsk() {
  const std::string& stored = APP_STATE.mailboxApPsk;
  if (stored.size() >= PSK_MIN_LEN && stored.size() <= PSK_MAX_LEN) return stored;

  const std::string minted = generatePsk();
  APP_STATE.mailboxApPsk = minted;
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("MSYNCUI", "could not persist the AP passphrase; the app will have to be re-paired next session");
  } else {
    LOG_INF("MSYNCUI", "Minted this device's AP passphrase");
  }
  return minted;
}

constexpr int QR_SIZE = 180;

// Consecutive failed note polls before the screen admits it is not getting
// through. Three at a 4 s cadence is ~12 s, long enough that a single dropped
// packet or a hotspot re-associating does not flash a scary line at the user.
//
// STILL ~12 s AFTER THE CADENCE WENT ADAPTIVE, and that is why a failed cycle
// arms POLL_IDLE_MS rather than the warm interval: retrying a broken link five
// times faster neither fixes it nor is free, and it would drag this threshold
// down to ~2 s, which is well inside the ordinary recovery time of the events it
// is meant to ride out.
constexpr int STALL_THRESHOLD = 3;

// How often Back is sampled from inside a book transfer. The download loop hands
// us a callback per 1 KB chunk, which at any real throughput is far too often to
// run the input stack on.
constexpr uint32_t ABORT_POLL_MS = 250;

// Absolute deadline `budget` ms from now, never past `cap` (absolute, 0 =
// uncapped). Same shape as MessageSync's and PeerProbe's: a phase budget must
// never outlive the session cap.
uint32_t deadlineWithin(uint32_t cap, uint32_t budget) {
  const uint32_t want = millis() + budget;
  if (cap == 0) return want;
  return static_cast<int32_t>(want - cap) > 0 ? cap : want;
}

bool expired(uint32_t deadline) { return deadline != 0 && static_cast<int32_t>(millis() - deadline) >= 0; }

// "1.2 MB" / "812 KB". Sizes here come off a manifest and are only ever shown, so
// one decimal is plenty and avoids a float format for the common case.
std::string humanBytes(size_t bytes) {
  char buf[24];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else {
    snprintf(buf, sizeof(buf), "%u KB", static_cast<unsigned>((bytes + 1023) / 1024));
  }
  return std::string(buf);
}

// std::hash over the visible name, so the paint signature can compare "is the
// screen still showing the same book" without keeping a second copy of the string.
size_t hashName(const std::string& s) { return std::hash<std::string>{}(s); }

// Origin of a composed peer base: "http://192.168.4.2:8080/m/abc" -> the part
// before "/m". The exact complement of PeerProbe::pathOf, which is what appended
// that path in the first place, so this cannot disagree with it about where the
// authority ends -- including on a base that carries a port, where a naive
// "find the first slash" would stop at the scheme's own "//".
std::string originOf(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  const size_t authorityStart = (schemeEnd == std::string::npos) ? 0 : schemeEnd + 3;
  const size_t slash = url.find('/', authorityStart);
  return slash == std::string::npos ? url : url.substr(0, slash);
}

// Backslash-escape the characters the zxing WIFI: format gives meaning to, so a
// value carrying one cannot end its field early. Applied to the SSID and the
// passphrase in the join QR.
std::string escapeQrField(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

// One trailing CR, so the app may end its lines CRLF or LF.
std::string_view stripCr(std::string_view line) {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  return line;
}

// Split "ssid\npassword\n" and judge both halves. Returns false without touching
// the outputs for anything the reader will not save, which is the whole point:
// this is the only gate between a station on the AP and a network entry on the
// card, and the caller does not ack what it did not accept.
bool parseWifiPayload(const std::string& body, std::string& ssid, std::string& password) {
  const size_t firstNl = body.find('\n');
  if (firstNl == std::string::npos) return false;  // one line is not this contract
  const size_t secondNl = body.find('\n', firstNl + 1);

  const std::string_view view(body);
  const std::string_view ssidLine = stripCr(view.substr(0, firstNl));
  const std::string_view pskLine = stripCr(secondNl == std::string::npos
                                               ? view.substr(firstNl + 1)
                                               : view.substr(firstNl + 1, secondNl - firstNl - 1));

  // STRICT ABOUT WHAT FOLLOWS, unlike PeerProbe's health body which tolerates
  // anything after its token. That probe only decides "is this the app"; this one
  // writes a network to the card. Without this check an HTML error page or a JSON
  // document served with a 200 has its first two lines read as a credential, and a
  // second line that happens to be 8..63 printable characters is all it takes to
  // plant a network the user never typed.
  if (secondNl != std::string::npos) {
    for (size_t i = secondNl + 1; i < body.size(); ++i) {
      const char c = body[i];
      if (c != '\n' && c != '\r' && c != ' ' && c != '\t') return false;
    }
  }

  if (ssidLine.empty() || ssidLine.size() > SSID_MAX_BYTES) return false;
  // Control bytes cannot come out of a text field on the phone and cannot be drawn
  // on the panel. High bytes are deliberately left alone: an SSID is arbitrary
  // octets and a UTF-8 network name is an ordinary thing to own.
  for (const char c : ssidLine) {
    const auto b = static_cast<unsigned char>(c);
    if (b < 0x20 || b == 0x7F) return false;
  }

  // Empty means the network is open. Anything else is a WPA passphrase, which the
  // standard defines as 8..63 printable ASCII characters -- and which is exactly
  // what softAP/begin will reject later, loudly and far from here, if it is not.
  if (!pskLine.empty()) {
    if (pskLine.size() < PSK_MIN_BYTES || pskLine.size() > PSK_MAX_BYTES) return false;
    for (const char c : pskLine) {
      const auto b = static_cast<unsigned char>(c);
      if (b < 0x20 || b > 0x7E) return false;
    }
  }

  ssid.assign(ssidLine);
  password.assign(pskLine);
  return true;
}
}  // namespace

void MailboxSyncActivity::onEnter() {
  Activity::onEnter();

  LOG_INF("MSYNCUI", "Mailbox sync entered (transport=%s), free heap %d",
          transport == Transport::SavedNetwork ? "saved-network" : "phone-ap", ESP.getFreeHeap());

  state = State::Connecting;
  sessionDeadline = millis() + SESSION_CAP_MS;
  // 0 is the "no cap" sentinel everywhere in this stack (deadlineWithin, expired,
  // HttpDownloader's deadlineMs), so the one millis() value that would turn the
  // session cap OFF is stepped over. One tick of skew, once every 49 days of
  // uptime, against a mode that would otherwise hold the radio up unbounded.
  if (sessionDeadline == 0) sessionDeadline = 1;
  nextPollAt = millis();

  // Painted from here, BLOCKING, and that is load-bearing rather than cosmetic:
  // every paint in this activity happens on the main task and completes before the
  // next state mutation, so the render task can never read `state`, `apPsk` or
  // `targetName` while this task is assigning them. The link is then brought up
  // from the first loop() instead of here, so the Connecting screen is on the
  // panel before the connect blocks for up to its 6 s budget.
  requestUpdateAndWait();
  painted = signature();
}

void MailboxSyncActivity::onExit() {
  Activity::onExit();

  LOG_INF("MSYNCUI", "Mailbox sync exiting: %d notes, %d books, free heap %d", notesStaged, booksReceived,
          ESP.getFreeHeap());

  // UNCONDITIONAL, on every path -- Back, session cap, connect failure, AP raise
  // failure, or a poll that never succeeded. The radio must never outlive this
  // activity: nothing else in the system is holding it up, and it is the single
  // largest current draw on the device.
  teardownRadio();

  // A live-sync session is a long WiFi session with SD writes and possibly a
  // multi-megabyte streamed download, so it fragments a PSRAM-less heap at least
  // as badly as transfer mode does, and WIFI_OFF does not defragment anything. The
  // user explicitly entered a mode and expects to land back at Home, which is
  // exactly where silentRestart() goes. Skipped when no link ever came up: see
  // sessionRan.
  if (sessionRan) {
    delay(30);
    silentRestart();
  }
}

bool MailboxSyncActivity::preventAutoSleep() { return state != State::Failed && state != State::Finished; }

void MailboxSyncActivity::loop() {
  // Back, at any point. The other place it is sampled is pollForAbort(), from
  // inside a book transfer, which latches `exiting` without being able to finish()
  // -- so the pop has to be driven from here, or an abort mid-transfer would leave
  // the activity sitting on a dead screen for ever.
  if (!exiting && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    LOG_DBG("MSYNCUI", "Back pressed: leaving mailbox sync");
    exiting = true;
  }
  if (exiting) {
    if (!finishCalled) {
      finishCalled = true;
      finish();
    }
    return;
  }

  // One-shot link bring-up, now that the Connecting screen has been painted.
  if (!linkStarted) {
    linkStarted = true;
    radioTouched = true;
    const bool up = (transport == Transport::SavedNetwork) ? startSavedNetworkLink() : startPhoneApLink();
    if (!up) {
      paintIfChanged();  // Failed, with a reason
      return;
    }
    sessionRan = true;
    paintIfChanged();
    return;
  }

  // Terminal states do nothing but wait for Back.
  if (state == State::Failed || state == State::Finished) return;

  // The session cap, checked before any work so a poll cannot start on a budget
  // it cannot finish inside. Never leans on the 60 s per-socket-op timeout.
  if (expired(sessionDeadline)) {
    LOG_INF("MSYNCUI", "Session cap reached");
    state = State::Finished;
    failureText = StrId::STR_SYNC_TIME_LIMIT;
    // THE RADIO GOES DOWN HERE, NOT AT onExit. A terminal state waits for the user
    // to read the screen and press Back, and that wait is unbounded -- the whole
    // point of the session cap is that this mode cannot hold the radio up after the
    // user has walked away, so deferring teardown to the exit that user has to
    // provide would defeat it. onExit's teardown is idempotent and becomes a no-op.
    teardownRadio();
    paintIfChanged();
    return;
  }

  if (transport == Transport::PhoneAp) {
    stepPhoneApLink();
    // WHICH STATES ARE "THE LINK IS UP AND THE POLL MAY RUN". Stalled belongs in
    // this list and leaving it out was a permanent dead-end: Stalled is a DISPLAY
    // state ("polls are failing, still trying"), not a link state, and the only
    // code that clears it is runPollCycle() -- so returning here on Stalled stopped
    // the polls that are the sole way out of it, and the AP transport sat on
    // "cannot reach the mailbox" for the rest of the session cap while the STA
    // transport (which does not pass through this guard) kept polling. That is
    // exactly the fork between the two transports A4 forbids.
    //
    // REGRESSION RULE: every new State must be classified here explicitly. A
    // non-terminal state that can only be left by a successful poll MUST fall
    // through; only states where there is genuinely no link to poll on
    // (Connecting, WaitingPhone, Linking) may return.
    if (state != State::Polling && state != State::Receiving && state != State::Stalled) {
      paintIfChanged();
      return;
    }
  }

  if (static_cast<int32_t>(millis() - nextPollAt) < 0) return;

  // ADAPTIVE CADENCE. The interval is charged to an IDLE link and to nothing
  // else: a cycle that completed something goes again with no wait at all, so a
  // queued backlog drains back-to-back instead of one item per interval, and the
  // idle interval only reappears once the mailbox has actually run dry. Every
  // number is measured from the END of the cycle, so a cycle that took longer
  // than its own interval simply runs again on the next loop() -- there is no
  // catch-up burst and no accumulated debt.
  switch (runPollCycle()) {
    case CycleResult::Moved:
      // Demonstrably non-empty: re-arm the warm window too, because whatever put
      // one item there is quite likely still putting more.
      idleCycles = 0;
      nextPollAt = millis() + POLL_ACTIVE_MS;
      break;
    case CycleResult::Idle:
      if (idleCycles < POLL_WARM_CYCLES) {
        ++idleCycles;
        nextPollAt = millis() + POLL_WARM_MS;
      } else {
        nextPollAt = millis() + POLL_IDLE_MS;
      }
      break;
    case CycleResult::Failed:
      // Not "the mailbox is empty" -- see STALL_THRESHOLD. Spend the warm window
      // so a link that comes back does not get a second free run of fast polls,
      // and back off now.
      idleCycles = POLL_WARM_CYCLES;
      nextPollAt = millis() + POLL_IDLE_MS;
      break;
  }
  paintIfChanged();
}

// --- Link bring-up ---------------------------------------------------------

bool MailboxSyncActivity::startSavedNetworkLink() {
  // Appendix A4: the base is the configured mailbox URL, and it is read here --
  // once, at entry -- and then only ever passed as a parameter. Nothing deeper in
  // the loop reads SETTINGS, which is the whole structural requirement A3 places
  // on A4.
  const std::string configured = MessageSync::configuredBase();
  if (configured.empty()) {
    fail(StrId::STR_SYNC_NO_URL);
    return false;
  }

  resetTaskWatchdogIfSubscribed();
  if (!MessageSync::connectSavedNetwork()) {
    fail(StrId::STR_SYNC_NO_NETWORK);  // fail() tears the radio down
    return false;
  }

  base = configured;
  state = State::Polling;
  // The warm window starts at link-up on both transports, which is the whole
  // point of it: the seconds right after the link comes up are when a queue the
  // sender is still filling is most likely to gain an item.
  idleCycles = 0;
  nextPollAt = millis();
  LOG_INF("MSYNCUI", "STA link up, syncing against the configured mailbox");
  return true;
}

bool MailboxSyncActivity::startPhoneApLink() {
  // Appendix A4's structural requirement, same as the STA path: SETTINGS is read
  // ONCE, here, at entry. The AP path cannot pass the result straight into `base`
  // the way the STA path can -- the peer base is only composable after discovery,
  // and it is recomposed if the phone drops and rejoins on a different lease -- so
  // it is held in `configuredUrl` and every later use reads that member. Nothing
  // deeper in the loop calls MessageSync::configuredBase().
  const std::string configured = MessageSync::configuredBase();
  if (configured.empty()) {
    fail(StrId::STR_SYNC_NO_URL);
    return false;
  }
  // Checked HERE rather than left to PeerProbe::discoverBase, which also fails
  // closed on it but from a place where the only thing the screen could honestly
  // say is "no phone found" -- a misdiagnosis the user cannot act on. A base with
  // no /m/{boxId} path is a settings problem, and it is told as one.
  if (PeerProbe::pathOf(configured).empty()) {
    fail(StrId::STR_SYNC_NO_BOX_PATH);
    return false;
  }

  // The shipped AP recipe (CrossPointWebServerActivity::startAccessPoint) MINUS
  // four things, every one of them deliberate:
  //   1. no startWebServer()  -- the reader is a pure CLIENT on its own AP here,
  //      so there is no WebServer on 80, no WebSocketsServer on 81, and no
  //      /upload, /delete or /rename handler reachable from the peer link. That
  //      deletes essentially the whole security objection an AP would carry.
  //   2. no captive-portal DNSServer -- a forwarder found by probing the DHCP
  //      range needs no name resolution, and the server costs heap in the
  //      tightest place in the system.
  //   3. no mDNS            -- same argument.
  //   4. a real PSK         -- see devicePsk().
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);

  // MODEM SLEEP: DECIDED HERE, NOT INHERITED. Skipping CrossPointWebServer::begin()
  // also skips its WiFi.setSleep(false) (CrossPointWebServer.cpp:117-120, whose
  // comment calls it critical for reliable operation), and A3 is explicit that a
  // client-only AP mode must make this call itself rather than run on whatever the
  // core's default happens to be. The choice is OFF, i.e. no modem sleep:
  //   - the reader is the HTTP CLIENT on this link and issues a fresh request every
  //     4 s, so sleep latency is paid on every poll of the whole session, not once;
  //   - ESP-IDF power save is documented for station mode, so on an AP-only
  //     interface the battery win is unproven while the latency cost is not;
  //   - the battery bound for this mode is the 30-minute session cap plus the fact
  //     that a foreground mode is watched by the user who started it -- not a power
  //     mode that would trade poll reliability for an unmeasured saving.
  // Revisit only with a number off a device (bracket the AP raise with the existing
  // free-heap/current instrumentation), which is what A3 asks for.
  WiFi.setSleep(false);
  delay(100);

  apSsid = AP_SSID;
  // Stable across sessions -- the app pairs once. Called from here, after mode(),
  // because a first-ever mint needs the radio powered for the hardware RNG.
  apPsk = devicePsk();

  if (!WiFi.softAP(apSsid.c_str(), apPsk.c_str(), AP_CHANNEL, false, AP_MAX_CONNECTIONS)) {
    LOG_ERR("MSYNCUI", "softAP failed to start");
    fail(StrId::STR_SYNC_LINK_FAILED);
    return false;
  }
  delay(100);  // let the AP and its DHCP server finish coming up

  // Keep the configured base around; the peer base is composed from it the moment
  // discovery succeeds, and recomposed if the phone drops and rejoins on a
  // different lease. This member is the ONLY copy the loop ever reads, and it is
  // also why pathOf() is validated once above instead of on every sweep.
  configuredUrl = configured;
  base.clear();
  state = State::WaitingPhone;
  phoneWaitDeadline = millis() + PHONE_JOIN_WAIT_MS;
  // The first station to appear is probed the moment it appears; the fast retry
  // budget starts unspent.
  nextProbeAt = millis();
  probeAttempts = 0;
  LOG_INF("MSYNCUI", "AP up as %s, waiting for a phone", apSsid.c_str());
  return true;
}

void MailboxSyncActivity::stepPhoneApLink() {
  const bool stationPresent = WiFi.softAPgetStationNum() > 0;

  // A phone that walks away, locks, or drops the peer network takes the base with
  // it: the lease it held may go to someone else, so the discovered base must not
  // survive the disconnect. Straight back to waiting, with a fresh join budget --
  // the user is still standing there and may simply be re-opening the app.
  if (!stationPresent && state != State::WaitingPhone) {
    LOG_INF("MSYNCUI", "Peer left the AP; waiting for a phone again");
    base.clear();
    state = State::WaitingPhone;
    phoneWaitDeadline = millis() + PHONE_JOIN_WAIT_MS;
    // A rejoin is a fresh link, so it gets a fresh fast-probe budget: the phone
    // that comes back is exactly the case the fast cadence is for, and carrying a
    // spent counter across would make the second sync of a session slower than
    // the first for no reason.
    nextProbeAt = millis();
    probeAttempts = 0;
    return;
  }

  if (state == State::WaitingPhone) {
    if (!stationPresent) {
      // A bounded wait, per PeerProbe's contract with its caller: probing an AP no
      // phone has joined is four guaranteed failures, and an unbounded wait would
      // hold the radio up for the whole session cap for nothing.
      if (expired(phoneWaitDeadline)) {
        LOG_INF("MSYNCUI", "No phone joined within the wait budget");
        fail(StrId::STR_SYNC_LINK_FAILED);  // and the AP comes down with it
      }
      return;
    }
    // A STATION IS ASSOCIATED BUT UNPROVEN, AND THE RETRY IS ON THE PROBE CADENCE.
    // Re-discovery used to run on every loop() iteration, so a station that
    // associates without answering /cp-proxy -- a phone that scanned the QR before
    // opening the app, an app still binding its listener, a backgrounded app, or a
    // stranger's device holding a lease -- drove WaitingPhone -> Linking -> failed
    // sweep -> WaitingPhone with no gate at all. phoneWaitDeadline cannot bound that
    // because it is only consulted while no station is present. Two full-frame
    // repaints of two completely different layouts per attempt, forever: A4's
    // strobing-panel finding, exactly.
    //
    // THE GATE IS nextProbeAt AND NOT nextPollAt, which is the fix for the other
    // half of that story. Reusing the poll interval here meant an app whose
    // listener came up 200 ms after the first probe waited 4 s for the second, and
    // that is the single most common few seconds lost in this mode. The
    // strobing-panel argument survives untouched: probeAnnounced latches on the
    // first attempt, so attempts 2..N cost ZERO repaints no matter how close
    // together they are -- the panel simply keeps showing the join screen, which
    // is the honest thing to show while the app is not answering yet.
    if (static_cast<int32_t>(millis() - nextProbeAt) < 0) return;

    LOG_INF("MSYNCUI", "A station joined; looking for the forwarder");
    state = State::Linking;
    // "Looking for the app..." before the probe sweep blocks -- but ONCE per link,
    // not once per retry. `probeAnnounced` latches here and is cleared only by a
    // discovery that actually succeeded, so a station that never proves itself
    // leaves the join panel (SSID, PSK, QR) on the screen, which is both the honest
    // thing to show and a signature that stops changing: the retries below then cost
    // zero refreshes.
    if (!probeAnnounced) {
      probeAnnounced = true;
      paintIfChanged();
    }
  }

  if (state == State::Linking) {
    // discoverBase() is the ONLY way a peer base is obtained, and it returns one
    // only for a candidate that answered the boxId-free /cp-proxy health path. The
    // capability URL therefore cannot reach an unproven station by construction --
    // appendix A3's sharpest security finding, closed structurally rather than by
    // remembering to probe first.
    resetTaskWatchdogIfSubscribed();
    // MORE ATTEMPTS OF SHORTER LENGTH, NOT A LONGER SEARCH. The sweep's own bound
    // (PEER_DISCOVERY_MS) is unchanged; what changes is how long any ONE candidate
    // may hold it up while the fast attempts last. A forwarder on a one-hop plain
    // HTTP link answers in milliseconds, so a second is already generous, and
    // spending the saving on another attempt half a second later is strictly
    // better against a listener that is about to open than sitting on a socket
    // that is never going to answer.
    const bool fastPhase = probeAttempts < PEER_FAST_ATTEMPTS;
    const std::string peer =
        PeerProbe::discoverBase(configuredUrl, deadlineWithin(sessionDeadline, PEER_DISCOVERY_MS),
                                PeerProbe::PROXY_PORT,
                                fastPhase ? PeerProbe::FAST_CANDIDATE_BUDGET_MS : PeerProbe::CANDIDATE_BUDGET_MS);
    if (peer.empty()) {
      // The station is associated but is not answering as a forwarder -- the app
      // is not open yet, or it is a device that just joined the network. Fall back
      // to waiting rather than failing: the phone is still there and the next
      // attempt will try again -- ON THE PROBE CADENCE, which is what this line
      // arms. Without it the WaitingPhone gate above lets the sweep re-run
      // immediately and the panel strobes.
      //
      // The interval reverts to POLL_IDLE_MS once the fast attempts are spent, so
      // a station that is simply never going to answer costs exactly what it
      // always did for the remaining ~29 minutes of the session cap.
      if (probeAttempts < PEER_FAST_ATTEMPTS) ++probeAttempts;
      nextProbeAt = millis() + (probeAttempts < PEER_FAST_ATTEMPTS ? PEER_RETRY_MS : POLL_IDLE_MS);
      state = State::WaitingPhone;
      return;
    }
    base = peer;
    state = State::Polling;
    // A new link is a fresh start for the failure bookkeeping: carrying a spent
    // consecutiveFailures across a re-link would drop the very next failed poll
    // straight back into Stalled. Clearing probeAnnounced buys the next
    // drop-and-rejoin its one "Looking for the app..." paint back.
    consecutiveFailures = 0;
    probeAnnounced = false;
    // The fast-probe budget is spent per LINK, not per session, so the next
    // drop-and-rejoin gets its quick sweeps back.
    probeAttempts = 0;
    // A link that has just come up is the strongest reason there is to poll
    // eagerly: the app is open in the user's hand and its queue is being filled
    // right now. Arms the warm window; the first poll below is immediate.
    idleCycles = 0;
    // A re-discovered base is the only point at which the thing on the other end
    // can have become a different phone, or the same phone with something staged
    // that was not staged before. It is therefore the one place the pickup's
    // give-up latch is allowed to re-arm; anything cheaper would put the log
    // flood back. The REFUSAL latch is deliberately not reset with it: a
    // credential this reader cannot use is still the same credential.
    wifiPickupMisses = 0;
    nextPollAt = millis();  // do not make the user wait a cadence for the first poll
    LOG_INF("MSYNCUI", "Peer link up");
    // Painted before the first poll rather than after it, or the panel would still
    // read "Looking for the app..." for as long as that poll's note pass and book
    // window take -- which is up to half a minute of the screen saying the wrong
    // thing about a link that is already up.
    paintIfChanged();
  }
}

// --- The poll cycle --------------------------------------------------------

MailboxSyncActivity::CycleResult MailboxSyncActivity::runPollCycle() {
  if (base.empty()) return CycleResult::Idle;

  resetTaskWatchdogIfSubscribed();

  // "Did this cycle COMPLETE anything." Deliberately not "did anything happen":
  // a window that named a target and did not finish it is a resume, and calling
  // that Moved would arm the zero-interval cadence on an outcome that can repeat
  // forever (a manifest entry the server will never serve names a target every
  // single window and completes none). A completed item cannot repeat -- it is
  // recorded in the id-keyed state file and never fetched again -- so Moved is
  // the one signal that is safe to spend a zero interval on. An in-flight
  // transfer gets the warm cadence instead, by falling through as Idle.
  bool completedSomething = false;

  // THE WI-FI HANDOVER GOES FIRST, AND ONLY ONCE PER SESSION. It is the smallest
  // request in the cycle -- one bounded local GET -- and it is the one the user is
  // standing over the reader waiting for, whereas the note pass can block for ten
  // seconds and the book window for thirty. Gated on the AP transport because the
  // saved-network transport's origin is a public host on the internet, and gated
  // on `wifiSavedSsid` because a handover that landed must not be re-applied every
  // four seconds for the rest of the session.
  if (transport == Transport::PhoneAp && wifiSavedSsid.empty()) tryWifiHandoff();

  // NOTES FIRST, ALWAYS -- the same ordering the sleep-entry window guarantees.
  // The frame is small and the note is the latency-sensitive half.
  const MessageSync::NoteResult note = MessageSync::syncOnLink(
      base, renderer.getBufferSize(), deadlineWithin(sessionDeadline, MessageSync::NOTE_PASS_BUDGET_MS));

  if (note == MessageSync::NoteResult::Failed) {
    // A failed poll is not an exit. A hotspot re-associating, a phone that locked
    // for a moment, a mailbox behind a flaky cell link -- all of these recover, and
    // the user asked for this mode precisely because they are waiting for
    // something. Only the screen changes, and only after STALL_THRESHOLD.
    if (++consecutiveFailures >= STALL_THRESHOLD && state == State::Polling) {
      state = State::Stalled;
    }
    return CycleResult::Failed;
  }
  consecutiveFailures = 0;
  if (state == State::Stalled) state = State::Polling;

  if (note == MessageSync::NoteResult::Staged) {
    // Staged, NOT shown -- and staging deliberately does not consume the note's
    // display turn (M2 #4): this frame gets the panel at the next sleep-entry,
    // once, and then the wallpaper comes back. The screen reports it; it does not
    // render it, and it must not -- MessageDisplayActivity paints a FULL_REFRESH
    // multi-flash waveform and would reintroduce the interrupting note.
    notesStaged++;
    completedSomething = true;
    LOG_INF("MSYNCUI", "Note staged (%d this session)", notesStaged);
  }

  if (expired(sessionDeadline)) return completedSomething ? CycleResult::Moved : CycleResult::Idle;

  // Shared by both transfer phases below, because they paint the same three
  // fields into the same Receiving state -- the only thing the user cares about is
  // "this named thing is arriving, this far along", and a book and a wallpaper are
  // the same sentence.
  const auto announceTarget = [this](const std::string& name, const size_t bytes, const size_t have) {
    targetName = name;
    targetBytes = bytes;
    targetHave = have;
    state = State::Receiving;
    // Paint here, before the transfer, so the panel names the target while the
    // bytes are moving instead of after. It costs one refresh out of the window
    // budget, and only when something on the screen actually changed.
    paintIfChanged();
  };

  // DRAIN, RATHER THAN ONE ITEM PER INTERVAL.
  //
  // BookSync and WallpaperSync each fetch at most ONE item per call -- that is
  // their contract and it is not changing, because "resume across bounded
  // windows" is what makes an unattended sleep-entry sync possible at all. What
  // WAS wrong is that this loop called each of them exactly once and then slept
  // the poll interval, so the per-call limit became a per-INTERVAL limit and a
  // mailbox holding five books took at least five intervals of pure idling on top
  // of five transfers. Calling them again while items keep landing costs one
  // manifest round trip per extra item and nothing else.
  //
  // THE LOOP ONLY CONTINUES ON A COMPLETION, never on "a target was named". A
  // window that named a target and did not finish it will name the same target
  // again, so looping on that is a way to spin against an entry the server will
  // not serve; the cadence handles the resume case instead, at POLL_WARM_MS. Two
  // bounds on top of that: MAX_ITEMS_PER_CYCLE on the number of passes and
  // `drainDeadline` on their total wall clock.
  //
  // EVERY DEADLINE IS STILL CAPPED BY THE SESSION DEADLINE. drainDeadline is
  // deadlineWithin(sessionDeadline, ...), and each window is deadlineWithin() of
  // THAT, so the min() capping composes: no window can outlive the drain budget
  // and no drain budget can outlive the session cap.
  uint32_t drainDeadline = deadlineWithin(sessionDeadline, CYCLE_DRAIN_BUDGET_MS);
  // 0 is the "no cap" sentinel throughout this stack, so it is stepped over here
  // for the same reason onEnter steps over it for the session deadline: one tick
  // of skew once every 49 days, against a cycle that would otherwise run its
  // passes with the drain bound switched off.
  if (drainDeadline == 0) drainDeadline = 1;

  // Hoisted out of the loop because the Receiving-state cleanup below reads the
  // LAST pass's answers: "is the thing currently named on the panel finished, and
  // did this pass name anything at all".
  bool bookTargetReported = false;
  bool wallpaperTargetReported = false;
  bool namedTargetFinished = false;

  for (uint8_t item = 0; item < MAX_ITEMS_PER_CYCLE; ++item) {
    bookTargetReported = false;
    wallpaperTargetReported = false;
    namedTargetFinished = false;

    // ONE BOOK PER PASS, by design: BookSync fetches at most one book per call.
    // The window gets the standard budget capped by what is left of the drain and
    // therefore of the session, so MIN_USEFUL_MS ("too little budget left to be
    // worth a window") fires when either is nearly spent.
    BookSync::Progress progress;
    progress.onTarget = [&](const std::string& filename, size_t bytes, size_t have) {
      bookTargetReported = true;
      announceTarget(filename, bytes, have);
    };
    progress.shouldAbort = [this] { return pollForAbort(); };

    const bool promoted =
        BookSync::syncOnLink(base, deadlineWithin(drainDeadline, BookSync::WINDOW_BUDGET_MS), progress);
    if (promoted) {
      booksReceived++;
      completedSomething = true;
      LOG_INF("MSYNCUI", "Book promoted (%d this session)", booksReceived);
    }
    if (bookTargetReported) namedTargetFinished = promoted;

    // Back was pressed inside the transfer, or the cap landed mid-pass. Either
    // way there is no second window to start.
    if (exiting || expired(sessionDeadline)) break;

    // ONE WALLPAPER PER PASS, after the books, on its own budget. Last of the
    // three phases because it is the least latency-sensitive: a note is a message
    // someone is waiting on, a book is a thing the user asked for, and a
    // wallpaper only has to be in place by the next time the reader sleeps.
    WallpaperSync::Progress wallpaperProgress;
    wallpaperProgress.onTarget = [&](const std::string& filename, const bool primary, size_t bytes, size_t have) {
      wallpaperTargetReported = true;
      // A primary carries no filename on the wire -- it is a single fixed slot -- so
      // the panel names it by what it is. WallpaperSync deliberately holds no I18n
      // table; this is the only place that decision costs anything.
      announceTarget(primary ? std::string(tr(STR_SYNC_WALLPAPER)) : filename, bytes, have);
    };
    wallpaperProgress.shouldAbort = [this] { return pollForAbort(); };

    const bool wallpaperApplied = WallpaperSync::syncOnLink(
        base, deadlineWithin(drainDeadline, WallpaperSync::WINDOW_BUDGET_MS), wallpaperProgress);
    if (wallpaperApplied) {
      wallpapersApplied++;
      completedSomething = true;
      LOG_INF("MSYNCUI", "Wallpaper applied (%d this session)", wallpapersApplied);
    }
    // WHICHEVER PHASE SPOKE LAST OWNS THE SCREEN, which is why this is not simply
    // `promoted || wallpaperApplied`. A pass that promotes a book and then starts
    // a wallpaper leaves the wallpaper's name on the panel; clearing on the
    // book's completion would blank a transfer that is still running.
    if (wallpaperTargetReported) namedTargetFinished = wallpaperApplied;

    if (!promoted && !wallpaperApplied) break;  // nothing landed: the mailbox is drained
    if (exiting || expired(drainDeadline) || expired(sessionDeadline)) break;
  }

  // A BOOK BIGGER THAN ONE WINDOW STAYS ON THE SCREEN BETWEEN WINDOWS, and that is
  // the whole reason this is not simply "back to waiting". Dropping to Polling
  // after every window would repaint twice per cadence for a book that is plainly
  // still arriving -- "Receiving X", "Waiting for mail", "Receiving X" -- which is
  // both a lie and, at 1720 ms a refresh, the strobing screen A4 forbids. Held in
  // Receiving instead, so the only signature change is `have` growing: exactly one
  // repaint per window, showing real progress.
  //
  // Left when the thing currently named on the panel finishes, or when a pass
  // goes by without EITHER phase naming a target at all -- which is how "there is
  // nothing left to fetch" arrives, and the only thing that stops a finished
  // transfer's name from sticking on the panel. Read off the LAST pass of the
  // drain loop, which is the one whose name is on the panel; `namedTargetFinished`
  // is maintained inside the loop for exactly that reason.
  if (state == State::Receiving && (namedTargetFinished || (!bookTargetReported && !wallpaperTargetReported))) {
    state = State::Polling;
    targetName.clear();
    targetBytes = 0;
    targetHave = 0;
  }

  return completedSomething ? CycleResult::Moved : CycleResult::Idle;
}

void MailboxSyncActivity::tryWifiHandoff() {
  // GIVE UP AFTER A FEW UNANSWERED PICKUPS, and do it before anything is spent.
  //
  // The overwhelmingly common session has nothing staged, so every pickup is a
  // 404 -- and a 404 is NOT silent the way this used to claim: HttpDownloader
  // logs every unexpected status at ERR unconditionally (see runGetWolf in
  // src/network/HttpDownloader.cpp). Left unlatched, a 30 minute session for a
  // user who never shares a network issues ~450 TCP connects and floods the
  // serial console with ~450 ERR lines, each one holding up to
  // WIFI_PICKUP_BUDGET_MS at the head of the latency-sensitive note pass.
  if (wifiPickupMisses >= WIFI_PICKUP_MAX_MISSES) return;

  const std::string origin = originOf(base);
  if (origin.empty()) return;
  const std::string url = origin + WIFI_SHARE_PATH;

  // Latched, because a refusal deliberately does NOT stop the retries: the whole
  // reason the reader does not ack a credential it cannot use is so a corrected
  // one is still there to pick up. Without the latch that costs a log line every
  // four seconds for the rest of the session.
  const auto refuse = [this](const char* why) {
    if (wifiRefusalLogged) return;
    wifiRefusalLogged = true;
    LOG_ERR("MSYNCUI", "Wi-Fi handover refused: %s", why);
  };

  std::string body;
  bool overflowed = false;
  // The bounded-body overload rather than the std::string one, for the same reason
  // PeerProbe uses it: the thing being asked is a station on an AP, and returning
  // false from the sink aborts the transfer instead of buffering whatever it feels
  // like sending.
  const bool ok = HttpDownloader::fetchUrl(
      url,
      [&body, &overflowed](const uint8_t* data, const size_t len) {
        if (body.size() + len > MAX_WIFI_BODY_BYTES) {
          overflowed = true;
          return false;
        }
        body.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      "", "", deadlineWithin(sessionDeadline, WIFI_PICKUP_BUDGET_MS));

  if (!ok) {
    // The overwhelmingly common answer here is a 404: the user has staged nothing,
    // or the app is older than this path. fetchUrl reports that the same way it
    // reports a refused connection, and it does not need to distinguish them --
    // both mean "nothing to do this cycle". This is the counter that stops the
    // cycle repeating for the rest of the session; see WIFI_PICKUP_MAX_MISSES.
    if (overflowed) refuse("the answer was too large to be a credential");
    if (++wifiPickupMisses >= WIFI_PICKUP_MAX_MISSES) {
      // ONE line at the transition, not one per poll. It is the record that the
      // pickup stopped being attempted, which is otherwise indistinguishable
      // from a build without the feature when somebody is reading the console.
      LOG_INF("MSYNCUI", "No Wi-Fi credential offered on this link; not asking again");
    }
    return;
  }
  // Answered. A later 404 on the same link starts the count over rather than
  // adding to a stale one -- an answer proves the endpoint is there.
  wifiPickupMisses = 0;

  std::string ssid;
  std::string password;
  if (!parseWifiPayload(body, ssid, password)) {
    refuse("the answer is not a credential this reader can use");
    return;
  }

  // LOAD BEFORE ADD, or the save inside addCredential() writes a file containing
  // ONLY this network. The store is a singleton that outlives activities but is
  // filled from the card lazily, and a device that went from boot straight into
  // "Sync with app" has never loaded it -- every network the user has ever saved
  // would be serialized away by a handover. MessageSync::connectSavedNetwork()
  // opens with the same call for the same reason.
  //
  // AND THE RESULT IS CHECKED, which is what makes "load before add" actually
  // protect anything. PersistableStore::loadFromFile returns false WITHOUT
  // touching `credentials` when readDocFromFile fails, and a truncated or
  // corrupt /.crosspoint/wifi.json is exactly that case: the in-memory list
  // stays empty, addCredential() saves, and every network the user had is gone.
  // A missing file is NOT that case -- it is first boot, an empty store is the
  // truth, and the handover is the right thing to write.
  //
  // Refusing here costs the user nothing: no ack goes back, so the credential
  // stays staged on the phone and a session with a readable card takes it.
  //
  // The `empty()` term keeps it from being a FALSE refusal: a store already
  // filled by an earlier successful load in this boot is correct in memory, and
  // saving it back is not destructive no matter what the card says now.
  const bool loaded = WIFI_STORE.loadFromFile();
  if (!loaded && WIFI_STORE.getCredentials().empty() &&
      Storage.exists(WifiCredentialStore::getFilePath())) {
    refuse("the saved networks could not be read");
    return;
  }
  // ADD-NEW-ONLY. addCredential() OVERWRITES the stored password of an SSID it
  // already knows, and the thing on the other end of this link has proved
  // nothing beyond answering /cp-proxy with the string "cp-proxy". A station
  // that returns the user's real home SSID with a wrong passphrase would
  // silently destroy the credential every unattended sync depends on, with no
  // user-visible symptom and no way to tell what happened. A handover is
  // therefore allowed to ADD a network and never to replace one.
  //
  // Re-offering a network the reader already holds is the normal case (the app
  // keeps offering until it sees an ack), so an identical credential is a
  // success, not a refusal -- it acks and stops the offers.
  if (const WifiCredential* existing = WIFI_STORE.findCredential(ssid)) {
    if (existing->password != password) {
      refuse("that network is already saved with a different password");
      return;
    }
  } else if (!WIFI_STORE.addCredential(ssid, password)) {
    // The eight-network store is full, or the card write failed. NO ACK: the
    // credential stays staged on the phone, and a session with room takes it.
    refuse("the credential could not be saved");
    return;
  }
  // DELIBERATELY NOT setLastConnectedSsid(): that field is the FIRST network
  // every unattended sleep-entry and wake check tries, for the rest of the
  // device's life, and this credential arrived from a station that authenticated
  // nothing. It is in the saved list either way, so the reader still finds it --
  // it just does not get to jump ahead of the network the user picked by hand on
  // the WiFi screen, which is the only thing that writes that field.
  // The SSID is in every beacon this reader can hear, so naming it costs nothing.
  // THE PASSWORD IS NEVER LOGGED, at any level, redacted or otherwise -- it is the
  // one secret this whole feature exists to move, and a log is a file on a card
  // somebody can read.
  LOG_INF("MSYNCUI", "Wi-Fi credential saved from the app: %s", ssid.c_str());

  // ACK, AND NEVER BEFORE THE SAVE. The app wipes its staging on this DELETE and
  // on nothing else, so acking a credential that is not on the card yet is exactly
  // how a handover is lost with nothing left to retry from. Best effort in the
  // other direction: the credential IS on the card now, so an ack that does not
  // land costs one duplicate handover next session and nothing the user sees.
  if (!HttpDownloader::deleteUrl(url, deadlineWithin(sessionDeadline, WIFI_PICKUP_BUDGET_MS))) {
    LOG_INF("MSYNCUI", "The app did not acknowledge the handover; it may offer the same network again");
  }

  wifiSavedSsid = ssid;
  // Painted here rather than left to the paint at the end of the poll cycle: the
  // note pass and the book window between this line and that one are up to forty
  // seconds of the panel saying nothing about the one thing the user is watching
  // for. Same argument as the book-target paint below, and it is still one frame.
  paintIfChanged();
}

bool MailboxSyncActivity::pollForAbort() {
  if (exiting) return true;

  static_assert(ABORT_POLL_MS > 0, "abort poll interval must be positive");
  const uint32_t now = millis();
  if (now - lastAbortPoll < ABORT_POLL_MS) return false;
  lastAbortPoll = now;

  resetTaskWatchdogIfSubscribed();
  // The main loop is blocked inside the transfer, so the input stack has to be
  // pumped by hand -- the same thing the transfer-mode activity does inside its
  // handleClient loop for the same reason.
  mappedInput.update();
  if (!mappedInput.wasPressed(MappedInputManager::Button::Back)) return false;

  LOG_DBG("MSYNCUI", "Back pressed during a transfer: abandoning the window");
  exiting = true;
  return true;
}

// --- Teardown --------------------------------------------------------------

void MailboxSyncActivity::teardownRadio() {
  if (!radioTouched) return;
  radioTouched = false;

  if (transport == Transport::PhoneAp) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
  } else {
    // The one WiFi teardown recipe for this module's STA paths, so this activity
    // cannot leave the modem powered in a way the sleep-entry path would not.
    MessageSync::radioOff();
  }
  LOG_DBG("MSYNCUI", "Radio down");
}

// --- Paint -----------------------------------------------------------------

MailboxSyncActivity::PaintSignature MailboxSyncActivity::signature() const {
  return PaintSignature{state,         hashName(targetName), targetHave,  notesStaged,
                        booksReceived, wallpapersApplied,    failureText, hashName(wifiSavedSsid)};
}

void MailboxSyncActivity::paintIfChanged() {
  if (exiting) return;
  const PaintSignature now = signature();
  if (now == painted) return;
  painted = now;
  // Blocking, so the panel really is showing this state before the caller goes off
  // and blocks on a socket for up to a window.
  requestUpdateAndWait();
}

void MailboxSyncActivity::fail(const StrId reason) {
  state = State::Failed;
  failureText = reason;
  LOG_ERR("MSYNCUI", "Mailbox sync failed: %s", I18N.get(reason));
  // Same argument as the session cap: Failed is terminal and waits on the user, so
  // the radio comes down now rather than whenever they get round to pressing Back.
  teardownRadio();
}

// --- Render ----------------------------------------------------------------

const char* MailboxSyncActivity::statusLine() const {
  switch (state) {
    case State::Connecting:
      return transport == Transport::SavedNetwork ? tr(STR_CONNECTING_SAVED_WIFI) : tr(STR_STARTING_HOTSPOT);
    case State::WaitingPhone:
      return tr(STR_SYNC_WAITING_PHONE);
    case State::Linking:
      return tr(STR_SYNC_LOOKING_FOR_APP);
    case State::Polling:
      return tr(STR_SYNC_WAITING_MAIL);
    case State::Receiving:
      return tr(STR_SYNC_RECEIVING);
    case State::Stalled:
      return tr(STR_SYNC_STALLED);
    case State::Failed:
      return I18N.get(failureText);
    case State::Finished:
      return failureText == StrId::STR_SYNC_TIME_LIMIT ? tr(STR_SYNC_TIME_LIMIT) : tr(STR_SYNC_FINISHED);
  }
  return "";
}

void MailboxSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 transport == Transport::SavedNetwork ? tr(STR_MAILBOX_SYNC) : tr(STR_SYNC_WITH_APP));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  renderBody(contentTop);

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void MailboxSyncActivity::renderBody(const int contentTop) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = contentTop + metrics.verticalSpacing;

  renderer.drawCenteredText(UI_10_FONT_ID, y, statusLine(), true, EpdFontFamily::BOLD);
  y += lineHeight + metrics.verticalSpacing;

  // The book or wallpaper being received, named. Nothing equivalent exists for a
  // note on purpose: a note has no user-visible name, and the only honest thing to
  // say about one is when it will appear.
  if (state == State::Receiving && !targetName.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, targetName.c_str());
    y += lineHeight;
    const std::string sizeLine = humanBytes(targetHave) + " / " + humanBytes(targetBytes);
    renderer.drawCenteredText(SMALL_FONT_ID, y, sizeLine.c_str());
    y += lineHeight;
  }

  if (state == State::WaitingPhone) {
    renderPhoneJoinPanel(y);
    return;
  }

  // Counts. The only place in the whole system where the user gets confirmation of
  // anything -- there are no acks by design -- so it is worth the two lines.
  y += metrics.verticalSpacing;
  char counts[96];
  snprintf(counts, sizeof(counts), "%s: %d    %s: %d", tr(STR_SYNC_NOTES_LABEL), notesStaged,
           tr(STR_SYNC_BOOKS_LABEL), booksReceived);
  renderer.drawCenteredText(UI_10_FONT_ID, y, counts);
  y += lineHeight;

  // Wallpapers get their own line rather than a third column, and only once one
  // has landed. The counts line is centred and already carries two labels with
  // their numbers; widening it to three would reflow the layout of every session,
  // including the overwhelming majority that never receive a wallpaper at all.
  if (wallpapersApplied > 0) {
    char wallpaperCount[64];
    snprintf(wallpaperCount, sizeof(wallpaperCount), "%s: %d", tr(STR_SYNC_WALLPAPERS_LABEL), wallpapersApplied);
    renderer.drawCenteredText(UI_10_FONT_ID, y, wallpaperCount);
    y += lineHeight;
  }

  // The network the phone handed over, named, because it is the one thing in this
  // whole mode the user cannot verify anywhere else until they next need it. Shown
  // only after the credential is actually on the card -- the line is a receipt, not
  // a promise.
  if (!wifiSavedSsid.empty()) {
    y += metrics.verticalSpacing;
    char wifiLine[128];
    snprintf(wifiLine, sizeof(wifiLine), "%s %s", tr(STR_SYNC_WIFI_SAVED), wifiSavedSsid.c_str());
    renderer.drawCenteredText(SMALL_FONT_ID, y, wifiLine);
    y += lineHeight;
  }

  // Why the note the user just saw arrive is not on the screen. Shown only once a
  // note has actually been staged, so it reads as an explanation rather than as an
  // instruction nobody asked for.
  if (notesStaged > 0) {
    y += metrics.verticalSpacing;
    renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_SYNC_NOTE_PENDING));
  }
}

void MailboxSyncActivity::renderPhoneJoinPanel(const int top) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto pageWidth = renderer.getScreenWidth();
  int y = top + metrics.verticalSpacing;

  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_SYNC_JOIN_PROMPT), true, EpdFontFamily::BOLD);
  y += lineHeight + metrics.verticalSpacing;

  // WPA join QR, per the zxing WIFI: convention the transfer-mode screen already
  // uses -- the phone's camera fills in both fields, so the passphrase below is the
  // fallback for when it does not, not the primary path.
  //
  // Both fields are ESCAPED. The minted passphrase never needs it (its alphabet
  // is alphanumerics plus '#'), but the passphrase is user-settable from two
  // editors now, and a ';' or a '\' typed into either one would otherwise end the
  // field early: the phone would join with a silently truncated passphrase and
  // fail, with the panel showing the correct one.
  const std::string wifiConfig = std::string("WIFI:T:WPA;S:") + escapeQrField(apSsid) + ";P:" + escapeQrField(apPsk) +
                                 ";;";
  const Rect qrBounds((pageWidth - QR_SIZE) / 2, y, QR_SIZE, QR_SIZE);
  QrUtils::drawQrCode(renderer, qrBounds, wifiConfig);
  y += QR_SIZE + metrics.verticalSpacing;

  renderer.drawCenteredText(UI_10_FONT_ID, y, apSsid.c_str(), true, EpdFontFamily::BOLD);
  y += lineHeight;

  // Sized for what this field is allowed to hold, not for the minted 10 chars:
  // MAILBOX_AP_PSK_MAX_LEN is 63, and the label is a translated string. A
  // truncated line here would show the user a passphrase that cannot work.
  std::string pskLine = tr(STR_SYNC_PASSWORD_LABEL);
  pskLine += ": ";
  pskLine += apPsk;
  renderer.drawCenteredText(UI_10_FONT_ID, y, pskLine.c_str());
}
