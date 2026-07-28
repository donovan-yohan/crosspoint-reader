#pragma once
#include <cstddef>

// M2 #1 (messenger): headless love-note sleep-sync + wake dedup.
// See docs/xteink/m2-1-wake-sync-plan.md ("REDESIGN 2026-07-28").
// Sync runs at deep-sleep entry (the user has walked away and the sleep screen
// is already on the panel, so the bounded WiFi window is hidden). Rendering
// happens instantly at the next wake from the pre-staged frame, gated by message
// id so a note is shown exactly once and never on every wake.
namespace MessageSync {

// Sleep-entry sync. No-op unless SETTINGS.messageSyncEnabled and a non-empty
// SETTINGS.messageSyncUrl. Connects to a saved WiFi (last-connected first) with
// a hard fail-fast deadline, does a cheap "latest id" GET, and only downloads +
// stages a genuinely new frame (temp -> validate size == frameBufferSize ->
// promote current.frame + current.id). Always leaves WiFi off; never blocks
// beyond the bounded connect deadline; leaves the last valid frame untouched on
// any failure/timeout/no-net/no-creds/same-id.
void syncBeforeSleep(size_t frameBufferSize);

// Wake gate: true if /.love-notes/current.frame exists AND its staged id differs
// from the persisted lastShownId. An id-less legacy frame is always unread (M1
// behaviour). Requires APP_STATE to be loaded first.
bool hasUnreadNote();

// Persist the staged note's id as lastShownId so it will not reshow next wake.
// Call once the frame has actually been rendered.
void markCurrentNoteShown();

}  // namespace MessageSync
