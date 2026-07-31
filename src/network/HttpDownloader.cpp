#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <base64.h>

#include <cstdlib>
#include <functional>
#include <string>

#if defined(FREEINK_NET_WOLFSSL)
#include <SecureHttpClient.h>

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }
#else
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#endif

namespace {
#if !defined(FREEINK_NET_WOLFSSL)
// RX holds the response headers. Smaller buffers leave enough contiguous heap
// for mbedTLS on redirect-heavy OPDS feeds while still preserving the headers
// we read directly (Location, Content-Length).
constexpr int HTTP_RX_BUF = 2048;
constexpr int HTTP_TX_BUF = 512;
#endif
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr uint32_t HTTP_TIMEOUT_MS = 60000;
// Floor for a timeout clamped against a caller deadline. SecureHttpClient divides
// its timeout by 1000 for the connect phase (SecureHttpClient.h:403-408), so a
// sub-second value rounds to a 0 s connect timeout, which the underlying
// WiFiClient reads as "fail immediately" rather than "no timeout".
constexpr uint32_t MIN_SOCKET_TIMEOUT_MS = 1000;
constexpr size_t READ_CHUNK = 1024;
constexpr int MAX_REDIRECTS = 5;

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  // Absolute millis() deadline for the whole transfer, 0 = unbounded. Polled in
  // the same place as cancelFlag, which is inside the body read loop, so it
  // bounds a socket that has stopped delivering bytes as well as a slow one.
  uint32_t deadlineMs = 0;
  // > 0 sends "Range: bytes=<rangeStart>-".
  size_t rangeStart = 0;
  // Take a 206 body, and report a 416 / an ignored Range distinctly instead of
  // collapsing them into a generic failure.
  bool acceptPartial = false;
  // Size of the whole resource when the server reports one, else the body length.
  size_t total = 0;
  // Bytes handed to write() by THIS call.
  size_t downloaded = 0;
  // Added to `downloaded` when reporting progress: the resume offset.
  size_t progressBase = 0;
  int status = 0;
  bool headersRead = false;
  // A Range was sent and the server answered 200 with the whole body.
  bool rangeIgnored = false;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Scheme + authority of a URL, with the path dropped: "https://host:port".
//
// THE PATH IS A SECRET ON THIS DEVICE. A mailbox URL is
// https://host/m/{boxId}/... and the boxId is the bearer capability for every
// note and every book (contract section 2: reads are unauthenticated, protected
// by the unguessable path). ERR is compiled into every shipping build, the
// status<0 branch below fires on any ordinary transport failure, every logged
// line lands in the RTC_NOINIT ring, and HalSystem dumps that ring verbatim into
// /crash_report.txt -- a file the device's own UI asks the user to attach to a
// bug report and which GET /download serves to anyone on the open transfer-mode
// AP. So the failing-fetch log gets the authority only; the full URL stays at
// DBG, which release builds compile out. Same authority-vs-path split as
// PeerProbe::pathOf and MailboxSyncActivity::originOf.
std::string authorityOf(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  const size_t authorityStart = (schemeEnd == std::string::npos) ? 0 : schemeEnd + 3;
  const size_t slash = url.find('/', authorityStart);
  return slash == std::string::npos ? url : url.substr(0, slash);
}

bool sinkExpired(const Sink& sink) {
  if (sink.cancelFlag && *sink.cancelFlag) return true;
  return sink.deadlineMs != 0 && static_cast<int32_t>(millis() - sink.deadlineMs) >= 0;
}

// Per-socket-op timeout for one request, clamped to what is left of the caller's
// window. sinkExpired() bounds the body read loop, but NOTHING polls it during
// connect: SecureHttpClient checks the abort callback once, immediately before
// ensureConnected() (SecureHttpClient.h:211), and the connect itself does not
// poll. So for the connect phase the socket timeout is the only bound there is,
// and a fixed 60 s is not a usable one inside an 8 s window (contract section 5
// gap G7: "do not rely on the socket timeout as a window bound").
//
// What this actually reaches: the status-line/header read deadline and the
// inter-read stall deadline in every body reader, plus the TCP connect on the
// plain-http path (_plain is a WiFiClient, whose setTimeout does bound connect).
// It does NOT reach the wolfSSL handshake -- SecureClient does not override
// setTimeout and caps its own handshake at an internal 15 s per method attempt
// (SecureClient.cpp) -- so https adds that much irreducible slop past the
// deadline on a connect that hangs. Never returns 0: see MIN_SOCKET_TIMEOUT_MS.
uint32_t socketTimeoutFor(const Sink& sink) {
  if (sink.deadlineMs == 0) return HTTP_TIMEOUT_MS;
  const int32_t remaining = static_cast<int32_t>(sink.deadlineMs - millis());
  if (remaining <= 0) return MIN_SOCKET_TIMEOUT_MS;
  const uint32_t budget = static_cast<uint32_t>(remaining);
  if (budget >= HTTP_TIMEOUT_MS) return HTTP_TIMEOUT_MS;
  return budget < MIN_SOCKET_TIMEOUT_MS ? MIN_SOCKET_TIMEOUT_MS : budget;
}

// Total size out of a Content-Range value. Handles both forms the contract can
// emit: "bytes 655360-1874232/1874233" (206) and "bytes */1874233" (416).
// Returns 0 for an absent header or an unknown ("*") total.
size_t contentRangeTotal(const std::string& value) {
  const size_t slash = value.rfind('/');
  if (slash == std::string::npos || slash + 1 >= value.size()) return 0;
  if (value[slash + 1] == '*') return 0;
  return static_cast<size_t>(strtoul(value.c_str() + slash + 1, nullptr, 10));
}

// START offset out of a 206's Content-Range ("bytes 655360-1874232/1874233"),
// or SIZE_MAX when the header is absent/unparseable.
//
// The total was being read and the start ignored, so a server or middlebox that
// answered "bytes=N-" with a slice starting somewhere ELSE had its bytes appended
// at the resume offset regardless. The result is a file that is corrupt in the
// middle yet exactly the length the manifest promises -- and a size match is the
// only integrity gate the contract affords, so nothing downstream catches it.
size_t contentRangeStart(const std::string& value) {
  const size_t space = value.find(' ');
  if (space == std::string::npos || space + 1 >= value.size()) return SIZE_MAX;
  const char* p = value.c_str() + space + 1;
  if (*p < '0' || *p > '9') return SIZE_MAX;  // "*" (a 416) has no start
  char* end = nullptr;
  const unsigned long start = strtoul(p, &end, 10);
  if (end == p || end == nullptr || *end != '-') return SIZE_MAX;
  return static_cast<size_t>(start);
}

#if defined(FREEINK_NET_WOLFSSL)
HttpDownloader::DownloadError runGetWolf(const std::string& startUrl, const std::string& username,
                                         const std::string& password, Sink& sink) {
  std::string url = startUrl;

  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    // Checked before the connect, not only inside the body loop: a redirect chain
    // pays a fresh connect per hop and each one is unabortable once started.
    if (sinkExpired(sink)) return HttpDownloader::ABORTED;
    freeink::SecureHttpClient http;
    http.setTimeout(socketTimeoutFor(sink));
    // NO CERTIFICATE VERIFICATION -- inherited, load-bearing, and documented as a
    // residual in HttpDownloader.h. wolfSSL here has no CA bundle wired up
    // (SecureClient offers a single pinned PEM root), so every https fetch on a
    // shipping build, including the mailbox capability URL, is MITM-able on a
    // hostile network. Do not read the runGet() comment below as describing this
    // path: that branch is dead on every env in platformio.ini.
    http.setInsecure();
    if (!http.begin(url)) {
      LOG_ERR("HTTP", "wolfSSL bad URL at %s", authorityOf(url).c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would
    // append a second User-Agent header, which strict servers reject (aiohttp
    // answers 400 "Duplicate 'User-Agent' header found").
    http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
    if (!username.empty() && !password.empty()) {
      const std::string credentials = username + ":" + password;
      const String encoded = base64::encode(credentials.c_str());
      http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
    }
    // begin() clears the request headers, so Range must be added after it. Added
    // inside the hop loop deliberately: a redirected resume must still be ranged.
    if (sink.rangeStart > 0) {
      http.addHeader("Range", "bytes=" + std::to_string(sink.rangeStart) + "-");
    }

    LOG_DBG("HTTP", "wolfSSL GET: %s", url.c_str());
    const int status = http.GET(
        [&http, &sink](const uint8_t* data, size_t len) {
          const int bodyStatus = http.getStatus();
          // 206 carries payload for a resuming caller; anything else non-200 is
          // an error body and must not reach the sink (it would corrupt the
          // file the caller is appending to).
          if (bodyStatus != 200 && !(sink.acceptPartial && bodyStatus == 206)) return true;
          // A 200 answer to a ranged request means the server ignored the Range.
          // The sink is positioned at the resume offset, so writing the full body
          // here yields a file that is corrupt yet plausibly sized. Stop before
          // the first byte and let the caller restart from 0 -- draining a whole
          // epub we cannot use would burn the window for nothing.
          if (bodyStatus == 200 && sink.rangeStart > 0) {
            sink.rangeIgnored = true;
            return false;
          }
          if (!sink.headersRead) {
            sink.headersRead = true;
            // On a 206 Content-Length is the SLICE length, not the file size, so
            // a resume at 90% would otherwise report progress starting from 0.
            if (sink.acceptPartial) {
              const std::string contentRange = http.getHeader("content-range");
              if (bodyStatus == 206) {
                // A slice that does not start where we asked is not usable at the
                // offset the file is positioned at. Reported as "the server
                // ignored the Range" because the recovery is identical: the
                // caller restarts the file from 0. Stopped before the first byte,
                // so the partial on the card is left exactly as it was.
                const size_t start = contentRangeStart(contentRange);
                if (start != sink.rangeStart) {
                  sink.rangeIgnored = true;
                  return false;
                }
              }
              sink.total = contentRangeTotal(contentRange);
            }
            if (sink.total == 0 && http.hasContentLength()) {
              sink.total = http.getContentLength();
              // Content-Range was absent or unparseable and we fell back to
              // Content-Length. On a 206 that is the slice, and the range is
              // always open-ended ("bytes=N-"), so the resource size is exactly
              // rangeStart + slice. Without this correction the caller sees a
              // resource "smaller than the manifest says" and, in BookSync, that
              // means "the blob was replaced under this id" -- it deletes every
              // byte fetched so far, on every window. Mirrors the esp_http_client
              // path (runGet below), which has always done this.
              if (bodyStatus == 206 && sink.total > 0) sink.total += sink.rangeStart;
            }
          }
          if (!sink.write(data, len)) return false;
          sink.downloaded += len;
          if (sink.progress && sink.total > 0) sink.progress(sink.progressBase + sink.downloaded, sink.total);
          return true;
        },
        [&sink]() { return sinkExpired(sink); });

    // Captured before the aborted()/status gates below so a caller that hit its
    // deadline mid-body still learns the status and the resource size.
    sink.status = status;
    if (sink.acceptPartial && sink.total == 0) {
      sink.total = contentRangeTotal(http.getHeader("content-range"));
    }

    if (http.aborted()) return HttpDownloader::ABORTED;
    if (status < 0) {
      LOG_ERR("HTTP", "wolfSSL request failed at %s", authorityOf(url).c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    if (isRedirect(status)) {
      const std::string location = http.getHeader("location");
      if (location.empty() || !freeink::SecureHttpClient::resolveUrl(url, location, url)) {
        LOG_ERR("HTTP", "wolfSSL bad redirect: %d", status);
        return HttpDownloader::HTTP_ERROR;
      }
      continue;
    }
    if (status != 200 && !(sink.acceptPartial && status == 206)) {
      if (sink.acceptPartial && status == 416) {
        LOG_DBG("HTTP", "wolfSSL 416 at offset %zu (total %zu)", sink.rangeStart, sink.total);
        return HttpDownloader::RANGE_NOT_SATISFIABLE;
      }
      LOG_ERR("HTTP", "wolfSSL unexpected status: %d", status);
      return HttpDownloader::HTTP_ERROR;
    }
    // Checked before callbackAborted(): the callback is what stopped the body,
    // and "the server does not do ranges" is not a file error.
    if (sink.rangeIgnored) {
      LOG_ERR("HTTP", "wolfSSL server ignored Range at offset %zu", sink.rangeStart);
      return HttpDownloader::RANGE_IGNORED;
    }
    if (http.callbackAborted()) return HttpDownloader::FILE_ERROR;
    if (!http.responseComplete()) {
      LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      return HttpDownloader::HTTP_ERROR;
    }
    return HttpDownloader::OK;
  }
  LOG_ERR("HTTP", "too many redirects");
  return HttpDownloader::HTTP_ERROR;
}
#endif

#if !defined(FREEINK_NET_WOLFSSL)
// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  // Clamped to the caller's remaining window for the same reason as the wolfSSL
  // path: open() is not abortable, so the socket timeout is its only bound.
  config.timeout_ms = static_cast<int>(socketTimeoutFor(sink));
  // Verify HTTPS against the bundled CA roots. NOTE: this whole function is dead
  // code on every shipping env (they all define FREEINK_NET_WOLFSSL and dispatch
  // to runGetWolf, which does NOT verify), so this is the verification story of a
  // build nobody ships -- see the residual documented in HttpDownloader.h.
  // Where it does apply: esp-tls CONFIG_ESP_TLS_INSECURE is off, so an unverified
  // TLS handshake cannot be set up at all; the model is public servers over
  // verified https and local servers over plain http (esp_http_client picks the
  // transport from the URL scheme, so http:// needs no cert config).
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (!username.empty() && !password.empty()) {
    // Preemptive Basic auth, like the prior addHeader; don't wait for a 401.
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }
  if (sink.rangeStart > 0) {
    const std::string range = "bytes=" + std::to_string(sink.rangeStart) + "-";
    esp_http_client_set_header(client, "Range", range.c_str());
  }

  // open()/read() does not auto-follow redirects (only perform() does), so step
  // 30x responses manually. OPDS download endpoints and the GitHub release CDN
  // both redirect.
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }
  int64_t contentLength = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  for (int hop = 0; isRedirect(status) && hop < MAX_REDIRECTS; ++hop) {
    if (esp_http_client_set_redirection(client) != ESP_OK) break;
    esp_http_client_close(client);
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "redirect open failed: %s", esp_err_to_name(err));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
  }

  // Dead code on every shipping env (all of them define FREEINK_NET_WOLFSSL and
  // dispatch to runGetWolf), kept in step with it so the two paths cannot answer
  // a resume differently. NOT covered by the compile gate.
  sink.status = status;
  if (status != 200 && !(sink.acceptPartial && status == 206)) {
    if (sink.acceptPartial && status == 416) {
      esp_http_client_cleanup(client);
      return HttpDownloader::RANGE_NOT_SATISFIABLE;
    }
    LOG_ERR("HTTP", "unexpected status: %d", status);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }
  if (sink.rangeStart > 0 && status == 200) {
    LOG_ERR("HTTP", "server ignored Range at offset %zu", sink.rangeStart);
    esp_http_client_cleanup(client);
    return HttpDownloader::RANGE_IGNORED;
  }

  // fetch_headers returns 0 for a chunked response (no Content-Length); leave
  // total at 0 so progress stays silent and the size check is skipped. On a 206
  // the length is the slice, and the range is always open-ended ("bytes=N-"),
  // so the resource total is exactly rangeStart + slice.
  sink.total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;
  if (status == 206 && sink.total > 0) sink.total += sink.rangeStart;

  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer", (unsigned)READ_CHUNK);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  while (true) {
    if (sinkExpired(sink)) {
      esp_http_client_cleanup(client);
      return HttpDownloader::ABORTED;
    }
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (read == 0) break;  // all data received
    if (!sink.write(reinterpret_cast<const uint8_t*>(buf.get()), read)) {
      esp_http_client_cleanup(client);
      return HttpDownloader::FILE_ERROR;
    }
    sink.downloaded += read;
    if (sink.progress && sink.total > 0) sink.progress(sink.progressBase + sink.downloaded, sink.total);
  }

  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);
  if (!complete) {
    LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}
#endif  // !FREEINK_NET_WOLFSSL

// All HTTP(S) fetches go through wolfSSL when it is the active TLS stack: it
// speaks TLS 1.3 and reads large bodies from servers where the esp_http_client/
// mbedTLS path fails to connect or stalls mid-stream. Plain-http URLs still use a
// WiFiClient inside runGetWolf, so this is safe for non-TLS targets too.
HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password, Sink& sink) {
#if defined(FREEINK_NET_WOLFSSL)
  return runGetWolf(url, username, password, sink);
#else
  return runGet(url, username, password, sink);
#endif
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGetSecure(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password, uint32_t deadlineMs) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.deadlineMs = deadlineMs;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGetSecure(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password, uint32_t deadlineMs) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  sink.deadlineMs = deadlineMs;
  return runGetSecure(url, username, password, sink) == OK;
}

bool HttpDownloader::deleteUrl(const std::string& url, const uint32_t deadlineMs) {
  LOG_DBG("HTTP", "DELETE: %s", url.c_str());
  // Only deadlineMs is set: nothing here writes to a sink, and socketTimeoutFor()
  // reads no other field. Reused rather than reimplemented so an ack is clamped by
  // exactly the same rule as a fetch issued from the same window.
  Sink sink;
  sink.deadlineMs = deadlineMs;
  if (sinkExpired(sink)) return false;

  // An ack's answer is its status; the body is noise. Capped so a peer that
  // answers a DELETE with a stream costs one aborted read rather than the heap.
  constexpr size_t MAX_ACK_BODY = 256;

#if defined(FREEINK_NET_WOLFSSL)
  freeink::SecureHttpClient http;
  http.setTimeout(socketTimeoutFor(sink));
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("HTTP", "wolfSSL bad URL at %s", authorityOf(url).c_str());
    return false;
  }
  http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);

  size_t drained = 0;
  const int status = http.sendRequest(
      "DELETE", nullptr, 0,
      [&drained](const uint8_t*, const size_t len) {
        drained += len;
        return drained <= MAX_ACK_BODY;
      },
      [&sink]() { return sinkExpired(sink); });
  if (http.aborted()) return false;
  return status >= 200 && status < 300;
#else
  // Dead code on every shipping env (all define FREEINK_NET_WOLFSSL), kept in
  // step with the branch above. NOT covered by the compile gate.
  (void)MAX_ACK_BODY;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = static_cast<int>(socketTimeoutFor(sink));
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.method = HTTP_METHOD_DELETE;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    return false;
  }
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  const esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "DELETE open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }
  esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  return status >= 200 && status < 300;
#endif
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             uint32_t deadlineMs, size_t maxBytes) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.deadlineMs = deadlineMs;
  size_t written = 0;
  bool overLimit = false;
  sink.write = [&file, &written, &overLimit, maxBytes](const uint8_t* data, const size_t len) {
    if (maxBytes != 0 && written + len > maxBytes) {
      // Refused before the write, so not one byte over the cap reaches the card.
      // Returning false stops the transfer; the caller removes destPath below.
      overLimit = true;
      return false;
    }
    if (file.write(data, len) != len) return false;
    written += len;
    return true;
  };

  const DownloadError result = runGetSecure(url, username, password, sink);
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  file.close();

  if (result != OK) {
    if (overLimit) {
      LOG_ERR("HTTP", "body over the %zu byte cap; aborted", maxBytes);
    }
    Storage.remove(destPath.c_str());
    return result;
  }
  if (sink.downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", sink.downloaded);
  return OK;
}

HttpDownloader::RangeResult HttpDownloader::resumeToFile(const std::string& url, const std::string& destPath,
                                                        size_t rangeStart, uint32_t deadlineMs,
                                                        ProgressCallback progress, bool* cancelFlag,
                                                        const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Resuming: %s -> %s from %zu", url.c_str(), destPath.c_str(), rangeStart);
  RangeResult out;

  // openFileForWrite() is hardcoded O_RDWR|O_CREAT|O_TRUNC, so the first write of
  // a resumed download would discard everything already fetched. Use the raw
  // oflag path and seek explicitly instead of relying on an append flag -- the
  // SdFat oflag set is not vendored here, so only the flags with in-tree
  // precedent (HalSystem.cpp, Dictionary.cpp) are used.
  //
  // O_TRUNC exactly when rangeStart == 0, i.e. when the caller means "start this
  // file over". Without it a stale file LONGER than the new body keeps its tail:
  // only the prefix is overwritten, a later window reads the stale length via the
  // caller's size probe, resumes from it, arrives at exactly the advertised size
  // and promotes a file whose interior is garbage. The size gate is the only
  // integrity gate the contract affords (no hash), so nothing downstream catches
  // that. Reachable whenever a caller believes the partial is gone when it is not
  // -- an unchecked remove(), or a size probe that returns 0 on a read failure.
  const oflag_t oflag = rangeStart == 0 ? (O_WRITE | O_CREAT | O_TRUNC) : (O_WRITE | O_CREAT);
  HalFile file = Storage.open(destPath.c_str(), oflag);
  if (!file) {
    LOG_ERR("HTTP", "Failed to open %s for resume", destPath.c_str());
    out.error = FILE_ERROR;
    return out;
  }
  if (rangeStart > 0 && !file.seekSet(rangeStart)) {
    LOG_ERR("HTTP", "Failed to seek %s to %zu", destPath.c_str(), rangeStart);
    file.close();
    out.error = FILE_ERROR;
    return out;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.deadlineMs = deadlineMs;
  sink.rangeStart = rangeStart;
  sink.acceptPartial = true;
  sink.progressBase = rangeStart;
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };

  out.error = runGetSecure(url, username, password, sink);
  // Close before the caller can rename or re-open the path (DESTRUCTOR_CLOSES_FILE
  // would otherwise close only once `file` leaves scope).
  file.close();
  out.bytesWritten = sink.downloaded;
  out.resourceTotal = sink.total;
  out.status = sink.status;

  // Deliberately no remove() on ANY path, including failure and a zero-byte
  // body: for a multi-window resume the partial file is the only progress state
  // there is, and a transport failure is expected to be retried, not restarted.
  LOG_DBG("HTTP", "Resume result %d: +%zu bytes (status %d, total %zu)", static_cast<int>(out.error),
          out.bytesWritten, out.status, out.resourceTotal);
  return out;
}
