#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// M2 #2 (messenger): note delivery -- the lock-screen model.
// See docs/xteink/mailbox-books-contract.md section 3A.
//
// THE NEWEST MAILBOX NOTE *IS* THE SLEEP/LOCK SCREEN. A note never renders live
// and never interrupts: no banner, no toast, no press-to-view, no "unread"
// state to dismiss. The user sets the book down, the panel paints, and the note
// is what is on the panel. Picking the device back up is a normal wake into
// whatever they were reading.
//
// Two arrival paths, both terminating at the same place -- the frame staged at
// /.love-notes/current.frame, which SleepActivity blits as the sleep image:
//
//   Path A  published while awake  -> the sleep-entry sync stages it and the
//           same sleep-entry repaints the panel with it (A2 ordering; see
//           enterDeepSleep() in main.cpp). Zero latency.
//   Path B  published while asleep -> a silent, zero-UI check on the next
//           *launcher* wake stages it; it becomes the lock at the next
//           sleep-entry. One wake->sleep cycle of lag, deliberately accepted as
//           the price of zero interruption.
//
// Reversion rule: the latest note stays the lock until a newer note replaces it.
// No timer, no read-tracking, no revert-to-wallpaper -- wallpaper is the
// no-note-exists fallback only.
//
// The two UNATTENDED entry points -- syncBeforeSleep and beginWakeCheck -- are
// gated on SETTINGS.messageSyncEnabled plus a non-empty SETTINGS.messageSyncUrl;
// there are no other user-facing settings for them. The reusable primitives below
// (connectSavedNetwork / syncOnLink / radioOff) are deliberately UNGATED: they run
// on a link and a base their caller chose, so the policy decision -- "may this
// radio come up right now, and against which origin" -- belongs to that caller. A
// user-initiated sync mode is its own policy and must not be forced through the
// unattended notes switch.
namespace MessageSync {

// --- Path A: sleep-entry sync (blocking, bounded) --------------------------
// Connects to a saved WiFi network (last-connected first) under a hard fail-fast
// deadline, does a cheap "latest id" GET, and only downloads + stages a
// genuinely new frame (incoming -> validate size == frameBufferSize -> promote
// current.frame + current.id). Always leaves WiFi off; leaves the last valid frame
// untouched on any failure/timeout/no-net/no-creds/same-id.
//
// `deadline` is an absolute millis() timestamp capping the WHOLE call (0 = no cap,
// which is only sane in a context that can afford to block for minutes -- there
// isn't one). It bounds the note phase's own two HTTP calls, and it is the value
// the books hook is expected to work to as well. Without it neither HTTP call has
// any wall-clock bound: the note phase blocks on the calling task, so an AP that
// associates but has no route to the mailbox would hold sleep entry -- panel
// already showing the sleep screen, POWER doing nothing -- for as long as the
// per-socket-op timeout allows.
//
// Returns true iff this call promoted a NEW frame -- the signal enterDeepSleep()
// uses to decide whether the already-painted sleep screen needs one repaint.
//
// `whileLinkUp`, if set, is invoked once with the mailbox base URL and that same
// "a new note was staged" flag, AFTER the note phase is completely finished and
// BEFORE the radio is torn down. It exists so the books window (M2 #3) can ride
// the association the note phase already paid the <= 6 s connect budget and the
// TLS handshake for, instead of opening a second connect path. It runs on every
// outcome of the note phase except a failed connect -- an empty mailbox or an
// up-to-date note still leaves a perfectly good link to use.
//
// MessageSync knows nothing about what the hook does; it only guarantees the
// ordering (notes first, always) and that WiFi is off when syncBeforeSleep
// returns, whatever the hook did.
using LinkUpHook = std::function<void(const std::string& base, bool stagedNewNote)>;
bool syncBeforeSleep(size_t frameBufferSize, uint32_t deadline, const LinkUpHook& whileLinkUp = nullptr);

// --- Reusable: one note pass on a link somebody else owns -------------------
// The same shape BookSync::syncOnLink already has (BookSync.h:48), for the same
// reason: syncBeforeSleep owns the radio end to end -- it connects and guarantees
// WiFi is off when it returns -- which is the opposite of what a live-sync poll
// loop needs. This is the pass without those bookends, and syncBeforeSleep is now
// a thin composition of connect + this + hook + teardown.
//
// `base` is the mailbox base URL with no trailing slash, and it is a PARAMETER,
// never read from SETTINGS in here. That is the whole structural requirement the
// peer-proxy transport places on this surface (contract appendix A4: "build it
// with the base URL as a parameter", A3: the peer base is http://{peerIp}:{port}
// plus the path of the configured one). Base-URL selection is the only difference
// between the two transports.
//
// `deadline` is an absolute millis() timestamp bounding the WHOLE pass -- both
// HTTP calls -- and 0 means unbounded, which no caller should want: these are
// BLOCKING calls, so the deadline is the only thing that keeps a base that
// answers the association but not the request from holding the calling task.
//
// Stages through incoming -> validate -> promote (contract 3A "Staging
// invariants"), so a pass killed by its deadline at any byte leaves the
// previously staged current.frame bit-for-bit intact. Never touches the radio.
//
// A promoted note does NOT render here or anywhere near here: it becomes the
// sleep screen at the next sleep-entry, unconditionally (3A). Callers report
// "staged", they do not display.
enum class NoteResult : uint8_t {
  Failed,    // fetch failed, deadline hit, or the frame missed the exact-size gate
  NoNote,    // mailbox is empty
  UpToDate,  // newest note is the one already staged
  Staged,    // a NEW frame is at current.frame; it shows at the next sleep-entry
};
NoteResult syncOnLink(const std::string& base, size_t frameBufferSize, uint32_t deadline);

// Wall-clock budget for one note pass, measured from the moment the association
// is up: one tiny latest.txt GET plus, at most, one frame (52272 B on the X3) at
// the contract's 30 KB/s pessimistic floor = ~1.8 s, so 10 s is a handshake plus
// ~4x slack. Capped rather than "whatever is left of the window" so a mailbox
// that associates but does not answer cannot spend the books budget too.
constexpr uint32_t NOTE_PASS_BUDGET_MS = 10000;

// --- Reusable: the radio, for callers that own it ---------------------------
// Hard fail-fast budget for the whole connect phase; unreachable networks bail
// sooner on WL_CONNECT_FAILED / WL_NO_SSID_AVAIL.
constexpr uint32_t CONNECT_BUDGET_MS = 6000;

// Connect to a saved network, last-connected SSID first, within a hard overall
// deadline. Returns true iff associated. Leaves the radio UP on success -- the
// caller owns it from there and must call radioOff() on every exit path,
// including errors.
bool connectSavedNetwork(uint32_t budgetMs = CONNECT_BUDGET_MS);

// The one WiFi teardown recipe for this module's STA paths. Exposed so a caller
// that owns the radio tears it down exactly the way the sleep-entry path does
// rather than open-coding a variant that leaves the modem powered.
void radioOff();

// SETTINGS.messageSyncUrl with trailing slashes stripped; empty when unset. The
// configured mailbox base -- the base for the internet transport, and the string
// the peer transport takes the path portion of (contract section 6: one
// capability URL, one budget, no second settings field).
std::string configuredBase();

// --- Path B: wake-side check (stepped, zero UI) -----------------------------
// Arm one bounded check. Call from setup() ONLY on the branch that lands at the
// launcher AND has no book one keypress away: WiFi and EPUB rendering must never
// be resident at once, and WIFI_OFF does not defragment the heap a TLS session
// just fragmented. Silently no-ops when disabled, unconfigured, or without saved
// credentials. There is no wall-clock throttle -- the throttle is structural, at
// most one check per wake, enforced by the single call site.
void beginWakeCheck(size_t frameBufferSize);

// One step of the armed check; call once per main-loop iteration. The <= 6 s
// connect budget is polled rather than blocked on. The two HTTP steps DO block the
// calling task, but each is bounded by its own phase budget (3 s probe, 5 s frame),
// so that is the worst-case input latency they can add. No-op when nothing is
// armed.
void stepWakeCheck();

// Abandon an armed check and tear WiFi down immediately. Mandatory before any
// reader activity is constructed -- see ActivityManager::replaceActivity.
void cancelWakeCheck();

// True while a check is armed (used to hold off auto-sleep / CPU downclocking).
bool wakeCheckActive();

// --- Shared: the staged frame ----------------------------------------------
// Read /.love-notes/current.frame straight into a live framebuffer. Requires an
// exact size match (the only integrity signal -- there is no hash) and never
// deletes the file, so a mismatch just falls back to the wallpaper. buffer must
// be at least bufferSize bytes.
bool loadStagedNote(uint8_t* buffer, size_t bufferSize);

}  // namespace MessageSync
