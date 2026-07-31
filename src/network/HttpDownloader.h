#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files. The transport
 * is chosen from the URL scheme; plain http is used for local servers.
 *
 * HTTPS IS NOT CERTIFICATE-VERIFIED ON ANY SHIPPING BUILD. Every env in
 * platformio.ini defines FREEINK_NET_WOLFSSL, so requests go through
 * SecureHttpClient with setInsecure() -- the verified esp_http_client/CA-bundle
 * path below it is dead code on those builds (SecureClient supports a single
 * pinned PEM root via setCACert, not a bundle). This header used to claim CA
 * verification outright, which was true only of the path nothing builds.
 *
 * WHAT THAT COSTS, so a caller can decide: an active MITM on any joined network
 * -- an evil twin of a saved SSID, a hostile hotspot, a captive portal -- sees
 * the full request URL and can substitute the response body. For OPDS that is a
 * public catalogue. For the mailbox the URL carries the boxId capability (read
 * access to every note and every book, unrevokable from the device) and the body
 * becomes a sleep-screen frame or an installed book, gated only by a size match
 * against a manifest fetched over the same channel. Fixing it properly means
 * shipping roots in flash or pinning the mailbox origin's issuer; until then this
 * is a known, documented residual rather than an assumption anyone should build
 * on.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
    // 416 Range Not Satisfiable: the requested offset is at or past the end of
    // the resource. Distinct from HTTP_ERROR on purpose -- a resuming caller
    // must correct its offset rather than retry the same range, whereas a
    // transient 5xx means "keep the partial and try again next window".
    RANGE_NOT_SATISFIABLE,
    // A Range was sent and the server answered 200 with the whole body, which is
    // legal (RFC 9110 lets a server ignore a Range it does not support). Not one
    // byte is delivered in this case: appending a full body at a resume offset
    // would produce a file that is corrupt but the right length on the next
    // window. The caller must restart from offset 0.
    RANGE_IGNORED,
  };

  /**
   * Fetch text content from a URL with optional credentials.
   *
   * `deadlineMs` is an absolute millis() timestamp (0 = none). Only pass a body
   * this small (a few hundred bytes) through here: the whole response is buffered
   * in the returned string.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "", uint32_t deadlineMs = 0);

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   *
   * `deadlineMs` is an absolute millis() timestamp (0 = none) enforced inside the
   * body read loop, so a caller working to a window budget is not left with only
   * the 60 s per-socket-op timeout as a bound.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "", uint32_t deadlineMs = 0);

  /**
   * DELETE a URL and report whether the server answered 2xx.
   *
   * For acks, not for transfers: no body is sent, and the response body is read
   * and thrown away up to a small cap (a server that answers an ack with a
   * stream is answering wrong, and the transfer is aborted rather than
   * buffered). The status is what the caller gets, because that is the whole
   * content of an acknowledgement.
   *
   * `deadlineMs` is an absolute millis() timestamp (0 = none), clamped into the
   * per-socket-op timeout exactly as the fetches above do -- an ack is issued
   * from inside somebody's window budget and must not be able to outlive it.
   */
  static bool deleteUrl(const std::string& url, uint32_t deadlineMs = 0);

  /**
   * Download a file to the SD card with optional credentials.
   *
   * Truncating and one-shot: destPath is removed before the transfer and again
   * on any failure. Callers resuming across several windows want resumeToFile()
   * instead -- for them the partial file IS the state and must never be dropped.
   *
   * `deadlineMs` is an absolute millis() timestamp (0 = none) enforced inside the
   * body read loop, so a caller working to a window budget is not left with only
   * the 60 s per-socket-op timeout as a bound. Independent of `cancelFlag`: a
   * caller that cannot poll (one blocking call on the input task) needs the
   * deadline, a caller with a UI loop wants both.
   *
   * `maxBytes` (0 = no cap) aborts the transfer the moment the body would exceed
   * it, and destPath is removed like any other failure. For a caller that knows
   * the only legal size up front -- a note frame IS the panel buffer size -- that
   * is a far tighter bound than the deadline: without it a server answering with
   * a stream gets to write to the SD card for the whole window.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      uint32_t deadlineMs = 0, size_t maxBytes = 0);

  struct RangeResult {
    DownloadError error = HTTP_ERROR;
    size_t bytesWritten = 0;  // appended by THIS call, not the file size
    // Size of the whole resource, taken from Content-Range on a 206/416 and
    // from Content-Length on a 200. 0 when the server reported neither.
    size_t resourceTotal = 0;
    int status = 0;  // final HTTP status (200 / 206 / 416 / ...), -1 on transport failure
  };

  /**
   * Resume a download by appending to destPath from byte `rangeStart`.
   *
   * Sends `Range: bytes=<rangeStart>-` when rangeStart > 0 and accepts both 206
   * and 200 (a server that ignores the header answers 200 with the whole body,
   * which is legal -- the caller must then restart from 0). destPath is NEVER
   * removed by this function on any outcome: for a multi-window resume the partial
   * file is the only record of progress.
   *
   * rangeStart > 0 opens O_WRITE|O_CREAT and seeks, so the bytes already there are
   * preserved. rangeStart == 0 means "start over" and adds O_TRUNC, so a stale
   * file longer than the new body cannot leave its tail behind for a later window
   * to mistake for progress.
   *
   * `deadlineMs` is an absolute millis() timestamp (0 = none) enforced inside
   * the body read loop, so it bounds a stalled socket too -- the 60 s
   * per-socket-op timeout is not a usable window bound on its own.
   */
  static RangeResult resumeToFile(const std::string& url, const std::string& destPath, size_t rangeStart,
                                  uint32_t deadlineMs = 0, ProgressCallback progress = nullptr,
                                  bool* cancelFlag = nullptr, const std::string& username = "",
                                  const std::string& password = "");
};
