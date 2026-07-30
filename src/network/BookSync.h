#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// M2 #3 (messenger): epub delivery through the same mailbox as the love notes.
// See docs/xteink/mailbox-books-contract.md sections 2, 3 and 4.
//
// THE SERVER PUBLISHES A MANIFEST; THE READER DECIDES. There is no ack, no
// per-device cursor, no "delivered" flag. Every window the reader diffs
// {base}/books.txt against its own id-keyed state file and fetches at most ONE
// book, resuming across windows with HTTP Range until the bytes match the
// manifest's size exactly. Consequences of that model, all deliberate:
//
//   * two readers can share one mailbox and converge independently;
//   * a reader that loses its SD card re-downloads everything (correct);
//   * a reader that was off for a month sees only the newest entries the server
//     still holds, and nothing anywhere knows it missed the rest.
//
// THE MANIFEST IS AUTHORITATIVE ABOUT WHAT EXISTS, NEVER ABOUT WHAT THE READER
// KEEPS. A local file that has dropped off the manifest is NOT deleted, and
// neither is one the manifest never named: /books is the user's library, and the
// reader is the only party that knows what is in it. Nothing in here removes a
// readable book -- see the removal audit in BookSync.cpp.
//
// Diffing is by id and never by filename, because a finished book LEAVES /books
// (EpubReaderActivity moves it to /read/ and may rename it on collision). A
// filename diff would silently re-download every book the user finishes, for
// ever.
namespace BookSync {

// Run one bounded books window on an ALREADY CONNECTED link.
//
// Called from the sleep-entry sync while the note phase's WiFi association is
// still up, so the <= 6 s connect budget and its TLS handshake are paid once for
// both halves (contract section 3, Placement). Notes always go first: this never
// runs before the note phase has finished with the link.
//
// `base` is the mailbox base URL with no trailing slash -- the same string the
// note path builds. `deadline` is an absolute millis() timestamp bounding the
// WHOLE window including the manifest fetch; it is enforced inside the body read
// loop, so a stalled socket cannot hold the window past it. Returns without
// fetching anything when too little of the budget is left to be useful.
//
// Never turns WiFi off (the caller owns the radio) and never blocks beyond
// `deadline` plus one SD promote. Returns true iff a book was promoted into
// /books by this call.
//
// `progress` exists for a FOREGROUND caller that has a screen and a Back button
// (contract appendix A4's live-sync mode); the unattended sleep-entry window
// passes nothing and behaves exactly as before. Both members are optional.
struct Progress {
  // Called at most once per syncOnLink(), after the manifest diff has picked a
  // target and before the first byte of it is requested, so a progress screen can
  // name the book it is about to spend a window on. `have` is what previous
  // windows already staged, so a resuming caller can show "1.2 of 4.0 MB" without
  // this unit growing a per-chunk hook -- deliberately absent, because the panel
  // has no partial refresh and a repaint per chunk would be 1720 ms of e-ink per
  // chunk (A4 "Render -- repaint on state change, never per poll").
  std::function<void(const std::string& filename, size_t bytes, size_t have)> onTarget;

  // Polled while bytes flow. Return true to abandon THIS window; the partial is
  // kept and the next window resumes from it, which is the same outcome as the
  // deadline expiring mid-body. It exists so Back stays responsive during a
  // multi-megabyte transfer instead of waiting out the window budget: the caller
  // is blocked inside this function for the whole window, so nothing else can
  // observe the button. Called from HttpDownloader's progress path, which is
  // per-chunk but only fires when the server reported a body size -- so it is a
  // latency improvement, never the bound. `deadline` remains the only guarantee.
  std::function<bool()> shouldAbort;
};
bool syncOnLink(const std::string& base, uint32_t deadline, const Progress& progress = {});

// Transfer budget for one window, contract section 3 "Byte budgets": at the
// measured 30 KB/s pessimistic floor, 20 s puts a 400 KiB novel in one window
// and a 2 MiB book in four, at 71% window efficiency against the ~8 s fixed
// connect+handshake cost. Shorter windows lose to the fixed cost; longer ones
// buy little and raise the "user picks the device back up mid-window" risk.
constexpr uint32_t WINDOW_BUDGET_MS = 20000;

// Below this much remaining budget the window is skipped entirely: a TLS
// handshake plus a manifest round-trip would eat most of it, and a download that
// transfers a few hundred bytes before the deadline still costs a full window's
// radio time.
constexpr uint32_t MIN_USEFUL_MS = 4000;

}  // namespace BookSync
