#pragma once
#include <cstddef>
#include <cstdint>

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
// Everything here is gated on SETTINGS.messageSyncEnabled plus a non-empty
// SETTINGS.messageSyncUrl; there are no other user-facing settings.
namespace MessageSync {

// --- Path A: sleep-entry sync (blocking, bounded) --------------------------
// Connects to a saved WiFi network (last-connected first) under a hard fail-fast
// deadline, does a cheap "latest id" GET, and only downloads + stages a
// genuinely new frame (incoming -> validate size == frameBufferSize -> promote
// current.frame + current.id). Always leaves WiFi off; never blocks beyond the
// bounded connect deadline; leaves the last valid frame untouched on any
// failure/timeout/no-net/no-creds/same-id.
//
// Returns true iff this call promoted a NEW frame -- the signal enterDeepSleep()
// uses to decide whether the already-painted sleep screen needs one repaint.
bool syncBeforeSleep(size_t frameBufferSize);

// --- Path B: wake-side check (stepped, zero UI) -----------------------------
// Arm one bounded check. Call from setup() ONLY on the branch that lands at the
// launcher: WiFi and EPUB rendering must never be resident at once, so a wake
// that resumes straight into the reader must not run a check. Silently no-ops
// when disabled, unconfigured, without saved credentials, or throttled.
void beginWakeCheck(size_t frameBufferSize);

// One non-blocking step of the armed check; call once per main-loop iteration.
// The <= 6 s connect budget is polled rather than blocked on, so input stays
// responsive at the launcher. No-op when nothing is armed.
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
