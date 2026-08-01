#include "BookSync.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdlib>
#include <string>

#include "network/HttpDownloader.h"

namespace {

// The library the file browser shows and the user owns. Nothing in this file
// removes a readable book from it -- see the removal audit above syncOnLink().
constexpr char BOOKS_DIR[] = "/books";

// Staging. Two independent reasons a partial epub is invisible to the library:
// the directory is dot-prefixed (FileBrowserActivity skips those unless
// SETTINGS.showHiddenFiles) AND the staging file is named after the book id, so
// it has no extension on the browser's allowlist. Belt and braces on purpose --
// a torn epub must never be openable.
constexpr char INCOMING_DIR[] = "/books/.incoming";

// Id-keyed record of what has been fetched. Dot-prefixed, so hidden from the
// browser too. Keyed by id and NEVER by filename: a finished book is moved out
// of /books into /read/ and may be renamed on collision, so a filename diff
// would re-download every book the user finishes, for ever.
constexpr char STATE_FILE[] = "/books/.mailbox-books-state";

constexpr char SUFFIX_MANIFEST[] = "/books.txt";
constexpr char SUFFIX_BOOK[] = "/books/";

// Server caps, mirrored so a malformed manifest line is refused before it can
// reach the filesystem or a size comparison (contract section 2).
constexpr size_t MAX_BOOKS = 20;
constexpr size_t MAX_BOOK_BYTES = 25165824;  // 24 MiB, the shared MAX_BOOK_BYTES
constexpr size_t MAX_ID_LEN = 64;
constexpr size_t MAX_FILENAME_LEN = 120;

// Worst legal manifest at those caps is 20 x (64 + 1 + 8 + 1 + 120 + 1) = 3900 B,
// so 8 KB is 2x the worst case and still a hard stop for a body off a network the
// reader does not control. The line cap is likewise 256 against a 194 B worst case.
constexpr size_t MANIFEST_MAX_BYTES = 8192;
constexpr size_t MANIFEST_MAX_LINE = 256;

// State ring. Larger than MAX_BOOKS on purpose: pruning an id the moment it drops
// off the manifest would make an evict-then-republish under the same id
// re-download. 64 ids is ~4.5 KB and costs nothing.
constexpr size_t STATE_MAX_ENTRIES = 64;
constexpr size_t STATE_MAX_BYTES = 5120;

// Suffix ceiling for a colliding destination name, matching
// buildReadFolderDestination's " (2)".." (99)".
constexpr int MAX_NAME_SUFFIX = 100;

struct ManifestEntry {
  std::string id;
  std::string filename;
  size_t bytes = 0;
};

int32_t remainingMs(uint32_t deadline) { return static_cast<int32_t>(deadline - millis()); }

// The book id charset from the contract, chosen so an id needs no escaping in a
// URL path segment. "." and ".." are refused separately: the id becomes a path
// component of the staging file.
bool validBookId(const std::string& id) {
  if (id.empty() || id.size() > MAX_ID_LEN) return false;
  if (id == "." || id == "..") return false;
  for (const char c : id) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                    c == '_' || c == '~' || c == '-';
    if (!ok) return false;
  }
  return true;
}

// The server sanitizes filenames, but the reader must not trust a manifest line
// it read over plain http on someone's hotspot. Rejecting rather than repairing
// is deliberate: a repaired name can lose its ".epub" tail, and a book that
// fails the browser's allowlist downloads fine and is then invisible for ever,
// which looks exactly like a sync bug.
bool validBookFilename(const std::string& name) {
  if (name.empty() || name.size() > MAX_FILENAME_LEN) return false;
  if (name.front() == '.') return false;  // no hidden files, no "." / ".."
  for (const char c : name) {
    // Printable ASCII only, so one character is one byte and a C string walk
    // cannot disagree with Content-Length about where the fields are.
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7E) return false;
    // Path separators: traversal, and a name that would escape /books.
    if (c == '/' || c == '\\') return false;
    // FAT-reserved. The server already replaces these with '_', so a legitimate
    // manifest never carries one; refusing is free and keeps them off the card.
    if (c == '"' || c == '*' || c == ':' || c == '<' || c == '>' || c == '?' || c == '|') return false;
  }
  return FsHelpers::hasEpubExtension(name);
}

// Size of the staging file, or 0 when there is nothing there. `ok` reports whether
// the answer is trustworthy: a file that exists but cannot be opened (a flaky
// card) reads as 0 bytes, and a caller that treats that as "no partial" would
// resume from 0 over a file whose tail is still on disk. resumeToFile() adds
// O_TRUNC at offset 0 so that is no longer corrupting, but the window is still
// wasted, so the caller bails instead.
size_t existingSize(const std::string& path, bool& ok) {
  ok = true;
  if (!Storage.exists(path.c_str())) return 0;
  HalFile file;
  if (!Storage.openFileForRead("BSYNC", path.c_str(), file)) {
    ok = false;
    return 0;
  }
  const size_t size = file.size();
  file.close();
  return size;
}

// Drop the staging file, and say so if it is still there. Every caller that
// discards a partial then goes on to assume `have == 0`; a remove() that silently
// failed would leave the next resume seeking into stale bytes, and the size gate
// (the only integrity gate the contract affords -- there is no hash) cannot tell
// the difference once the length lines up.
bool discardPartial(const std::string& path) {
  if (Storage.remove(path.c_str())) return true;
  if (!Storage.exists(path.c_str())) return true;  // already gone
  LOG_ERR("BSYNC", "cannot remove partial %s, deferring", path.c_str());
  return false;
}

// Whole state file, bounded. Newest entry first, so a truncating read keeps the
// entries that matter; the trailing partial line is dropped so a half-written id
// can never be matched.
std::string loadState() {
  std::string state;
  if (!Storage.exists(STATE_FILE)) return state;
  HalFile file;
  if (!Storage.openFileForRead("BSYNC", STATE_FILE, file)) return state;
  const size_t size = file.size();
  const size_t want = size > STATE_MAX_BYTES ? STATE_MAX_BYTES : size;
  if (want > 0) {
    state.resize(want);
    const int read = file.read(&state[0], want);
    state.resize(read > 0 ? static_cast<size_t>(read) : 0);
  }
  file.close();
  if (size > want) {
    const size_t lastEol = state.rfind('\n');
    state.erase(lastEol == std::string::npos ? 0 : lastEol + 1);
    LOG_ERR("BSYNC", "state file oversized (%u B), read first %u", static_cast<unsigned>(size),
            static_cast<unsigned>(state.size()));
  }
  return state;
}

// "{id} done" and "{id} skip" both mean "do not fetch this id". Only `done` is
// written here; `skip` is the contract's slot for a user-dismissed book and is
// honoured if some other writer ever puts one there.
bool stateHas(const std::string& state, const std::string& id) {
  size_t pos = 0;
  while (pos < state.size()) {
    const size_t eol = state.find('\n', pos);
    const size_t end = (eol == std::string::npos) ? state.size() : eol;
    if (end - pos > id.size() && state.compare(pos, id.size(), id) == 0 && state[pos + id.size()] == ' ') {
      return true;
    }
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }
  return false;
}

// Rewrite the whole file with the new id first, keeping at most STATE_MAX_ENTRIES
// lines. Called ONLY after a successful rename: an id recorded for a book that is
// not there marks it delivered and it is never retried.
bool recordDone(const std::string& id, const std::string& state) {
  std::string next = id;
  next += " done\n";
  size_t kept = 1;
  size_t pos = 0;
  while (pos < state.size() && kept < STATE_MAX_ENTRIES) {
    const size_t eol = state.find('\n', pos);
    const size_t end = (eol == std::string::npos) ? state.size() : eol;
    if (end > pos) {
      next.append(state, pos, end - pos);
      next += '\n';
      ++kept;
    }
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }

  HalFile file;
  if (!Storage.openFileForWrite("BSYNC", STATE_FILE, file)) {
    LOG_ERR("BSYNC", "cannot write state file");
    return false;
  }
  const size_t written = file.write(next.data(), next.size());
  file.close();
  if (written != next.size()) {
    LOG_ERR("BSYNC", "short state write: %u of %u", static_cast<unsigned>(written),
            static_cast<unsigned>(next.size()));
    return false;
  }
  return true;
}

// Streaming manifest scan. Holds ONE candidate, never a vector of entries: the
// window fetches at most one book, newest first, so the scan can stop at the
// first manifest id the state file has not seen. Bounded in bytes, in line
// length and in line count, and it aborts the transfer rather than growing.
struct ManifestScan {
  const std::string* state = nullptr;
  std::string line;
  size_t bytesSeen = 0;
  size_t linesSeen = 0;
  bool lineOverflow = false;
  bool stopped = false;  // we cut the body short on purpose; not a transport failure
  bool found = false;
  ManifestEntry target;

  // Returns true when the scan is finished and the body should stop.
  bool takeLine() {
    if (lineOverflow) {
      LOG_ERR("BSYNC", "manifest line too long, skipped");
      return false;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) return false;
    if (++linesSeen > MAX_BOOKS) {
      LOG_ERR("BSYNC", "manifest longer than %u entries, stopping", static_cast<unsigned>(MAX_BOOKS));
      return true;
    }

    // "{id} {bytes} {filename}": split at the first space, then at the next; the
    // filename is the rest of the line and may itself contain spaces.
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    const size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;

    ManifestEntry entry;
    entry.id = line.substr(0, sp1);
    entry.filename = line.substr(sp2 + 1);
    const std::string bytesText = line.substr(sp1 + 1, sp2 - sp1 - 1);

    char* parseEnd = nullptr;
    const unsigned long parsed = strtoul(bytesText.c_str(), &parseEnd, 10);
    if (bytesText.empty() || parseEnd != bytesText.c_str() + bytesText.size()) return false;
    entry.bytes = static_cast<size_t>(parsed);

    if (!validBookId(entry.id)) {
      LOG_ERR("BSYNC", "manifest: bad id, skipped");
      return false;
    }
    if (entry.bytes == 0 || entry.bytes > MAX_BOOK_BYTES) {
      LOG_ERR("BSYNC", "manifest: bad size %lu for %s", parsed, entry.id.c_str());
      return false;
    }
    if (!validBookFilename(entry.filename)) {
      LOG_ERR("BSYNC", "manifest: unsafe filename for %s, skipped", entry.id.c_str());
      return false;
    }
    // A malformed line is skipped rather than fatal: one forged or garbled entry
    // must not wedge the whole queue behind it.
    if (stateHas(*state, entry.id)) return false;

    target = std::move(entry);
    found = true;
    return true;
  }

  bool feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
      if (++bytesSeen > MANIFEST_MAX_BYTES) {
        LOG_ERR("BSYNC", "manifest over %u B, aborting", static_cast<unsigned>(MANIFEST_MAX_BYTES));
        stopped = true;
        return false;
      }
      const char c = static_cast<char>(data[i]);
      if (c == '\n') {
        const bool done = takeLine();
        line.clear();
        lineOverflow = false;
        if (done) {
          stopped = true;
          return false;
        }
        continue;
      }
      if (line.size() < MANIFEST_MAX_LINE) {
        line += c;
      } else {
        lineOverflow = true;
      }
    }
    return true;
  }
};

// GET the manifest and pick the newest id the reader has not already fetched.
// An empty body is a normal window (an empty library), not an error.
bool selectTarget(const std::string& base, const std::string& state, uint32_t deadline, ManifestEntry& out) {
  ManifestScan scan;
  scan.state = &state;
  const bool ok = HttpDownloader::fetchUrl(
      base + SUFFIX_MANIFEST, [&scan](const uint8_t* data, const size_t len) { return scan.feed(data, len); }, "", "",
      deadline);

  // A deliberate stop reports as a failed transfer (the callback aborted the
  // body); the entries parsed up to that point are still byte-valid.
  if (!ok && !scan.stopped) {
    LOG_DBG("BSYNC", "manifest fetch failed");
    return false;
  }
  // A final line with no trailing newline is still a legal entry to consider.
  if (!scan.found && !scan.stopped && !scan.line.empty()) scan.takeLine();
  if (!scan.found) {
    LOG_DBG("BSYNC", "no new books in manifest");
    return false;
  }
  out = std::move(scan.target);
  return true;
}

// Non-colliding destination inside /books, mirroring buildReadFolderDestination:
// "name.epub" -> "name (2).epub". SdFat's rename does not overwrite, and more to
// the point a book the user already has must not be clobbered by a mailbox
// delivery that happens to share its name.
std::string buildDestination(const std::string& filename) {
  std::string dest = std::string(BOOKS_DIR) + "/" + filename;
  if (!Storage.exists(dest.c_str())) return dest;

  const size_t dot = filename.rfind('.');
  const std::string stem = (dot == std::string::npos) ? filename : filename.substr(0, dot);
  const std::string ext = (dot == std::string::npos) ? "" : filename.substr(dot);
  for (int suffix = 2; suffix < MAX_NAME_SUFFIX; ++suffix) {
    dest = std::string(BOOKS_DIR) + "/" + stem + " (" + std::to_string(suffix) + ")" + ext;
    if (!Storage.exists(dest.c_str())) return dest;
  }
  return std::string();  // 99 collisions: give up rather than clobber
}

// Size match against the manifest, then rename, then record. That order is the
// whole integrity story: the contract offers no hash, so `bytes` is the only
// signal, and state-before-rename would mark a book delivered that is not there.
bool promote(const ManifestEntry& entry, const std::string& part, const std::string& state) {
  const std::string dest = buildDestination(entry.filename);
  if (dest.empty()) {
    LOG_ERR("BSYNC", "no free destination name for %s", entry.filename.c_str());
    return false;
  }
  if (!Storage.rename(part.c_str(), dest.c_str())) {
    LOG_ERR("BSYNC", "promote rename failed: %s -> %s", part.c_str(), dest.c_str());
    return false;
  }
  if (!recordDone(entry.id, state)) {
    // The book is in the library and readable; only the record failed. It will
    // be re-fetched next window and land as "name (2).epub" -- a duplicate, not
    // a loss, and the least bad outcome of an SD write failure here.
    LOG_ERR("BSYNC", "book %s promoted but not recorded", entry.id.c_str());
  }
  LOG_INF("BSYNC", "Delivered %s (%u B) as %s", entry.id.c_str(), static_cast<unsigned>(entry.bytes), dest.c_str());
  return true;
}

}  // namespace

// REMOVAL AUDIT -- every Storage::remove() reachable from this file goes through
// discardPartial() and targets the staging file /books/.incoming/{id} and nothing
// else, and a remove that failed is reported rather than assumed. The manifest is
// authoritative about what EXISTS on the server, never about what the reader
// keeps: a book that has been evicted server-side, or that the manifest never
// named, stays exactly where it is. /books is user space and the reader is the
// only party that knows what is in it.
bool BookSync::syncOnLink(const std::string& base, const uint32_t deadline, const Progress& progress) {
  if (base.empty()) return false;
  if (remainingMs(deadline) < static_cast<int32_t>(MIN_USEFUL_MS)) {
    LOG_DBG("BSYNC", "skipping books window: %d ms left", static_cast<int>(remainingMs(deadline)));
    return false;
  }

  const std::string state = loadState();
  ManifestEntry entry;
  if (!selectTarget(base, state, deadline, entry)) return false;

  // The manifest fetch is itself part of the window; re-check before paying for a
  // transfer that could not make useful progress.
  if (remainingMs(deadline) < static_cast<int32_t>(MIN_USEFUL_MS)) {
    LOG_DBG("BSYNC", "manifest used the window, %s deferred", entry.id.c_str());
    return false;
  }

  const std::string part = std::string(INCOMING_DIR) + "/" + entry.id;
  bool sizeKnown = false;
  size_t have = existingSize(part, sizeKnown);
  if (!sizeKnown) return false;  // cannot read the partial: nothing safe to do this window
  if (have > entry.bytes) {
    // Longer than the manifest says: the blob was replaced under the same id
    // (same-size replacement is undetectable -- there is no ETag). Unusable.
    LOG_ERR("BSYNC", "partial %s is %u B > manifest %u, restarting", entry.id.c_str(), static_cast<unsigned>(have),
            static_cast<unsigned>(entry.bytes));
    if (!discardPartial(part)) return false;
    have = 0;
  }

  if (have < entry.bytes) {
    // openFileForWrite does not create parents, so both levels must exist before
    // the first staging write.
    if (!Storage.ensureDirectoryExists(BOOKS_DIR) || !Storage.ensureDirectoryExists(INCOMING_DIR)) {
      LOG_ERR("BSYNC", "cannot create staging directory");
      return false;
    }

    // Name the target before the first byte, so a foreground caller's screen says
    // what it is doing rather than going quiet for a window. Fires only on the
    // path that is about to spend budget: a book already at its full size falls
    // straight through to promote() below and there is nothing to report.
    if (progress.onTarget) progress.onTarget(entry.filename, entry.bytes, have);

    // A user-requested abort is delivered as HttpDownloader's cancelFlag, which
    // sinkExpired() polls in the body read loop -- the same place the deadline is
    // polled, so it lands in the ABORTED case below and is handled by the existing
    // "the bytes so far are good, keep the partial" rule. Nothing about resume
    // changes: an aborted window is a short window.
    bool aborted = false;
    HttpDownloader::ProgressCallback abortTick;
    if (progress.shouldAbort) {
      abortTick = [&aborted, &progress](size_t, size_t) {
        if (!aborted && progress.shouldAbort()) aborted = true;
      };
    }

    // At most two attempts, and the second one ONLY for an ignored Range: a
    // server behind a range-stripping proxy answers 200 with the whole body, which
    // delivers no bytes at all at a resume offset. Discarding the partial and
    // re-sending the same Range next window spends that window on nothing at all --
    // window N keeps ~600 KB, window N+1 throws it away, window N+2 is window N
    // again. Contract section 3 step 6 says the opposite: "take the whole body or
    // fall back to a non-resuming download". Attempt 2 always runs with have == 0,
    // which sends no Range header at all, so it cannot come back RANGE_IGNORED and
    // the loop terminates.
    //
    // What this does and does not buy, stated plainly: every window now spends its
    // budget on bytes, and any book that fits inside one window completes. A book
    // BIGGER than one window still cannot be delivered by a server that strips
    // Range -- without resume there is no mechanism that could, and none of the
    // options is better: a persisted "does not range" flag cannot resume either,
    // and writing a `skip` line would permanently deny the user a book that would
    // arrive fine on the next network. So it keeps retrying, from 0, one window at a
    // time. Only reachable behind such a proxy: single-range support is mandatory
    // for the contract server and is byte-verified there.
    bool transferred = false;
    for (int attempt = 0; attempt < 2 && !transferred; ++attempt) {
      if (attempt > 0 && remainingMs(deadline) < static_cast<int32_t>(MIN_USEFUL_MS)) {
        LOG_DBG("BSYNC", "no useful budget left to restart %s", entry.id.c_str());
        return false;
      }
      LOG_INF("BSYNC", "Fetching %s from %u/%u B", entry.id.c_str(), static_cast<unsigned>(have),
              static_cast<unsigned>(entry.bytes));
      const HttpDownloader::RangeResult result = HttpDownloader::resumeToFile(
          base + SUFFIX_BOOK + entry.id, part, have, deadline, abortTick, abortTick ? &aborted : nullptr);

      // The ONE signal available that the bytes changed underneath a resume: the
      // total the server reports versus the size the manifest advertised.
      if (result.resourceTotal != 0 && result.resourceTotal != entry.bytes) {
        LOG_ERR("BSYNC", "%s is %u B on the server, manifest says %u: discarding partial", entry.id.c_str(),
                static_cast<unsigned>(result.resourceTotal), static_cast<unsigned>(entry.bytes));
        discardPartial(part);
        return false;
      }

      switch (result.error) {
        case HttpDownloader::OK:
        case HttpDownloader::ABORTED:  // deadline hit, or the caller asked to stop: bytes so far are good
          transferred = true;
          break;
        case HttpDownloader::RANGE_NOT_SATISFIABLE:
          // The offset is at or past the end and the totals agree, so the partial
          // cannot be extended and cannot be trusted. Restart cleanly next window
          // rather than retrying the same range for ever.
          LOG_ERR("BSYNC", "416 for %s at %u B, discarding partial", entry.id.c_str(), static_cast<unsigned>(have));
          discardPartial(part);
          return false;
        case HttpDownloader::RANGE_IGNORED:
          // Not one byte was written, so the partial is intact -- but this server
          // will not resume it. Restart from 0 inside THIS window (resumeToFile
          // truncates at offset 0, so the stale tail cannot survive) rather than
          // deferring a restart that would be answered with the same Range.
          LOG_ERR("BSYNC", "server ignored Range for %s, restarting from 0", entry.id.c_str());
          if (!discardPartial(part)) return false;
          have = 0;
          continue;
        default:
          // Transport or SD failure. KEEP the partial: it is the resume state, and
          // this is the single biggest departure from the note path, where deleting
          // on failure is right because a frame is one-shot.
          LOG_ERR("BSYNC", "%s transfer failed (%d) after +%u B", entry.id.c_str(), static_cast<int>(result.error),
                  static_cast<unsigned>(result.bytesWritten));
          return false;
      }
    }
    if (!transferred) return false;
    have = existingSize(part, sizeKnown);
    if (!sizeKnown) return false;
  }

  if (have != entry.bytes) {
    // Short file: a resume in progress, not a failure. Keep it for the next window.
    LOG_INF("BSYNC", "%s at %u/%u B, more windows needed", entry.id.c_str(), static_cast<unsigned>(have),
            static_cast<unsigned>(entry.bytes));
    return false;
  }

  return promote(entry, part, state);
}
