#include "MailboxSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_random.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/BookSync.h"
#include "network/MessageSync.h"
#include "network/PeerProbe.h"
#include "util/QrUtils.h"
#include "util/TaskWatchdog.h"

using MailboxSync::PEER_DISCOVERY_MS;
using MailboxSync::PHONE_JOIN_WAIT_MS;
using MailboxSync::POLL_INTERVAL_MS;
using MailboxSync::SESSION_CAP_MS;
using MailboxSync::Transport;

namespace {
// The SSID the app's WifiNetworkSpecifier matches on, deliberately the same
// string the shipped transfer-mode AP uses (CrossPointWebServerActivity.cpp:23):
// the phone side has one network name to know about, not two.
constexpr const char* AP_SSID = "CrossPoint-Reader";
constexpr uint8_t AP_CHANNEL = 1;
// The whole candidate space PeerProbe sweeps is derived from this: four leases.
constexpr uint8_t AP_MAX_CONNECTIONS = 4;

// PER-DEVICE, PER-SESSION AP PASSPHRASE -- appendix A3's second required fix.
//
// The shipped transfer-mode AP is open (AP_PASSWORD is a compile-time nullptr),
// and A3 is explicit that an open AP is not good enough here even with the
// boxId-free health probe in front of the capability URL: a station that answers
// /cp-proxy can still impersonate the forwarder and serve the reader a frame or an
// epub of its choosing, bounded only by the staging size gates against a manifest
// from that same station. A PSK is what stops an uninvited station occupying a
// lease at all.
//
// It is generated fresh per session rather than provisioned once, and that is the
// stronger choice available to a FOREGROUND mode: the user is looking at the panel
// and joins with what is printed on it, so there is nothing to store, nothing to
// leak from storage, and nothing an attacker can precompute. A passphrase derived
// from the MAC would be strictly worse than useless -- the AP's BSSID is in every
// beacon, so anyone in range could compute it.
//
// Generated AFTER WiFi.mode(WIFI_AP) has powered the radio: esp_fill_random is a
// true hardware RNG only while WiFi or BT is enabled, and a PSK from the
// pseudo-random fallback would defeat the point.
constexpr size_t PSK_LEN = 12;  // 12 chars over a 32-symbol alphabet = 60 bits.
// Unambiguous alphabet: no 0/O, no 1/I/L, so a passphrase read off e-ink and typed
// into a phone cannot fail on a glyph. 32 symbols exactly, so the rejection-free
// mask below is uniform.
constexpr char PSK_ALPHABET[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ#";
static_assert(sizeof(PSK_ALPHABET) - 1 == 32, "PSK alphabet must be exactly 32 symbols for a uniform 5-bit draw");

std::string generatePsk() {
  uint8_t raw[PSK_LEN];
  esp_fill_random(raw, sizeof(raw));
  std::string psk;
  psk.reserve(PSK_LEN);
  for (const uint8_t b : raw) psk.push_back(PSK_ALPHABET[b & 0x1F]);
  return psk;
}

constexpr int QR_SIZE = 180;

// Consecutive failed note polls before the screen admits it is not getting
// through. Three at a 4 s cadence is ~12 s, long enough that a single dropped
// packet or a hotspot re-associating does not flash a scary line at the user.
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
  runPollCycle();
  nextPollAt = millis() + POLL_INTERVAL_MS;
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
  //   4. a real PSK         -- see generatePsk().
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
  apPsk = generatePsk();  // after mode(): the RNG needs the radio powered

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
    // A STATION IS ASSOCIATED BUT UNPROVEN, AND THE RETRY IS ON THE POLL CADENCE.
    // Re-discovery used to run on every loop() iteration, so a station that
    // associates without answering /cp-proxy -- a phone that scanned the QR before
    // opening the app, an app still binding its listener, a backgrounded app, or a
    // stranger's device holding a lease -- drove WaitingPhone -> Linking -> failed
    // sweep -> WaitingPhone with no gate at all. phoneWaitDeadline cannot bound that
    // because it is only consulted while no station is present. Two full-frame
    // repaints of two completely different layouts per attempt, forever: A4's
    // strobing-panel finding, exactly.
    if (static_cast<int32_t>(millis() - nextPollAt) < 0) return;

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
    const std::string peer =
        PeerProbe::discoverBase(configuredUrl, deadlineWithin(sessionDeadline, PEER_DISCOVERY_MS));
    if (peer.empty()) {
      // The station is associated but is not answering as a forwarder -- the app
      // is not open yet, or it is a device that just joined the network. Fall back
      // to waiting rather than failing: the phone is still there and the next
      // attempt will try again -- ON THE POLL CADENCE, which is what this line
      // arms. Without it the WaitingPhone gate above lets the sweep re-run
      // immediately and the panel strobes.
      nextPollAt = millis() + POLL_INTERVAL_MS;
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

void MailboxSyncActivity::runPollCycle() {
  if (base.empty()) return;

  resetTaskWatchdogIfSubscribed();

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
    return;
  }
  consecutiveFailures = 0;
  if (state == State::Stalled) state = State::Polling;

  if (note == MessageSync::NoteResult::Staged) {
    // Staged, NOT shown. The lock-screen model is unconditional: this frame becomes
    // the sleep screen at the next sleep-entry. The screen reports it; it does not
    // render it, and it must not -- MessageDisplayActivity paints a FULL_REFRESH
    // multi-flash waveform and would reintroduce the interrupting note.
    notesStaged++;
    LOG_INF("MSYNCUI", "Note staged (%d this session)", notesStaged);
  }

  if (expired(sessionDeadline)) return;

  // ONE BOOK PER POLL, by design: BookSync fetches at most one book per call, so a
  // mailbox holding five books drains over five poll cycles rather than one sweep.
  // The window gets the standard budget capped by what is left of the session, so
  // MIN_USEFUL_MS ("too little budget left to be worth a window") effectively only
  // fires when the cap is nearly spent.
  bool targetReported = false;
  BookSync::Progress progress;
  progress.onTarget = [this, &targetReported](const std::string& filename, size_t bytes, size_t have) {
    targetReported = true;
    targetName = filename;
    targetBytes = bytes;
    targetHave = have;
    state = State::Receiving;
    // Paint here, before the transfer, so the panel names the book while the bytes
    // are moving instead of after. It costs one refresh out of the window budget,
    // and only when something on the screen actually changed.
    paintIfChanged();
  };
  progress.shouldAbort = [this] { return pollForAbort(); };

  const bool promoted =
      BookSync::syncOnLink(base, deadlineWithin(sessionDeadline, BookSync::WINDOW_BUDGET_MS), progress);
  if (promoted) {
    booksReceived++;
    LOG_INF("MSYNCUI", "Book promoted (%d this session)", booksReceived);
  }

  // A BOOK BIGGER THAN ONE WINDOW STAYS ON THE SCREEN BETWEEN WINDOWS, and that is
  // the whole reason this is not simply "back to waiting". Dropping to Polling
  // after every window would repaint twice per cadence for a book that is plainly
  // still arriving -- "Receiving X", "Waiting for mail", "Receiving X" -- which is
  // both a lie and, at 1720 ms a refresh, the strobing screen A4 forbids. Held in
  // Receiving instead, so the only signature change is `have` growing: exactly one
  // repaint per window, showing real progress.
  //
  // Left when the book is promoted, or when a cycle passes without BookSync naming
  // a target at all -- which is how "there is nothing left to fetch" arrives, and
  // the only thing that stops a finished book's name from sticking on the panel.
  if (state == State::Receiving && (promoted || !targetReported)) {
    state = State::Polling;
    targetName.clear();
    targetBytes = 0;
    targetHave = 0;
  }
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
  return PaintSignature{state, hashName(targetName), targetHave, notesStaged, booksReceived, failureText};
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

  // The book being received, named. Nothing equivalent exists for a note on
  // purpose: a note has no user-visible name, and the only honest thing to say
  // about one is when it will appear.
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
  const std::string wifiConfig = std::string("WIFI:T:WPA;S:") + apSsid + ";P:" + apPsk + ";;";
  const Rect qrBounds((pageWidth - QR_SIZE) / 2, y, QR_SIZE, QR_SIZE);
  QrUtils::drawQrCode(renderer, qrBounds, wifiConfig);
  y += QR_SIZE + metrics.verticalSpacing;

  renderer.drawCenteredText(UI_10_FONT_ID, y, apSsid.c_str(), true, EpdFontFamily::BOLD);
  y += lineHeight;

  char pskLine[64];
  snprintf(pskLine, sizeof(pskLine), "%s: %s", tr(STR_SYNC_PASSWORD_LABEL), apPsk.c_str());
  renderer.drawCenteredText(UI_10_FONT_ID, y, pskLine);
}
