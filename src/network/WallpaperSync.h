#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// Wallpaper delivery through the same mailbox as the love notes and the books.
// See docs/xteink/mailbox-books-contract.md, wallpaper section.
//
// WHY THIS EXISTS. Setting the reader's sleep screen used to require being on the
// same LAN as it: the app pushed a BMP straight at the reader's web server, to
// /sleep.bmp or /.sleep/<name>.bmp. A host who is not on that network had no
// route at all. The mailbox already carries notes and books to a reader that only
// comes up for a few seconds at a time, so the wallpaper rides the same windows,
// with the same manifest-and-diff model and none of its own machinery.
//
// THE MODEL IS BookSync's, DELIBERATELY. The server publishes a manifest, the
// reader decides, there is no ack and no per-device cursor. Every window the
// reader diffs {base}/wallpaper.txt against its own id-keyed state file and
// fetches at most ONE item, resuming across windows with HTTP Range until the
// bytes match the manifest exactly. Read BookSync.h before changing anything
// here; the consequences listed there (two readers converge independently, a lost
// SD card re-downloads, a reader that was off for a month sees only what the
// server still holds) apply unchanged.
//
// TWO DIFFERENCES FROM BOOKS, AND ONLY TWO.
//
//   1. THE MANIFEST IS NEWEST LAST, where books.txt is newest first. A book is
//      one of many things a library holds and the newest is the one the user is
//      waiting for; a wallpaper is a SETTING, and the last one the host set is
//      the one that must win. Taking the OLDEST unapplied entry each window and
//      applying entries in manifest order means a reader draining a backlog ends
//      on the newest primary rather than on whichever one it happened to fetch
//      first. The server does its half of that by superseding an undelivered
//      primary with a newer one instead of accumulating both.
//
//   2. THE PAYLOAD IS VALIDATED before it is applied. Books have no content gate
//      -- an epub that will not open is still a file in the user's library and
//      deleting it would be worse. A wallpaper that is not a parseable BMP is
//      read by the SLEEP RENDERER, on a code path with no user in front of it,
//      so it is rejected at the staging boundary and the id is recorded `skip`
//      so it is never fetched again.
//
// NOTHING HERE DELETES A WALLPAPER THE USER OWNS except the one it is replacing
// under an identical name, which is the whole point of a named slot. See the
// removal audit above syncOnLink() in the .cpp.
namespace WallpaperSync {

// Run one bounded wallpaper window on an ALREADY CONNECTED link.
//
// `base` is the mailbox base URL with no trailing slash, the same string the note
// and book paths build. `deadline` is an absolute millis() timestamp bounding the
// WHOLE window including the manifest fetch; it is enforced inside the body read
// loop, so a stalled socket cannot hold the window past it. Returns without
// fetching anything when too little of the budget is left to be useful.
//
// Never turns WiFi off (the caller owns the radio). Returns true iff a wallpaper
// was APPLIED by this call -- staged-but-incomplete is false, and so is a
// completed download that failed validation.
//
// `progress` exists for a FOREGROUND caller that has a screen and a Back button;
// the unattended sleep-entry window passes nothing. Both members are optional and
// behave exactly as BookSync::Progress does.
struct Progress {
  // Called at most once per syncOnLink(), after the manifest diff has picked a
  // target and before the first byte of it is requested. `primary` says which
  // slot the item is bound for, because a primary carries no filename on the wire
  // (it is a fixed slot) and only the caller has an i18n table to name it with --
  // this unit stays free of I18n.h on purpose. `have` is what previous windows
  // already staged, so a resuming caller can show real progress.
  std::function<void(const std::string& filename, bool primary, size_t bytes, size_t have)> onTarget;

  // Polled while bytes flow. Return true to abandon THIS window; the partial is
  // kept and the next window resumes from it, which is the same outcome as the
  // deadline expiring mid-body.
  std::function<bool()> shouldAbort;
};
bool syncOnLink(const std::string& base, uint32_t deadline, const Progress& progress = {});

// Transfer budget for one window. The same number as BookSync::WINDOW_BUDGET_MS
// and for the same reason: at the measured 30 KB/s pessimistic floor it is 71%
// window efficiency against the ~8 s fixed connect+handshake cost. A panel-sized
// 8bpp BMP is ~1.1 MB, so the pessimistic floor needs two windows for one
// wallpaper and a normal link needs one; both are resumed, not restarted.
constexpr uint32_t WINDOW_BUDGET_MS = 20000;

// Below this much remaining budget the window is skipped entirely -- a TLS
// handshake plus a manifest round-trip would eat most of it.
constexpr uint32_t MIN_USEFUL_MS = 4000;

}  // namespace WallpaperSync
