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
// BookSync::syncOnLink, the same staging, the same progress screen, the same
// session cap, the same teardown. `Transport` selects a base URL and nothing else.
//
// WHAT THE USER SEES, AND WHAT THEY DELIBERATELY DO NOT. Books land in /books and
// are readable the moment the mode exits, because a book is a file in a library.
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

// Poll cadence. Both polls are tiny by contract -- latest.txt is <= 128 B and
// books.txt is capped at 8192 B on the reader -- so a round trip is ~4 KB of
// payload, which is the only reason 4 s is affordable at all.
//
// ON THE STA TRANSPORT THIS IS STILL EXPENSIVE, AND IT IS A KNOWN GAP.
// SecureHttpClient is stack-local inside HttpDownloader's hop loop, so keep-alive
// is dead across calls (contract 5 G6): every poll pays two fresh TLS handshakes
// for those 4 KB. A3's peer link has no handshake at all -- it is plain HTTP,
// TLS terminates on the phone -- so G6 is a constraint on the internet transport
// only, and the peer transport is the one this cadence was chosen for.
constexpr uint32_t POLL_INTERVAL_MS = 4000;

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
    Receiving,     // A book window is running; targetName is on the panel.
    Stalled,       // Consecutive failed polls. Still trying -- not an exit.
    Failed,        // Terminal. failureText says why; Back is the only way out.
    Finished,      // Session cap spent. Shows the counts.
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
  std::string targetName;   // Book currently being received, for the Receiving state.
  size_t targetBytes = 0;   // ...and its size, so the panel can show "1.2 / 4.0 MB".
  size_t targetHave = 0;
  StrId failureText = StrId::STR_SYNC_FINISHED;  // Meaningful only in State::Failed.

  int notesStaged = 0;
  int booksReceived = 0;

  // The network handed over on this session, empty until one has been saved. Both
  // the "did it happen" flag and the panel line, because there is exactly one
  // handover per session by construction: the pickup is skipped once this is set.
  std::string wifiSavedSsid;
  // "The log already says we could not take what the app offered." A refusal
  // leaves the credential staged on purpose (see SSID_MAX_BYTES above), so the
  // pickup keeps retrying on the poll cadence -- and the line has to be latched or
  // it repeats every four seconds for the rest of the session.
  bool wifiRefusalLogged = false;

  uint32_t sessionDeadline = 0;  // Absolute millis(); set once the activity starts.
  uint32_t phoneWaitDeadline = 0;
  uint32_t nextPollAt = 0;
  uint32_t lastAbortPoll = 0;  // Throttles the in-transfer Back sampling.
  int consecutiveFailures = 0;

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
    StrId failure;
    size_t wifiHash;
    bool operator==(const PaintSignature& o) const {
      return state == o.state && nameHash == o.nameHash && have == o.have && notes == o.notes && books == o.books &&
             failure == o.failure && wifiHash == o.wifiHash;
    }
  };
  PaintSignature painted{State::Finished, 0, 0, -1, -1, StrId::STR_SYNC_FINISHED, 0};

  PaintSignature signature() const;
  // The ONLY paint call site. Blocking (requestUpdateAndWait) so the panel really
  // is showing the new state before the caller goes off and blocks on a socket.
  void paintIfChanged();
  void fail(StrId reason);

  bool startSavedNetworkLink();
  bool startPhoneApLink();
  void stepPhoneApLink();  // WaitingPhone -> Linking -> Polling, and back on a drop.
  void runPollCycle();
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
