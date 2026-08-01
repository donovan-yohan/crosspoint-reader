#pragma once

#include <I18nKeys.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "activities/Activity.h"

// M2 #3 / M3: the user-initiated mailbox drain. Contract appendix A4 (live sync
// over a saved network) and appendix A3 (the same loop with a phone acting as the
// mailbox's front door on the reader's own AP).
//
// ONE ACTIVITY, TWO TRANSPORTS, AND A4 IS EMPHATIC THAT THEY MUST NOT FORK. The
// link comes up differently -- a headless STA connect to a saved network, or a
// softAP the phone joins -- and after that every single thing is identical: the
// same poll cadence, the same MessageSync::syncOnLink, the same
// BookSync::syncOnLink, the same WallpaperSync::syncOnLink, the same staging, the
// same progress screen, the same session cap, the same teardown. `Transport`
// selects a base URL and nothing else.
//
// WHAT THE USER SEES, AND WHAT THEY DELIBERATELY DO NOT. Books land in /books and
// are readable the moment the mode exits, because a book is a file in a library.
// A wallpaper lands in its slot here too, but by its nature it can only be SEEN
// at the next sleep; the counts line is the receipt, and the panel is no more
// hijacked to preview it than it is to preview a note.
// A note does NOT render here: contract 3A routes every note through the sleep
// screen, so a note staged during a sync takes its one turn on the panel at the
// next sleep-entry (SleepActivity, keyed on the note id) and the progress screen
// only says that it will. Rendering it inline would reintroduce exactly the
// interrupting-note model 3A deleted. Nothing in this activity touches
// APP_STATE.messageLastDisplayedId -- staging is not displaying.
//
// REPAINT ON STATE CHANGE, NEVER PER POLL. There is no partial-refresh path
// exposed to activities (GfxRenderer::displayWindow is commented out), so every
// paint is a full frame and a HALF refresh is 1720 ms. A screen that ticked once
// per 4 s poll would be ~450 full-frame refreshes in a capped session -- a
// strobing panel, most of a second in every four spent refreshing, for no
// information. paintIfChanged() is the only paint call site and it compares a
// signature of everything the screen shows, so "repaint only on a transition" is
// structural here rather than a rule someone has to remember.
namespace MailboxSync {

// POLL CADENCE, AND IT IS ADAPTIVE RATHER THAN FIXED.
//
// Both polls are tiny by contract -- latest.txt is <= 128 B and books.txt is
// capped at 8192 B on the reader -- so a round trip is ~4 KB of payload, which is
// the only reason a 4 s idle cadence is affordable at all.
//
// ON THE STA TRANSPORT THIS IS STILL EXPENSIVE, AND IT IS A KNOWN GAP.
// SecureHttpClient is stack-local inside HttpDownloader's hop loop, so keep-alive
// is dead across calls (contract 5 G6): every poll pays two fresh TLS handshakes
// for those 4 KB. A3's peer link has no handshake at all -- it is plain HTTP,
// TLS terminates on the phone -- so G6 is a constraint on the internet transport
// only, and the peer transport is the one this cadence was chosen for.
//
// WHY THREE NUMBERS AND NOT ONE. A single fixed interval was charged to the case
// it is worst for. The loop used to sleep the interval UNCONDITIONALLY, including
// straight after a cycle that had just transferred something -- and because each
// window fetches at most one item, a mailbox holding N items cost at least N x
// interval of pure idling on top of the transfers themselves. The user's report
// is that exact arithmetic: "sync with app takes a long time from starting it to
// items landing". The interval is a POWER measure for an idle link, and an idle
// link is the only thing it should be charged to.
//
// THE SAME THREE NUMBERS ON BOTH TRANSPORTS. A4 is emphatic that the transports
// must not fork after the link is up, and cadence is named in that list, so the
// policy here is transport-independent on purpose. The STA transport pays its
// handshake per poll either way; what a warm cadence costs there is bounded by
// the handshake itself (the interval is measured from the END of the previous
// cycle, so a 3 s cycle at POLL_WARM_MS is a 3.75 s effective cadence, not a
// 0.75 s one) and it is spent only in the first few cycles of a session the user
// deliberately started and is standing over.

// After a cycle that COMPLETED something: go again with no interval at all. The
// queue is demonstrably non-empty, the reader is awake, the radio is up and the
// user is watching -- there is nothing to save by waiting. Not a busy-spin, and
// structurally so: a cycle that completed an item necessarily did a manifest
// round trip and a body transfer, and main.cpp's loop() ends in delay(10) on
// every iteration (this activity does not request skipLoopDelay), so the task
// yields between cycles regardless of what this number says.
constexpr uint32_t POLL_ACTIVE_MS = 0;

// HOW MANY CYCLES MAY RUN BACK-TO-BACK AT POLL_ACTIVE_MS BEFORE THE CADENCE
// STOPS TRUSTING "MOVED", and it exists because the premise under
// POLL_ACTIVE_MS is not universally true.
//
// The premise is "a completed item cannot repeat, because it is recorded in an
// id-keyed state file and never fetched again". That holds on every path where
// the record lands -- and all three sync modules DELIBERATELY report success
// when the body landed but the record did not, because the item genuinely did
// arrive and losing it would be worse than re-fetching it. Read the three
// sites: BookSync.cpp promote() ("promoted but not recorded ... re-fetched next
// window"), WallpaperSync.cpp applyEntry() ("applied but not recorded"), and
// MessageSync.cpp promoteIncoming(), which drops the id sidecar when its write
// fails so the frame reads as unseen. Each of those is a completion that DOES
// repeat, every cycle, for as long as the SD card keeps refusing the small
// write while accepting the large one.
//
// A fixed poll interval used to rate-limit that to one repeat per interval. A
// zero interval does not, so the same failing card would be re-downloading and
// re-writing at link speed for the rest of the session cap -- which is the one
// thing a device with a sick filesystem must not be made to do. This bounds the
// zero-interval run instead: after this many consecutive cycles that all claim
// to have moved something, the cadence drops to POLL_WARM_MS, which is still
// five times faster than the old fixed interval.
//
// Sized so no real backlog can reach it: MAX_ITEMS_PER_CYCLE items per cycle
// means 8 x 3 = 24 items drain with no interval at all, and a queue past that
// pays POLL_WARM_MS per three items -- a few hundred ms against transfers that
// dominate it. The counter is cleared by any cycle that does NOT complete
// something, so it only ever counts an uninterrupted run.
constexpr uint8_t MAX_ACTIVE_CYCLES = 8;

// The first POLL_WARM_CYCLES cycles that find nothing, counted from link-up and
// re-armed by every completed item. A phone typically finishes uploading into its
// own queue a beat AFTER the reader has linked to it, so the seconds right after
// the link comes up are precisely when "nothing yet" is most likely to become
// "something" -- and 750 ms of a plain-HTTP round trip on a one-hop link is a few
// KB. 8 x 750 ms is a ~6 s warm window, which covers a user tapping send in the
// app while watching the reader.
constexpr uint32_t POLL_WARM_MS = 750;
constexpr uint8_t POLL_WARM_CYCLES = 8;

// Genuinely idle: the warm window is spent and nothing has arrived. This is the
// old fixed interval, unchanged, and it is still what a long unattended session
// settles at -- the power argument for it has not changed, only the set of cycles
// it is charged to. Also the retry interval for a FAILED poll, so the stall
// threshold below still measures the ~12 s it was written for.
constexpr uint32_t POLL_IDLE_MS = 4000;

// HOW MANY ITEMS ONE CYCLE MAY DRAIN. BookSync and WallpaperSync each fetch at
// most one item per call by design, so "one call each per cycle" put a hard
// one-item-per-interval ceiling on the whole mode. Three passes of a book window
// plus a wallpaper window would be 3 x (20 + 20) = 120 s of windows if every one
// of them ran long, so THE ITEM COUNT IS NOT WHAT KEEPS A CYCLE HONEST --
// CYCLE_DRAIN_BUDGET_MS IS, and it cuts the drain at 40 s however many passes
// are left. What this number bounds is the number of manifest round trips a
// single cycle can spend, and three is where that stops paying: anything past it
// lands on the next cycle, which POLL_ACTIVE_MS starts with no interval at all.
// The pair puts the worst-case cycle at the note pass plus the drain budget,
// 10 + 40 = 50 s, against the 10 + 20 + 20 + 4 = 54 s that shipped.
constexpr uint8_t MAX_ITEMS_PER_CYCLE = 3;

// Wall-clock bound on the drain loop, capped by the session deadline like every
// other budget here. MAX_ITEMS_PER_CYCLE bounds the number of windows and this
// bounds their total time, so neither a slow link nor a manifest full of large
// books can hold one cycle open indefinitely.
//
// AND THE NUMBER IS SET BY NOTE STARVATION, NOT BY THE DRAIN. The note pass runs
// at the HEAD of a cycle and nowhere else, so this budget is exactly how long a
// note that arrives one moment too late waits behind books before anyone asks
// for it again. That gap used to be one book window plus one wallpaper window
// plus the poll interval: 20 + 20 + 4 = 44 s. Draining three items per cycle
// makes the books ahead of it longer, and a zero interval after them takes only
// the 4 s back -- so at 60000 the gap grew to 60 s and this mode got SLOWER at
// the one thing it exists for, which is delivering a message someone is waiting
// on. 40000 is chosen so it cannot: 40 + 0 = 40 s is strictly better than the
// 44 s that shipped.
//
// It costs the drain nothing in the case the drain was written for. Three small
// books over a one-hop peer link are seconds, not minutes, so the item count is
// what binds and all three still land in one cycle. The only case this cuts
// short is three consecutive SLOW books -- which is precisely the case where a
// waiting note should get the link back, and it is not even a delay: the next
// cycle starts at POLL_ACTIVE_MS, so the books resume immediately after the note
// pass they now let through.
constexpr uint32_t CYCLE_DRAIN_BUDGET_MS = 40000;

// Hard session cap, a safety net rather than a budget: the mode holds the radio
// up and preventAutoSleep() asserted for its whole life, so it must not be able
// to outlive a user who walked away. Enforced against an absolute millis()
// deadline in the loop, never against the 60 s per-socket-op timeout (5 G7).
constexpr uint32_t SESSION_CAP_MS = 30UL * 60UL * 1000UL;

// How long the AP transport waits for a phone to associate before giving up, and
// how long it then spends looking for the forwarder among the DHCP leases. The
// join wait is generous because it includes a human: unlocking a phone, opening
// the app, accepting the system join dialog.
constexpr uint32_t PHONE_JOIN_WAIT_MS = 120000;
constexpr uint32_t PEER_DISCOVERY_MS = 8000;

// HOW OFTEN AN ASSOCIATED-BUT-SILENT STATION IS RE-PROBED, and it is deliberately
// NOT the poll cadence any more.
//
// The two were the same variable, which meant the reader waited a full 4 s poll
// interval between "the phone associated" and "ask it again whether its listener
// is up". A phone opens that listener within about a second of association --
// often after it, because the system join dialog resolves before the app's server
// binds -- so the single most common way to lose several seconds in this mode was
// to probe once, a moment too early, and then sit out a poll interval. Retrying
// twice a second for the first PEER_FAST_ATTEMPTS covers that window with attempts
// instead of with waiting.
//
// THE TOTAL BUDGET IS NOT EXTENDED BY THIS. PEER_DISCOVERY_MS still bounds each
// sweep, and once PEER_FAST_ATTEMPTS are spent both the retry interval
// (POLL_IDLE_MS) and the per-candidate budget (PeerProbe::CANDIDATE_BUDGET_MS)
// revert to exactly what they were, so a station that never answers costs the
// same for the rest of the session as it always did.
constexpr uint32_t PEER_RETRY_MS = 500;

// How many sweeps run on the fast cadence and the short per-candidate budget
// before discovery settles into its patient form. Eight covers roughly the first
// four seconds after a station appears, which is several times the ~1 s a
// foreground app needs to bind its listener; past that the phone is not merely
// slow to start, and hammering it is neither faster nor free.
constexpr uint8_t PEER_FAST_ATTEMPTS = 8;

// WI-FI HANDOVER OVER THE PEER LINK -- the reason this feature exists is that a
// Wi-Fi password is the single worst thing to type on e-ink. The phone already
// knows the network the user is standing in, the user types the password ONCE
// there, and the credential rides the link that is already up.
//
// THE WIRE, pinned here because the app half is in another repository:
//   GET    {peerOrigin}/cp-wifi -> 200 "ssid\npassword\n" (text), or 404 when the
//                                  user has staged nothing.
//   DELETE {peerOrigin}/cp-wifi -> the reader's acknowledgement; the app wipes its
//                                  staging on it and only on it.
//
// PEER ORIGIN, NOT THE MAILBOX BASE. Every other request in this activity goes to
// base = origin + /m/{boxId}; this one goes to the ORIGIN and a fixed path, the
// same boxId-free construction PeerProbe uses for its health probe. The capability
// URL has no business in a request that is not a mailbox read, and keeping the
// boxId off this path means an app that serves /cp-wifi does not have to be told
// the box id to do it.
//
// AP TRANSPORT ONLY, and that is a security boundary rather than a convenience.
// On the saved-network transport the origin is a public mailbox host somewhere on
// the internet; asking it for the user's home Wi-Fi password would be absurd, and
// accepting an answer would be a straightforward way to plant a network on the
// reader. The pickup is gated on Transport::PhoneAp at its only call site.
constexpr char WIFI_SHARE_PATH[] = "/cp-wifi";

// Per-attempt wall-clock budget for both the pickup and the ack. Plain HTTP, one
// hop, no route anywhere and ~100 bytes of payload: the same reasoning (and the
// same number) as PeerProbe::CANDIDATE_BUDGET_MS.
constexpr uint32_t WIFI_PICKUP_BUDGET_MS = 1500;

// Largest body the reader will take from /cp-wifi. A legal answer is at most
// 32 + 1 + 63 + 2 = 98 bytes; a peer that sends more is not answering this
// contract, so the transfer is aborted rather than buffered.
constexpr size_t MAX_WIFI_BODY_BYTES = 128;

// How many consecutive unanswered pickups end the attempt for this link.
//
// The pickup runs at the head of every poll cycle, and for the overwhelming
// majority of sessions there is nothing staged, so every one of them is a 404.
// That is not free: each costs a TCP connect and up to WIFI_PICKUP_BUDGET_MS
// ahead of the latency-sensitive note pass, and HttpDownloader logs every
// non-200 at ERR unconditionally. Over a capped 30 minute session at the 4 s
// cadence that is ~450 connects and ~450 ERR lines on the serial console, which
// is the channel used to diagnose everything else about this mode.
//
// Three, and not one, because the phone's forwarder can answer the health probe
// a moment before its own staging is armed. Three costs nothing in the sharing
// case: the credential is staged before the session starts, so the FIRST pickup
// finds it. Re-armed on peer re-discovery, which is the event that means "this
// is a different link now".
//
// THE GRACE THIS BUYS IS NOW MEASURED IN CYCLES THAT ARE CLOSER TOGETHER. Three
// misses used to span ~12 s of the fixed interval and now span ~2 s of the warm
// one. That is still the race it was written for -- the listener and the staging
// are two pieces of in-process state on the same phone, set microseconds apart --
// and the number stays at three because what it bounds (a TCP connect and an ERR
// line at the head of the latency-sensitive note pass) is a per-attempt cost, not
// a per-second one.
constexpr uint8_t WIFI_PICKUP_MAX_MISSES = 3;

// What the reader will accept as a credential. SSID is 1..32 bytes (802.11), and
// a WPA passphrase is 8..63 printable ASCII -- empty is allowed and means "this
// network is open". Anything else is refused WITHOUT an ack, so it stays staged on
// the phone and a corrected value is picked up on a later session rather than
// silently lost.
constexpr size_t SSID_MAX_BYTES = 32;
constexpr size_t PSK_MIN_BYTES = 8;
constexpr size_t PSK_MAX_BYTES = 63;

enum class Transport : uint8_t {
  // Appendix A4: headless connect to a saved network (the canonical one being the
  // user's own phone hotspot), base = the configured mailbox URL.
  SavedNetwork,
  // Appendix A3: raise the reader's own AP -- WITHOUT the web server, so nothing
  // on the reader is writable from the peer link -- and speak the contract at the
  // phone's forwarder. Base = http://{peerIp}:8080 + the path of the configured URL.
  PhoneAp,
};

}  // namespace MailboxSync

class MailboxSyncActivity final : public Activity {
 public:
  MailboxSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, MailboxSync::Transport transport)
      : Activity("MailboxSync", renderer, mappedInput), transport(transport) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Asserted for the whole live session. The inactivity timer would otherwise
  // sleep the device mid-drain; the precedent is MessageSync's wake check, which
  // sits in the same condition at main.cpp for exactly this reason.
  //
  // RELEASED once the session is over -- cap spent or failed -- because those
  // states are terminal, the radio is already down, and they wait on the user
  // pressing Back. Holding the device awake indefinitely on a screen nobody is
  // reading would reintroduce the battery drain the session cap exists to prevent,
  // for a user who has walked away.
  bool preventAutoSleep() override;

 private:
  // The screen, and therefore the paint trigger. Every transition below is a
  // full-frame repaint and nothing else is.
  enum class State : uint8_t {
    Connecting,    // STA: associating with a saved network. AP: raising the softAP.
    WaitingPhone,  // AP only: AP is up, SSID + PSK on the panel, no station yet.
    Linking,       // AP only: a station joined; probing the leases for the forwarder.
    Polling,       // Link up, base known, nothing in flight.
    Receiving,     // A book or wallpaper window is running; targetName is on the panel.
    Stalled,       // Consecutive failed polls. Still trying -- not an exit.
    Failed,        // Terminal. failureText says why; Back is the only way out.
    Finished,      // Session cap spent. Shows the counts.
  };

  // What one poll cycle turned out to be worth, which is the only input the
  // cadence takes. Three outcomes and not two, because "the poll failed" must not
  // be read as "the mailbox is empty": a failing link retried on the warm cadence
  // would reach STALL_THRESHOLD in ~2 s and flash the stalled line at a user whose
  // hotspot merely re-associated, which is the thing the threshold exists to
  // prevent.
  enum class CycleResult : uint8_t {
    Moved,   // an item COMPLETED: a note staged, a book promoted, a wallpaper applied
    Idle,    // the link is fine and there was nothing to take (or a transfer is still mid-flight)
    Failed,  // the note pass did not get through
  };

  const MailboxSync::Transport transport;

  State state = State::Connecting;
  // The configured mailbox URL, read from SETTINGS EXACTLY ONCE at link bring-up and
  // never again -- appendix A3's structural requirement on A4. The AP transport
  // needs it after entry (the peer base is only composable once a forwarder has been
  // discovered, and is recomposed when the phone rejoins on a different lease), and
  // this member is what it reads instead of going back to SETTINGS from inside the
  // loop. The STA transport has no use for it: there the base IS the configured URL.
  std::string configuredUrl;
  std::string base;         // Empty until the link is up and the base is composed.
  std::string apSsid;       // AP transport only, shown on the panel.
  std::string apPsk;        // Shown on the panel. Per DEVICE: APP_STATE.mailboxApPsk.
  std::string targetName;   // Book or wallpaper being received, for the Receiving state.
  size_t targetBytes = 0;   // ...and its size, so the panel can show "1.2 / 4.0 MB".
  size_t targetHave = 0;
  StrId failureText = StrId::STR_SYNC_FINISHED;  // Meaningful only in State::Failed.

  int notesStaged = 0;
  int booksReceived = 0;
  // Wallpapers actually APPLIED this session, not merely downloaded: a blob that
  // arrived complete and failed the BMP parse is not something to congratulate the
  // user about. Rendered on its own line and only once it is non-zero, so the
  // established two-label counts line keeps its layout for every session that
  // never receives one.
  int wallpapersApplied = 0;

  // The network handed over on this session, empty until one has been saved. Both
  // the "did it happen" flag and the panel line, because there is exactly one
  // handover per session by construction: the pickup is skipped once this is set.
  std::string wifiSavedSsid;
  // "The log already says we could not take what the app offered." A refusal
  // leaves the credential staged on purpose (see SSID_MAX_BYTES above), so the
  // pickup keeps retrying on the poll cadence -- and the line has to be latched or
  // it repeats every four seconds for the rest of the session.
  bool wifiRefusalLogged = false;
  // Consecutive pickups that got no answer on THIS link. At
  // WIFI_PICKUP_MAX_MISSES the pickup stops being attempted; reset when a peer
  // base is (re)discovered, which is the only point at which the thing on the
  // other end can have changed.
  uint8_t wifiPickupMisses = 0;

  uint32_t sessionDeadline = 0;  // Absolute millis(); set once the activity starts.
  uint32_t phoneWaitDeadline = 0;
  uint32_t nextPollAt = 0;
  // When the next peer-discovery sweep may run. SEPARATE FROM nextPollAt on
  // purpose: they were one variable, which tied "how often do we ask the phone
  // whether its listener is up" to "how often do we ask the mailbox for mail",
  // and those two want opposite cadences. AP transport only.
  uint32_t nextProbeAt = 0;
  uint32_t lastAbortPoll = 0;  // Throttles the in-transfer Back sampling.
  int consecutiveFailures = 0;
  // Consecutive cycles that found nothing, capped at POLL_WARM_CYCLES. Zeroed by
  // link-up and by every completed item, which is what makes the warm window
  // "just after something interesting happened" rather than "once per session".
  uint8_t idleCycles = 0;
  // Consecutive cycles that COMPLETED something, capped at MAX_ACTIVE_CYCLES.
  // The brake on the zero interval: see MAX_ACTIVE_CYCLES for why a completion
  // is not the never-repeats signal POLL_ACTIVE_MS would like it to be. Cleared
  // by any cycle that completes nothing, so an ordinary backlog never reaches it.
  uint8_t activeCycles = 0;
  // Consecutive failed discovery sweeps on this link. Drives both the retry
  // interval and the per-candidate probe budget down from fast to patient, and is
  // cleared by a sweep that found the forwarder or by the station leaving.
  uint8_t probeAttempts = 0;

  bool linkStarted = false;  // Guards the one-shot bring-up out of the first loop().
  // AP transport: "the panel has already said we are looking for the app on this
  // link". Latched when the probe sweep is announced and cleared only by a sweep
  // that succeeded, so a station that associates without ever answering /cp-proxy
  // costs ONE repaint rather than one per retry. Together with the poll-cadence gate
  // on re-discovery this is what keeps WaitingPhone <-> Linking from strobing the
  // panel for a whole session -- see stepPhoneApLink().
  bool probeAnnounced = false;
  // Two separate facts, because they answer two different questions. `radioTouched`
  // is "did any WiFi call happen", and it is what makes teardown unconditional on
  // every exit path including a failed connect. `sessionRan` is "did a link
  // actually come up and carry traffic", and it is what earns the silentRestart():
  // a session that never associated has not fragmented the heap and rebooting the
  // user out of the menu for it would be gratuitous.
  bool radioTouched = false;
  bool sessionRan = false;
  bool exiting = false;       // Latched by Back (possibly from inside a download), so
                              // nothing repaints on the way out.
  bool finishCalled = false;  // finish() is idempotent from this activity's side.

  // Everything the screen shows, folded into one comparable value. A paint happens
  // iff this changed.
  struct PaintSignature {
    State state;
    size_t nameHash;
    size_t have;
    int notes;
    int books;
    int wallpapers;
    StrId failure;
    size_t wifiHash;
    bool operator==(const PaintSignature& o) const {
      return state == o.state && nameHash == o.nameHash && have == o.have && notes == o.notes && books == o.books &&
             wallpapers == o.wallpapers && failure == o.failure && wifiHash == o.wifiHash;
    }
  };
  PaintSignature painted{State::Finished, 0, 0, -1, -1, -1, StrId::STR_SYNC_FINISHED, 0};

  PaintSignature signature() const;
  // The ONLY paint call site. Blocking (requestUpdateAndWait) so the panel really
  // is showing the new state before the caller goes off and blocks on a socket.
  void paintIfChanged();
  void fail(StrId reason);

  bool startSavedNetworkLink();
  bool startPhoneApLink();
  void stepPhoneApLink();  // WaitingPhone -> Linking -> Polling, and back on a drop.
  // One poll cycle, and the answer the cadence is chosen from.
  CycleResult runPollCycle();
  // One /cp-wifi pickup attempt, from the front of a poll cycle on the AP
  // transport. Everything about it is non-fatal to the session: a 404 (nothing
  // staged, or an app too old to serve the path) and a transport failure are the
  // same "not this cycle", and neither touches the note poll's stall bookkeeping.
  void tryWifiHandoff();
  void teardownRadio();
  // Polled from inside a book transfer so Back does not have to wait out the
  // window budget. Latches `exiting`; the transfer unwinds through the existing
  // "keep the partial" path.
  bool pollForAbort();

  void renderBody(int contentTop) const;
  void renderPhoneJoinPanel(int contentTop) const;
  const char* statusLine() const;
};
