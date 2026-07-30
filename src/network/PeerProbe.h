#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// M3 / contract appendix A3: "sync with app" -- the reader raises its OWN AP, the
// phone joins it as a peer while keeping cellular as its default route, and the
// app runs a tiny read-only HTTP forwarder on that link. The reader then speaks
// the same mailbox contract at the phone that it speaks at the internet, so
// nothing in section 2 moves: it is one new base URL.
//
// This unit answers exactly one question -- "which station on my AP is that
// forwarder, and what base do I speak the contract at" -- and it answers it
// WITHOUT putting the boxId on the wire before a candidate has proved it is the
// forwarder.
//
// THE CAPABILITY URL IS THE SECRET, AND THE PROBE IS WHERE IT LEAKS. Mailbox
// reads are unauthenticated and protected only by the unguessable boxId in the
// path (contract section 2), so discovering the proxy with
// GET {candidate}/m/{boxId}/latest.txt would hand the read capability for every
// note and every book to whichever device happens to hold 192.168.4.2 -- on an
// open AP, possibly a stranger's phone, unprompted. That is A3's sharpest security
// finding. The fix is structural here rather than advisory: discoverBase() is the
// only way to obtain a peer base, and it returns one only for a candidate that
// answered the boxId-free health path, so a capability-bearing request cannot be
// sent to an unproven station by construction.
//
// WHAT THIS DOES NOT FIX, stated so nobody mistakes it for enough. A station that
// answers the health path can still impersonate the forwarder and serve a frame or
// an epub of its choosing. The staging gates bound the damage -- a frame must match
// the live panel buffer size exactly, a book must match the manifest's bytes -- but
// the manifest comes from the same peer, so "a book the user did not send" is
// reachable. The other half of A3's finding is a per-device PSK on the AP
// (AP_PASSWORD is a compile-time nullptr today, i.e. an open network), which is a
// provisioning-side change and lives outside this unit. It is REQUIRED before any
// unattended variant of the peer path; the foreground mode is watched by the user
// who started it, which is the only reason it can ship first.
//
// Plain HTTP, deliberately: TLS terminates on the phone, which has a real stack, a
// CA store and keep-alive. There is no handshake to pay on the peer link, which is
// what makes a ~4 s poll affordable there for the first time (contract section 5
// G6 is a constraint on the internet transport only). Nothing on the reader is
// reachable from the peer link in return -- the mode raises the AP WITHOUT the web
// server, so there is no upload, delete, rename or WS handler listening.
namespace PeerProbe {

// Fixed port the app binds its forwarder to on the peer interface.
constexpr uint16_t PROXY_PORT = 8080;

// The boxId-free health path, and the token a real forwarder answers with. The
// reader accepts exactly HEALTH_TOKEN or HEALTH_TOKEN + "/<anything>", so the app
// can version its forwarder ("cp-proxy/1") without a firmware change.
//
// NOTE FOR THE APP SIDE: A3 specifies this path but only says "a known string" for
// the body. This is the reader half pinning it. The forwarder must answer
// GET /cp-proxy with a body whose first non-whitespace token is "cp-proxy"
// (recommended: exactly "cp-proxy/1", text/plain) and must NOT require any header.
constexpr char HEALTH_PATH[] = "/cp-proxy";
constexpr char HEALTH_TOKEN[] = "cp-proxy";

// Per-candidate wall-clock budget. Plain HTTP over a one-hop link with no route
// anywhere: a real forwarder answers in milliseconds, a dead address fails on
// connect, and the whole candidate space is AP_MAX_CONNECTIONS wide.
constexpr uint32_t CANDIDATE_BUDGET_MS = 1500;

// Largest health body the reader will accept. A real answer is ~10 bytes; a
// candidate that sends more is either not the forwarder or is trying to make the
// reader buffer a stream, so the fetch is aborted and the candidate rejected.
constexpr size_t MAX_HEALTH_BYTES = 64;

// True iff `origin` ("http://host:port", no trailing slash) answered HEALTH_PATH
// with HEALTH_TOKEN inside `deadline` (absolute millis(), 0 = unbounded, which no
// caller should want -- this is a blocking call). Sends no boxId and no
// credentials: safe to point at a station that may be a stranger's phone.
bool isProxy(const std::string& origin, uint32_t deadline);

// Probe this AP's DHCP range for the forwarder and compose the base to speak the
// mailbox contract at. Returns "" when no station answered, when the deadline is
// spent, or when `configuredBase` has no path to forward.
//
// The result is `http://{peerIp}:{port}` + pathOf(configuredBase), because the
// forwarder passes /m/* through VERBATIM: one capability URL, one budget, no
// second settings field (contract section 6).
//
// Candidates are derived from WiFi.softAPIP() rather than a hardcoded 192.168.4.x:
// the firmware never calls softAPConfig, so that subnet is the IDF's default to
// change, not the reader's to promise. `deadline` is absolute millis() and bounds
// the whole sweep; each candidate additionally gets at most CANDIDATE_BUDGET_MS.
//
// CALLER'S JOB: wait for WiFi.softAPgetStationNum() > 0 before calling, and bound
// that wait. Probing an AP no phone has joined yet is four guaranteed failures.
std::string discoverBase(const std::string& configuredBase, uint32_t deadline, uint16_t port = PROXY_PORT);

// Path portion of a mailbox base URL, trailing slashes stripped:
// "https://host/m/{boxId}" -> "/m/{boxId}", "https://host" -> "". The view aliases
// `url`, so it must not outlive it.
//
// Kept header-inline and constexpr so the static_asserts below are a compile-time
// gate on it: this is the one string operation in A3 that can silently produce a
// base that works (the forwarder answers) but is wrong (it forwards to the wrong
// mailbox path), and there is no runtime signal for that.
constexpr std::string_view pathOf(std::string_view url) {
  const size_t schemeEnd = url.find("://");
  const size_t authorityStart = (schemeEnd == std::string_view::npos) ? 0 : schemeEnd + 3;
  const size_t slash = url.find('/', authorityStart);
  if (slash == std::string_view::npos) return {};
  std::string_view path = url.substr(slash);
  while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
  if (path == "/") return {};
  return path;
}

static_assert(pathOf("https://mailbox.example.com/m/01JQ8ZK4T2") == "/m/01JQ8ZK4T2");
static_assert(pathOf("https://mailbox.example.com/m/01JQ8ZK4T2/") == "/m/01JQ8ZK4T2");
static_assert(pathOf("https://mailbox.example.com/m/01JQ8ZK4T2///") == "/m/01JQ8ZK4T2");
static_assert(pathOf("http://10.0.0.9:8787/m/abc") == "/m/abc");
// No path to forward: the peer base would be a bare origin and every request
// would miss the forwarder's /m/* route. discoverBase() refuses these.
static_assert(pathOf("https://mailbox.example.com").empty());
static_assert(pathOf("https://mailbox.example.com/").empty());
static_assert(pathOf("").empty());
// The port colon must never be mistaken for a path separator, and the scheme's
// own "//" must never be mistaken for the start of the path.
static_assert(pathOf("http://192.168.4.2:8080").empty());
// Query and fragment are carried through untouched, which is the same naivety the
// internet path already has (it composes base + "/latest.txt" with no parse). A
// mailbox base is an origin plus /m/{boxId} and never carries either; this asserts
// the behaviour rather than endorsing such a base.
static_assert(pathOf("https://h/a/b?x=1#y") == "/a/b?x=1#y");

}  // namespace PeerProbe
