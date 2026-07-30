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

// Suffixes appended to the mailbox base the caller passes in (trailing '/'
// stripped) -- the configured one over the internet, a peer-proxy origin plus the
// configured path over an AP link; the contract is identical either way. Server
// contract: GET base + "/latest.txt" -> tiny plain-text body = latest message id
// (empty body means "no note"); GET base + "/current.frame" -> raw framebuffer
// bytes (exactly the panel buffer size).
constexpr char SUFFIX_ID[] = "/latest.txt";
constexpr char SUFFIX_FRAME[] = "/current.frame";

constexpr uint32_t STATUS_POLL_MS = 100;
constexpr size_t MAX_ID_LEN = 128;

// Absolute deadline `budget` ms from now, never past `cap` (an absolute deadline,
// 0 = uncapped).
uint32_t deadlineWithin(uint32_t cap, uint32_t budget) {
  const uint32_t want = millis() + budget;
  if (cap == 0) return want;
  return static_cast<int32_t>(want - cap) > 0 ? cap : want;
}

std::string trimId(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
  if (start > 0) s.erase(0, start);
  if (s.size() > MAX_ID_LEN) s.resize(MAX_ID_LEN);
  return s;
}

bool stagedFramePresent() {
  HalFile file;
  if (!Storage.openFileForRead("MSYNC", CURRENT_FRAME, file)) return false;
  const bool present = file.size() > 0;
  file.close();
  return present;
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

// --- The three staging phases, shared verbatim by both arrival paths --------
// Every note download, on either path, goes through incoming -> validate ->
// promote. That is not boilerplate here: the bytes only ever land in
// INCOMING_FRAME, so a check abandoned at any byte leaves current.frame -- the
// thing the next sleep-entry is going to blit -- bit-for-bit intact.

enum class Probe : uint8_t { Failed, Empty, UpToDate, NewNote };

// Cheap dedup probe: one tiny GET of the mailbox's latest id. Requires WiFi.
// `deadline` is an absolute millis() bound on the call (0 = none): this is a
// BLOCKING call on whichever task runs it, so the deadline is the only thing that
// keeps a mailbox that associates-but-does-not-answer from holding that task.
Probe probeLatest(const std::string& base, std::string& latestIdOut, uint32_t deadline) {
  std::string latestId;
  if (!HttpDownloader::fetchUrl(base + SUFFIX_ID, latestId, "", "", deadline)) {
    LOG_DBG("MSYNC", "latest-id fetch failed");
    return Probe::Failed;
  }
  latestId = trimId(std::move(latestId));
  if (latestId.empty()) {
    LOG_DBG("MSYNC", "mailbox empty");
    return Probe::Empty;
  }
  // The staged id is the ONLY download dedup, and it answers exactly one
  // question: "do we already hold these bytes". It is deliberately NOT
  // APP_STATE.messageLastDisplayedId: that field decides whose turn it is on the
  // panel (M2 #4), and wiring it in here would resurrect the round-1 defect where
  // a note the user had already seen could never be re-fetched -- and, worse,
  // where losing state.json would re-download every note ever staged.
  if (latestId == MessageSync::stagedNoteId()) {
    LOG_DBG("MSYNC", "No new note (latest=%s)", latestId.c_str());
    return Probe::UpToDate;
  }
  latestIdOut = std::move(latestId);
  return Probe::NewNote;
}

// Download the frame to INCOMING_FRAME. Requires WiFi. `deadline` is an absolute
// millis() bound (0 = none) -- see probeLatest: blocking call, and the file is
// removed by the caller on any non-OK result, so a deadline hit mid-body simply
// re-fetches next window rather than leaving a torn frame staged.
HttpDownloader::DownloadError downloadIncoming(const std::string& base, uint32_t deadline) {
  // openFileForWrite uses O_CREAT only (no parent-dir creation); a fresh device
  // that never received an M1 web upload has no /.love-notes yet, so ensure it.
  Storage.ensureDirectoryExists(NOTES_DIR);
  return HttpDownloader::downloadToFile(base + SUFFIX_FRAME, INCOMING_FRAME, nullptr, nullptr, "", "", deadline);
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
  if (!Storage.writeFile(CURRENT_ID, String(latestId.c_str()))) {
    // The rename already landed, so current.frame IS the new note while
    // current.id still names the PREVIOUS one -- and that pairing is silently
    // fatal under display-once: noteAwaitingDisplay() would compare the stale id
    // against messageLastDisplayedId, find them equal, and skip the note at BOTH
    // paint sites, while the mailbox re-fetched its 52 KB every window
    // (latestId != stagedNoteId()). Drop the sidecar instead: an id-less frame
    // reads as unseen, so the note still gets its turn and mints a key when it
    // is painted. The frame is genuinely staged either way -- still true.
    LOG_ERR("MSYNC", "id sidecar write failed for %s; dropping stale id", latestId.c_str());
    Storage.remove(CURRENT_ID);
  }
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
// rather than a race, and it is why the transfers take a deadline rather than a
// cancel flag: there is nobody to poll the flag while they run. The deadline is
// what bounds them, and the phase budgets below are therefore also the worst-case
// input latency at the launcher -- see WAKE_PROBE_BUDGET_MS.
enum class WakeStep : uint8_t { Idle, Settle, Connect, Probe, Download };

struct WakeCheck {
  WakeStep step = WakeStep::Idle;
  size_t frameSize = 0;
  // millis() deadline for the CURRENT phase, re-armed on each transition. Per
  // phase rather than per check because the two HTTP steps are single blocking
  // calls: a whole-check deadline that has nearly expired by the time Download
  // starts would either kill the transfer before it began or, if it were only
  // tested between steps, bound nothing at all.
  uint32_t phaseDeadline = 0;
  uint32_t settleUntil = 0;  // mirrors connectHeadless's post-disconnect settle
  std::string base;
  std::string latestId;
  std::string lastSsid;  // by name, not by pointer: the store can be reloaded
  size_t nextIdx = 0;
  bool triedLast = false;
  bool attemptInFlight = false;
};
WakeCheck wake;

// Connect budget: section 3A prices the window at "connect <= 6 s + one TLS
// handshake, 8 s worst case", and requires the caller to enforce it against
// millis() rather than leaning on the 60 s per-socket-op timeout. Polled, so it
// never blocks input.
constexpr uint32_t WAKE_CONNECT_BUDGET_MS = 8000;

// Budgets for the two BLOCKING steps, and therefore the worst-case input freeze
// at the launcher: the machine is stepped from the task that dispatches input, so
// while one of these calls is in flight nothing is dispatched. They are the reason
// the numbers are this small -- 3A rejected B1 because "a blocking connectHeadless
// on the main task freezes input for up to 6 s -- unacceptable at the launcher",
// and an unbounded HTTP call is the same defect with a worse constant.
//
// latest.txt is <= 128 B by contract, so its cost is one handshake plus a round
// trip. The frame is 52272 B on the X3 (48000 on the X4); at the contract's
// 30 KB/s pessimistic floor that is ~1.8 s, and 5 s covers a handshake on top.
// A deadline hit mid-frame costs nothing but the radio time: the bytes went to
// INCOMING_FRAME, so the note is simply re-fetched on the next wake or staged by
// the sleep-entry sync.
constexpr uint32_t WAKE_PROBE_BUDGET_MS = 3000;
constexpr uint32_t WAKE_FRAME_BUDGET_MS = 5000;

void wakeFinish(const char* why) {
  if (wake.step == WakeStep::Idle) return;
  MessageSync::radioOff();
  wake = WakeCheck{};  // back to Idle, and releases the held strings
  // Paired with the "Wake check armed" line: armed-then-finished(why) is the
  // whole Path B story in two lines, and INF keeps it in release builds.
  LOG_INF("MSYNC", "Wake check finished (%s)", why);
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

std::string MessageSync::configuredBase() {
  std::string base = SETTINGS.messageSyncUrl;
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base;
}

void MessageSync::radioOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool MessageSync::connectSavedNetwork(const uint32_t budgetMs) {
  WIFI_STORE.loadFromFile();
  const auto& creds = WIFI_STORE.getCredentials();
  if (creds.empty()) {
    // INF, not DBG: this and the budget-exhausted line below are the two answers
    // to "why did the unattended sync do nothing", and gh_release/gh_release_rc
    // build at LOG_LEVEL=1 -- DBG is compiled out of exactly the builds most
    // likely to be in a user's hand when it fails.
    LOG_INF("MSYNC", "No saved WiFi credentials");
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

  const uint32_t deadline = millis() + budgetMs;
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
  LOG_INF("MSYNC", "No saved network reachable within budget");
  return false;
}

MessageSync::NoteResult MessageSync::syncOnLink(const std::string& base, size_t frameBufferSize, uint32_t deadline) {
  if (base.empty()) return NoteResult::Failed;

  std::string latestId;
  switch (probeLatest(base, latestId, deadline)) {
    case Probe::Failed:
      return NoteResult::Failed;
    case Probe::Empty:
      return NoteResult::NoNote;
    case Probe::UpToDate:
      return NoteResult::UpToDate;
    case Probe::NewNote:
      break;
  }

  LOG_INF("MSYNC", "New note %s: downloading frame", latestId.c_str());
  const HttpDownloader::DownloadError err = downloadIncoming(base, deadline);
  if (err != HttpDownloader::OK) {
    LOG_ERR("MSYNC", "frame download failed (%d)", static_cast<int>(err));
    Storage.remove(INCOMING_FRAME);
    return NoteResult::Failed;
  }
  // Promoted while the radio is still up. That is pure SD work either way, and
  // doing it here rather than after a teardown is what lets a caller's link-up
  // hook see the final "a new note is staged" answer.
  return promoteIncoming(latestId, frameBufferSize) ? NoteResult::Staged : NoteResult::Failed;
}

namespace {
const char* noteResultName(const MessageSync::NoteResult r) {
  switch (r) {
    case MessageSync::NoteResult::Failed:
      return "fetch failed";
    case MessageSync::NoteResult::NoNote:
      return "mailbox empty";
    case MessageSync::NoteResult::UpToDate:
      return "already staged";
    case MessageSync::NoteResult::Staged:
      return "staged a new note";
  }
  return "unknown";
}
}  // namespace

// Every exit below names its reason at INF. This path is unattended and has NO
// UI whatsoever: when it declines to run, the device is byte-for-byte
// indistinguishable from a healthy one -- nothing paints, nothing errors, and
// (the part that actually costs the time) NO request reaches the mailbox, so the
// server logs cannot tell "switched off" from "never woke" from "no WiFi". These
// gates used to be silent returns; one flipped bool then reads as a dead radio.
//
// Three reasons this is LOG_INF and not LOG_DBG:
//   - gh_release / gh_release_rc build at LOG_LEVEL=1, so DBG is compiled OUT of
//     precisely the builds a user is holding when this fails.
//   - the log ring is RTC_NOINIT (Logging.cpp), so a line written here SURVIVES
//     the deep sleep that follows and is still readable at the next wake, both
//     over serial and through getLastLogs() in the web UI. That is the only
//     forensic trail this path can leave.
//   - it is at most one line per sleep.
bool MessageSync::syncBeforeSleep(size_t frameBufferSize, uint32_t deadline, const LinkUpHook& whileLinkUp) {
  if (!SETTINGS.messageSyncEnabled) {
    LOG_INF("MSYNC", "Sleep sync OFF: 'Message sync' is disabled in Settings > System");
    return false;
  }
  const std::string base = configuredBase();
  if (base.empty()) {
    LOG_INF("MSYNC", "Sleep sync OFF: no mailbox URL configured");
    return false;
  }

  if (!connectSavedNetwork()) {
    LOG_INF("MSYNC", "Sleep sync skipped: no saved WiFi network joined");
    radioOff();
    return false;
  }

  // The note half runs to completion first, unconditionally: it is tiny and
  // latency-sensitive, and it must never queue behind a book. Both of its HTTP
  // calls are bounded from HERE -- i.e. from the moment the link came up, so the
  // connect's own <= 6 s budget is not double-charged -- and never past the
  // caller's deadline.
  const uint32_t noteDeadline = deadlineWithin(deadline, NOTE_PASS_BUDGET_MS);
  const NoteResult note = syncOnLink(base, frameBufferSize, noteDeadline);
  const bool staged = note == NoteResult::Staged;
  // The positive breadcrumb, and the one that matters most: it proves the window
  // ran end to end -- radio up, mailbox answered -- which is what separates "the
  // reader never asked" from "the reader asked and got nothing".
  LOG_INF("MSYNC", "Sleep sync note pass: %s", noteResultName(note));

  // Everything else that needs this window happens here, on the link the connect
  // above already paid for. Bounded by the hook itself -- see BookSync::syncOnLink.
  if (whileLinkUp) whileLinkUp(base, staged);

  radioOff();  // no longer needed, regardless of either half's outcome
  return staged;
}

// Same three gates as syncBeforeSleep, same silence problem, same treatment:
// this is the OTHER unattended entry point, and when both are off the device
// stops talking to the mailbox entirely with nothing anywhere to say why.
void MessageSync::beginWakeCheck(size_t frameBufferSize) {
  if (wake.step != WakeStep::Idle) return;
  if (!SETTINGS.messageSyncEnabled) {
    LOG_INF("MSYNC", "Wake check OFF: 'Message sync' is disabled in Settings > System");
    return;
  }
  std::string base = configuredBase();
  if (base.empty()) {
    LOG_INF("MSYNC", "Wake check OFF: no mailbox URL configured");
    return;
  }

  WIFI_STORE.loadFromFile();
  if (WIFI_STORE.getCredentials().empty()) {
    LOG_INF("MSYNC", "Wake check skipped: no saved WiFi credentials");
    return;
  }

  wake = WakeCheck{};
  wake.step = WakeStep::Settle;
  wake.frameSize = frameBufferSize;
  wake.base = std::move(base);
  wake.lastSsid = WIFI_STORE.getLastConnectedSsid();
  wake.phaseDeadline = millis() + WAKE_CONNECT_BUDGET_MS;
  wake.settleUntil = millis() + 100;
  wifiBeginSta();
  LOG_INF("MSYNC", "Wake check armed");
}

void MessageSync::stepWakeCheck() {
  if (wake.step == WakeStep::Idle) return;
  if (static_cast<int32_t>(millis() - wake.phaseDeadline) >= 0) {
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
        wake.phaseDeadline = millis() + WAKE_PROBE_BUDGET_MS;
      }
      return;
    }

    case WakeStep::Probe: {
      // One tiny GET plus the single TLS handshake. Blocking, so bounded by the
      // deadline it is handed -- nothing polls input while it runs.
      std::string latestId;
      if (probeLatest(wake.base, latestId, wake.phaseDeadline) != Probe::NewNote) {
        wakeFinish("nothing new");
        return;
      }
      wake.latestId = std::move(latestId);
      wake.step = WakeStep::Download;
      wake.phaseDeadline = millis() + WAKE_FRAME_BUDGET_MS;
      return;
    }

    case WakeStep::Download: {
      LOG_INF("MSYNC", "Wake check: new note %s, downloading frame", wake.latestId.c_str());
      const HttpDownloader::DownloadError err = downloadIncoming(wake.base, wake.phaseDeadline);
      // Radio off before the SD promote and before control returns to the
      // launcher: WiFi and a chapter build must never be resident at once.
      MessageSync::radioOff();
      if (err != HttpDownloader::OK) {
        LOG_ERR("MSYNC", "wake frame download failed (%d)", static_cast<int>(err));
        Storage.remove(INCOMING_FRAME);
      } else {
        promoteIncoming(wake.latestId, wake.frameSize);
      }
      // The one number that decides whether Path B is safe to arm when a book is
      // one keypress away: WiFi.mode(WIFI_OFF) does not defragment the heap and
      // silentRestart() is not available at the launcher, so if a post-check
      // chapter build OOMs there is no recovery. Logged on the completed path
      // (the expensive one -- a TLS session plus a 52 KB SD write) so the arm
      // condition in main.cpp can be decided on a measurement instead of a guess.
      LOG_INF("MSYNC", "Post-check heap: free %u, min free %u, max alloc %u", (unsigned)ESP.getFreeHeap(),
              (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
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

std::string MessageSync::stagedNoteId() {
  char buf[MAX_ID_LEN + 1] = {};
  const size_t n = Storage.readFileToBuffer(CURRENT_ID, buf, sizeof(buf));
  if (n == 0) return std::string();
  return trimId(std::string(buf));
}

bool MessageSync::noteAwaitingDisplay() {
  if (!stagedFramePresent()) return false;
  const std::string id = stagedNoteId();
  // No sidecar: an UNSEEN frame by construction, so it gets a turn. Every writer
  // clears current.id before staging a frame -- the app's direct send deletes it
  // as step 1 of every send, and promoteIncoming() drops it rather than leave a
  // stale one -- so an empty sidecar can only mean "these bytes have not been
  // keyed yet", never "these bytes were already painted". The turn is not
  // open-ended: markStagedNoteDisplayed() mints the key at paint time.
  if (id.empty()) return true;
  return id != APP_STATE.messageLastDisplayedId;
}

void MessageSync::markStagedNoteDisplayed() {
  std::string id = stagedNoteId();
  if (id.empty()) {
    // An id-less frame is a real, deliverable note: the app uploads the 52 KB
    // frame and only then the sidecar, and a sidecar upload that fails after a
    // successful frame is a documented, non-fatal outcome on a flaky WS link
    // (SendLoveNoteFrameResult.idStaged:false / idError). Exempting it from
    // display-once would hand it the panel on EVERY sleep forever with no way
    // for the user to clear it -- exactly the behaviour this milestone removes.
    // So mint a local key HERE, at the paint, and stage it as the sidecar.
    //
    // Safe on both consumers: a genuinely new id-less frame still earns a fresh
    // turn, because every writer clears current.id before it stages a frame; and
    // download dedup is untouched, because a mailbox latestId can never equal
    // this marker (the frame is re-fetchable exactly as it was without a sidecar).
    char minted[32];
    snprintf(minted, sizeof(minted), "local-%lu", static_cast<unsigned long>(millis()));
    if (!Storage.writeFile(CURRENT_ID, String(minted))) {
      // SD write failed: leave the frame unkeyed rather than record an id that
      // is not on the card. It keeps its turn and is re-keyed at the next paint.
      LOG_ERR("MSYNC", "could not mint a local id for the staged note");
      return;
    }
    id = minted;
    LOG_DBG("MSYNC", "Staged note had no id sidecar; minted %s", minted);
  }
  if (APP_STATE.messageLastDisplayedId == id) return;
  APP_STATE.messageLastDisplayedId = id;
  // One SD write, and only on the sleeps where a note actually reached the
  // panel -- not once per sleep and never on the wake path. Persisted here
  // rather than at the next state save because deep sleep is the next thing that
  // happens: an unsaved id would hand the same note a second turn after the
  // wake, which is the exact behaviour this milestone removes.
  APP_STATE.saveToFile();
  LOG_DBG("MSYNC", "Note %s displayed; wallpaper resumes next sleep", id.c_str());
}

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
