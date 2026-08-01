#include "PeerProbe.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_ap_get_sta_list.h>

#include "network/HttpDownloader.h"

namespace {
// The IDF's default softAP DHCP pool starts one past the AP itself, and
// AP_MAX_CONNECTIONS (CrossPointWebServerActivity.cpp) caps the pool at four
// leases -- so this is the ENTIRE candidate space, not a sample of it.
constexpr uint8_t FIRST_LEASE_OFFSET = 1;
constexpr uint8_t MAX_LEASES = 4;

// Absolute deadline `budget` ms from now, never past `cap` (absolute, 0 = uncapped).
// Same shape as MessageSync's: the per-candidate budget must never outlive the
// sweep's own bound.
uint32_t deadlineWithin(uint32_t cap, uint32_t budget) {
  const uint32_t want = millis() + budget;
  if (cap == 0) return want;
  return static_cast<int32_t>(want - cap) > 0 ? cap : want;
}

bool expired(uint32_t deadline) { return deadline != 0 && static_cast<int32_t>(millis() - deadline) >= 0; }

// The addresses the reader's own DHCP server has actually handed out, written to
// `out` (at most `max`) and returned by count. 0 means "no usable lease table",
// which is the caller's signal to sweep the whole range instead -- an empty
// answer is never treated as "nobody is here".
//
// WHY THIS EXISTS. Probing an address no station holds is not a fast failure: on
// a local subnet lwIP ARPs for it, nothing replies, and the connect waits out the
// whole per-candidate budget. With four addresses in the range and one phone on
// the link, three quarters of every sweep was spent waiting for machines that do
// not exist -- and a phone that landed on a later lease paid all of it before its
// own probe was sent. Asking the DHCP server which addresses are live turns that
// into one probe.
//
// FAILING BACK IS THE ONLY SAFE FAILURE. esp_wifi_ap_get_sta_list_with_ip is
// documented for the default AP interface with the lwIP DHCP server enabled;
// anything else (a station associated but with no lease yet, an error, a build
// without CONFIG_LWIP_DHCPS) yields 0 here and the caller sweeps the range
// exactly as it always did.
//
// The two IDF list structs are ESP_WIFI_MAX_CONN_NUM entries wide -- a few
// hundred bytes of stack -- so they live in this function and are gone before the
// caller starts blocking on sockets.
uint8_t leasedCandidates(const IPAddress& ap, IPAddress* out, const uint8_t max) {
  wifi_sta_list_t stations{};
  if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK) return 0;
  if (stations.num <= 0) return 0;

  wifi_sta_mac_ip_list_t leases{};
  if (esp_wifi_ap_get_sta_list_with_ip(&stations, &leases) != ESP_OK) return 0;

  uint8_t count = 0;
  for (int i = 0; i < leases.num && count < max; ++i) {
    // esp_ip4_addr_t.addr is lwIP's network byte order, i.e. the first octet is
    // the low byte. Unpacked by hand rather than through IPAddress(uint32_t) so
    // the byte order this depends on is written down where it is relied upon.
    const uint32_t raw = leases.sta[i].ip.addr;
    if (raw == 0) continue;  // associated, but the DHCP handshake has not landed
    const IPAddress ip(static_cast<uint8_t>(raw & 0xFF), static_cast<uint8_t>((raw >> 8) & 0xFF),
                       static_cast<uint8_t>((raw >> 16) & 0xFF), static_cast<uint8_t>((raw >> 24) & 0xFF));

    // A lease off this AP's own subnet, or one that collides with the AP itself,
    // is not something to point a capability-free probe at -- and neither can be
    // produced by a healthy DHCP server, so this is a sanity gate rather than a
    // security one (the security gate is the health probe itself).
    if (ip[0] != ap[0] || ip[1] != ap[1] || ip[2] != ap[2]) continue;
    if (ip[3] == ap[3]) continue;

    bool duplicate = false;
    for (uint8_t j = 0; j < count; ++j) {
      if (out[j] == ip) duplicate = true;
    }
    if (!duplicate) out[count++] = ip;
  }
  return count;
}

// First non-whitespace token of `body`, so a forwarder may answer with or without
// a trailing newline and with or without a version suffix.
std::string_view firstToken(std::string_view body) {
  const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!body.empty() && isSpace(body.front())) body.remove_prefix(1);
  size_t end = 0;
  while (end < body.size() && !isSpace(body[end])) ++end;
  return body.substr(0, end);
}
}  // namespace

bool PeerProbe::isProxy(const std::string& origin, uint32_t deadline) {
  if (origin.empty()) return false;

  std::string body;
  bool overflowed = false;
  // The bounded-body overload, not the std::string one: that buffers the WHOLE
  // response, and the thing being asked here may be a stranger's phone rather
  // than the app. Returning false from the sink aborts the transfer, which
  // fetchUrl reports as a failure -- exactly the answer wanted for a candidate
  // that will not answer in 64 bytes.
  const bool ok = HttpDownloader::fetchUrl(
      origin + HEALTH_PATH,
      [&body, &overflowed](const uint8_t* data, size_t len) {
        if (body.size() + len > MAX_HEALTH_BYTES) {
          overflowed = true;
          return false;
        }
        body.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      "", "", deadline);
  if (!ok) {
    LOG_DBG("PEER", "%s: no health answer%s", origin.c_str(), overflowed ? " (body too large)" : "");
    return false;
  }

  const std::string_view token = firstToken(body);
  const std::string_view expect(HEALTH_TOKEN);
  const bool versioned =
      token.size() > expect.size() && token.compare(0, expect.size(), expect) == 0 && token[expect.size()] == '/';
  if (token != expect && !versioned) {
    LOG_DBG("PEER", "%s: not a proxy (health body '%.*s')", origin.c_str(), static_cast<int>(token.size()),
            token.data());
    return false;
  }
  LOG_INF("PEER", "Proxy at %s", origin.c_str());
  return true;
}

std::string PeerProbe::discoverBase(const std::string& configuredBase, uint32_t deadline, uint16_t port,
                                    uint32_t candidateBudget) {
  const std::string_view path = pathOf(configuredBase);
  if (path.empty()) {
    // Fail closed. A bare origin would make every proxied request miss the
    // forwarder's /m/* route, and the failure would look like "the phone is not
    // running the app" instead of "the configured mailbox URL has no box path".
    LOG_ERR("PEER", "configured base has no path to forward: '%s'", configuredBase.c_str());
    return std::string();
  }

  const IPAddress ap = WiFi.softAPIP();
  if (ap[0] == 0) {
    LOG_ERR("PEER", "no softAP address: AP not up");
    return std::string();
  }

  // THE WHOLE CANDIDATE SPACE IS SWEPT, not "the first responder wins", and the
  // sweep refuses to choose when more than one station answers as a forwarder.
  //
  // The health probe proves only that something on this AP speaks five lines of
  // HTTP containing the token "cp-proxy". Returning the first responder meant a
  // station that joined before the user's phone was silently PREFERRED to the
  // phone, and it then received the capability URL on every poll and got to
  // serve whatever frames and manifests it liked. Ambiguity is not something
  // this code can resolve -- there is nothing to tell the two apart -- so it
  // fails closed and the panel keeps saying "looking for the app", which is the
  // honest thing to show while a second device is squatting the link.
  //
  // THE CANDIDATE SPACE IS THE DHCP LEASE TABLE WHEN THERE IS ONE, and the whole
  // range only as a fallback. That is a LATENCY change and not a security one:
  // both lists are the same width (MAX_LEASES == AP_MAX_CONNECTIONS), the sweep
  // below is the same sweep either way, and narrowing to the leases removes
  // addresses that CANNOT be a forwarder -- never one that could -- so the
  // refusal above still sees every station that could answer. What it buys is
  // that a sweep no longer waits out a per-candidate budget on each of the three
  // addresses nobody holds, which is where nearly all of its wall clock went.
  IPAddress candidates[MAX_LEASES];
  uint8_t candidateCount = leasedCandidates(ap, candidates, MAX_LEASES);
  if (candidateCount == 0) {
    for (uint8_t i = 0; i < MAX_LEASES; ++i) {
      const int last = static_cast<int>(ap[3]) + FIRST_LEASE_OFFSET + i;
      if (last > 254) break;  // ran off the subnet; nothing left to probe
      candidates[candidateCount++] = IPAddress(ap[0], ap[1], ap[2], static_cast<uint8_t>(last));
    }
    LOG_DBG("PEER", "no DHCP lease list; sweeping %u addresses of the range",
            static_cast<unsigned>(candidateCount));
  }

  std::string found;
  for (uint8_t i = 0; i < candidateCount; ++i) {
    if (expired(deadline)) {
      LOG_DBG("PEER", "discovery deadline spent after %u candidates", static_cast<unsigned>(i));
      break;
    }

    std::string origin = "http://";
    origin += candidates[i].toString().c_str();
    origin += ":";
    origin += std::to_string(port);

    // boxId-free, every time, for every candidate -- see the security note in the
    // header. The capability path is only ever appended AFTER the sweep has
    // settled on exactly one answer.
    if (isProxy(origin, deadlineWithin(deadline, candidateBudget))) {
      if (!found.empty()) {
        LOG_ERR("PEER", "two stations answer as the forwarder; refusing to pick one");
        return std::string();
      }
      found = std::move(origin);
    }
  }

  if (!found.empty()) {
    // A deadline that cut the sweep short after one match is still a usable
    // answer: the alternative is refusing a link the user is standing in front
    // of because a lease that was never probed MIGHT have answered.
    found.append(path);
    return found;
  }

  LOG_DBG("PEER", "no proxy found on the peer link");
  return std::string();
}
