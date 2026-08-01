#include "WallpaperSync.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "network/HttpDownloader.h"

namespace {

// The single, top-priority wallpaper slot, read by
// SleepActivity::renderCustomSleepScreen before it ever looks at a directory.
constexpr char PRIMARY_PATH[] = "/sleep.bmp";

// The rotating set. The leading dot is part of the name, and it is what makes
// the folder invisible to the file browser.
constexpr char SET_DIR[] = "/.sleep";

// Staging, and NOT under /.sleep on purpose. SleepActivity picks the rotation
// directory with "/.sleep if it exists, else /sleep", and it does NOT fall back
// when /.sleep exists but holds no usable BMP -- it goes straight to the default
// sleep screen. Creating /.sleep merely to hold a partial download would
// therefore silently retire the legacy /sleep folder of a user who has one, for
// as long as the transfer takes. /.crosspoint is the firmware's own scratch
// directory (main.cpp keeps sleep_frame.bin there) and nothing renders from it,
// so a partial parked here is invisible by construction rather than by two
// filename conventions agreeing. /.sleep is created only when a `set` item is
// actually being applied into it, which is the point at which the user asked for
// a rotation to exist.
constexpr char SCRATCH_DIR[] = "/.crosspoint";
constexpr char INCOMING_DIR[] = "/.crosspoint/wallpaper";

// Id-keyed record of what has been applied. Keyed by id and never by filename,
// for the same reason BookSync is: the destination name is not stable (the
// primary slot is a single fixed path that every primary overwrites, so a
// filename diff would deliver exactly one wallpaper, ever).
constexpr char STATE_FILE[] = "/.crosspoint/mailbox-wallpaper-state";

constexpr char SUFFIX_MANIFEST[] = "/wallpaper.txt";
constexpr char SUFFIX_ITEM[] = "/wallpaper/";

constexpr char TARGET_PRIMARY[] = "primary";
constexpr char TARGET_SET[] = "set";

// Server caps, mirrored so a malformed manifest line is refused before it can
// reach the filesystem or a size comparison.
constexpr size_t MAX_WALLPAPERS = 8;
constexpr size_t MAX_WALLPAPER_BYTES = 4194304;  // 4 MiB, the shared MAX_WALLPAPER_BYTES
constexpr size_t MAX_ID_LEN = 64;
constexpr size_t MAX_FILENAME_LEN = 120;

// Smallest thing that can possibly be a BMP: 14 byte file header plus a 40 byte
// BITMAPINFOHEADER. Cheaper than opening the file to find out.
constexpr size_t MIN_BMP_BYTES = 54;

// Worst legal manifest at those caps is 8 x (64 + 1 + 8 + 1 + 7 + 1 + 120 + 1)
// = 1624 B, so 4 KB is 2.5x the worst case and still a hard stop for a body off
// a network the reader does not control. The line cap is likewise 256 against a
// 203 B worst case.
constexpr size_t MANIFEST_MAX_BYTES = 4096;
constexpr size_t MANIFEST_MAX_LINE = 256;

// State ring. Larger than MAX_WALLPAPERS for the reason BookSync's is: pruning an
// id the moment it drops off the manifest would make an evict-then-republish
// under the same id re-download. 48 ids is ~3.4 KB and costs nothing.
constexpr size_t STATE_MAX_ENTRIES = 48;
constexpr size_t STATE_MAX_BYTES = 4096;

struct ManifestEntry {
  std::string id;
  std::string filename;  // empty for a primary; the wire carries "-" there
  size_t bytes = 0;
  bool primary = false;
};

int32_t remainingMs(uint32_t deadline) { return static_cast<int32_t>(deadline - millis()); }

// The id charset from the contract, chosen so an id needs no escaping in a URL
// path segment. "." and ".." are refused separately: the id becomes a path
// component of the staging file.
bool validWallpaperId(const std::string& id) {
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
// it read over plain http on someone's hotspot. Rejecting rather than repairing,
// exactly as BookSync does: a repaired name can lose its ".bmp" tail, and
// SleepActivity's rotation scan skips anything without it -- so a "repaired"
// wallpaper would download fine and then be invisible for ever, which looks
// exactly like a sync bug.
bool validWallpaperFilename(const std::string& name) {
  if (name.empty() || name.size() > MAX_FILENAME_LEN) return false;
  if (name.front() == '.') return false;  // no hidden files, no "." / ".."
  for (const char c : name) {
    // Printable ASCII only, so one character is one byte and a C string walk
    // cannot disagree with Content-Length about where the fields are.
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7E) return false;
    // Path separators: traversal, and a name that would escape /.sleep.
    if (c == '/' || c == '\\') return false;
    // FAT-reserved. The server already replaces these with '_', so a legitimate
    // manifest never carries one; refusing is free and keeps them off the card.
    if (c == '"' || c == '*' || c == ':' || c == '<' || c == '>' || c == '?' || c == '|') return false;
  }
  // SleepActivity's rotation scan uses this exact predicate, so a name that
  // passes here is a name the sleep screen will actually consider.
  return FsHelpers::hasBmpExtension(name);
}

// Size of the staging file, or 0 when there is nothing there. `ok` reports
// whether the answer is trustworthy: a file that exists but cannot be opened (a
// flaky card) reads as 0 bytes, and a caller that treats that as "no partial"
// would waste the window re-fetching from 0.
size_t existingSize(const std::string& path, bool& ok) {
  ok = true;
  if (!Storage.exists(path.c_str())) return 0;
  HalFile file;
  if (!Storage.openFileForRead("WSYNC", path.c_str(), file)) {
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
// cannot tell the difference once the length lines up.
bool discardPartial(const std::string& path) {
  if (Storage.remove(path.c_str())) return true;
  if (!Storage.exists(path.c_str())) return true;  // already gone
  LOG_ERR("WSYNC", "cannot remove partial %s, deferring", path.c_str());
  return false;
}

// Drop every staging file that is not the one being worked on right now.
//
// WITHOUT THIS THE STAGING DIRECTORY GROWS FOR EVER, and specifically it grows
// by up to 4 MiB every time a host re-pins the wallpaper while the reader is
// away. The server's supersede rule is the cause: a NEW primary REPLACES an
// undelivered one, so the id the reader has half-downloaded simply stops
// appearing in the manifest -- it is never selected again, so nothing on the
// normal path ever revisits its partial.
//
// The rule that makes this safe is that exactly ONE transfer is live at a time:
// selectTarget() takes the FIRST unhandled manifest line and keeps taking the
// same one every window until it is applied or skipped. So anything in here that
// is not `keepId` is, by construction, an id that has left the manifest. Called
// on every window that has a target, including the one that only applies an
// already-complete file, because applyEntry() renames the kept file away.
//
// Bounded by the directory it walks: this file is the only writer, and it never
// leaves more than one entry behind.
void pruneStaging(const std::string& keepId) {
  if (!Storage.exists(INCOMING_DIR)) return;
  HalFile dir = Storage.open(INCOMING_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  std::vector<std::string> stale;
  char name[MAX_ID_LEN + 2];
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    const bool isDir = entry.isDirectory();
    entry.getName(name, sizeof(name));
    entry.close();
    if (isDir) continue;
    if (keepId == name) continue;
    // Collected rather than removed inside the walk: removing an entry from the
    // directory being iterated is exactly the kind of thing that quietly skips
    // the next one.
    stale.emplace_back(name);
  }
  dir.close();
  for (const std::string& orphan : stale) {
    const std::string path = std::string(INCOMING_DIR) + "/" + orphan;
    LOG_INF("WSYNC", "Dropping orphaned staging file %s", path.c_str());
    Storage.remove(path.c_str());
  }
}

// Whole state file, bounded. Newest entry first, so a truncating read keeps the
// entries that matter; the trailing partial line is dropped so a half-written id
// can never be matched.
std::string loadState() {
  std::string state;
  if (!Storage.exists(STATE_FILE)) return state;
  HalFile file;
  if (!Storage.openFileForRead("WSYNC", STATE_FILE, file)) return state;
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
    LOG_ERR("WSYNC", "state file oversized (%u B), read first %u", static_cast<unsigned>(size),
            static_cast<unsigned>(state.size()));
  }
  return state;
}

// "{id} done" and "{id} skip" both mean "do not fetch this id". `done` is an
// applied wallpaper; `skip` is one whose bytes arrived complete and turned out
// not to be a usable BMP, which must never be retried -- see applyEntry().
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

// Rewrite the whole file with the new id first, keeping at most
// STATE_MAX_ENTRIES lines. Called ONLY after the item has reached its final
// resting place (applied, or definitively rejected): an id recorded for a
// wallpaper still in flight would strand it for ever.
bool recordState(const std::string& id, const char* verdict, const std::string& state) {
  std::string next = id;
  next += ' ';
  next += verdict;
  next += '\n';
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
  if (!Storage.openFileForWrite("WSYNC", STATE_FILE, file)) {
    LOG_ERR("WSYNC", "cannot write state file");
    return false;
  }
  const size_t written = file.write(next.data(), next.size());
  file.close();
  if (written != next.size()) {
    LOG_ERR("WSYNC", "short state write: %u of %u", static_cast<unsigned>(written),
            static_cast<unsigned>(next.size()));
    return false;
  }
  return true;
}

// Streaming manifest scan. Holds ONE candidate, never a vector of entries: the
// window applies at most one wallpaper.
//
// THE STOP RULE IS THE ONE THING THAT DIFFERS FROM BookSync's SCAN, and it is
// the whole reason wallpaper.txt is ordered newest LAST. Stopping at the first
// unseen line takes the OLDEST unapplied item, so a reader draining a backlog
// applies them in the order the host set them and finishes on the newest.
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
      LOG_ERR("WSYNC", "manifest line too long, skipped");
      return false;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) return false;
    if (++linesSeen > MAX_WALLPAPERS) {
      LOG_ERR("WSYNC", "manifest longer than %u entries, stopping", static_cast<unsigned>(MAX_WALLPAPERS));
      return true;
    }

    // "{id} {bytes} {target} {filename}": split at the first three spaces; the
    // filename is the rest of the line and may itself contain spaces.
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    const size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;
    const size_t sp3 = line.find(' ', sp2 + 1);
    if (sp3 == std::string::npos) return false;

    ManifestEntry entry;
    entry.id = line.substr(0, sp1);
    const std::string bytesText = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string targetText = line.substr(sp2 + 1, sp3 - sp2 - 1);
    const std::string filenameText = line.substr(sp3 + 1);

    char* parseEnd = nullptr;
    const unsigned long parsed = strtoul(bytesText.c_str(), &parseEnd, 10);
    if (bytesText.empty() || parseEnd != bytesText.c_str() + bytesText.size()) return false;
    entry.bytes = static_cast<size_t>(parsed);

    if (!validWallpaperId(entry.id)) {
      LOG_ERR("WSYNC", "manifest: bad id, skipped");
      return false;
    }
    if (entry.bytes < MIN_BMP_BYTES || entry.bytes > MAX_WALLPAPER_BYTES) {
      LOG_ERR("WSYNC", "manifest: bad size %lu for %s", parsed, entry.id.c_str());
      return false;
    }

    if (targetText == TARGET_PRIMARY) {
      entry.primary = true;
      // The filename field is "-" on this line and carries no information: the
      // primary is a single fixed path. It is not validated, and it is not used.
    } else if (targetText == TARGET_SET) {
      entry.primary = false;
      entry.filename = filenameText;
      if (!validWallpaperFilename(entry.filename)) {
        LOG_ERR("WSYNC", "manifest: unsafe filename for %s, skipped", entry.id.c_str());
        return false;
      }
    } else {
      // A target this build does not know is skipped rather than guessed at, so a
      // newer server can add one without this reader writing it somewhere wrong.
      LOG_ERR("WSYNC", "manifest: unknown target for %s, skipped", entry.id.c_str());
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
        LOG_ERR("WSYNC", "manifest over %u B, aborting", static_cast<unsigned>(MANIFEST_MAX_BYTES));
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

// GET the manifest and pick the oldest id the reader has not already handled.
// An empty body is a normal window (nothing pending), not an error.
bool selectTarget(const std::string& base, const std::string& state, uint32_t deadline, ManifestEntry& out) {
  ManifestScan scan;
  scan.state = &state;
  const bool ok = HttpDownloader::fetchUrl(
      base + SUFFIX_MANIFEST, [&scan](const uint8_t* data, const size_t len) { return scan.feed(data, len); }, "", "",
      deadline);

  // A deliberate stop reports as a failed transfer (the callback aborted the
  // body); the entries parsed up to that point are still byte-valid.
  if (!ok && !scan.stopped) {
    LOG_DBG("WSYNC", "manifest fetch failed");
    return false;
  }
  // A final line with no trailing newline is still a legal entry to consider.
  if (!scan.found && !scan.stopped && !scan.line.empty()) scan.takeLine();
  if (!scan.found) {
    LOG_DBG("WSYNC", "no new wallpapers in manifest");
    return false;
  }
  out = std::move(scan.target);
  return true;
}

// Would the sleep renderer actually be able to draw this?
//
// This is the gate books do not have and wallpapers must. SleepActivity opens
// the file, calls Bitmap::parseHeaders() and renders on Ok -- so asking the SAME
// parser the SAME question here means "accepted by this function" and "drawable
// by the sleep screen" cannot drift apart. A byte-complete blob that fails it is
// not a transfer problem and re-fetching it would fail identically for ever.
bool parsesAsBitmap(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("WSYNC", path.c_str(), file)) {
    LOG_ERR("WSYNC", "cannot reopen staged wallpaper %s", path.c_str());
    return false;
  }
  BmpReaderError err;
  {
    Bitmap bitmap(file);
    err = bitmap.parseHeaders();
  }
  file.close();
  if (err != BmpReaderError::Ok) {
    LOG_ERR("WSYNC", "staged wallpaper is not usable: %s", Bitmap::errorToString(err));
    return false;
  }
  return true;
}

// Move the staged file into its final slot.
//
// REPLACE, WITH NO WINDOW IN WHICH A TORN FILE IS VISIBLE. Storage.rename() does
// not overwrite (the same constraint BookSync's buildDestination works around),
// so an occupied slot is removed first. Between the remove and the rename the
// slot is ABSENT, never half-written: the sleep renderer either finds the old
// wallpaper, or finds nothing and falls through to its next choice, or finds the
// complete new one. A power cut in that gap leaves the complete staging file and
// no state line, so the next window re-applies it from disk without touching the
// network.
bool moveIntoPlace(const std::string& part, const std::string& dest) {
  if (Storage.exists(dest.c_str()) && !Storage.remove(dest.c_str())) {
    LOG_ERR("WSYNC", "cannot clear %s, deferring", dest.c_str());
    return false;
  }
  if (!Storage.rename(part.c_str(), dest.c_str())) {
    LOG_ERR("WSYNC", "apply rename failed: %s -> %s", part.c_str(), dest.c_str());
    return false;
  }
  return true;
}

// A DELIVERED PRIMARY IS AN INSTRUCTION, NOT A SUGGESTION, so it also selects the
// sleep-screen mode that can show it. Without this the feature has a silent
// failure mode that reads exactly like a broken mailbox: a host sets the reader's
// sleep screen from the other side of the world, the bytes arrive, /sleep.bmp is
// written -- and a reader whose mode is DARK or BLANK or COVER never looks at
// that file, so nothing whatsoever changes on the panel.
//
// PRIMARY ONLY, AND NOT `set`. Pinning one image is "this is my sleep screen
// now"; adding to the rotation is not, and a `set` delivery must not be able to
// override a mode the user chose on the device.
//
// The precedent is BmpViewerActivity::doSetSleepCover(), which writes /sleep.bmp
// and then sets exactly this mode for exactly this reason -- an explicit "set as
// sleep cover" that left the mode alone would appear to do nothing.
// COVER_CUSTOM is left alone because it already reaches the custom path.
void adoptCustomSleepMode() {
  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM ||
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM) {
    return;
  }
  SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
  SETTINGS.saveToFile();
  LOG_INF("WSYNC", "Sleep screen switched to the custom wallpaper");
}

// Validate, move, record. That order is the whole integrity story: the contract
// offers no hash, so `bytes` plus a header parse is all there is, and recording
// before the move would mark a wallpaper applied that is not there.
bool applyEntry(const ManifestEntry& entry, const std::string& part, const std::string& state) {
  if (!parsesAsBitmap(part)) {
    // Complete, and not a BMP. Retrying spends a window per sync on bytes that
    // cannot ever be applied, so the id is retired with `skip` and the blob is
    // dropped. The server still holds it; the reader has simply decided.
    discardPartial(part);
    recordState(entry.id, "skip", state);
    return false;
  }

  std::string dest;
  if (entry.primary) {
    dest = PRIMARY_PATH;
  } else {
    // Created only here, i.e. only once there is a complete, parseable BMP to put
    // in it -- see the SCRATCH_DIR note at the top of this file for why an empty
    // /.sleep is not harmless.
    if (!Storage.ensureDirectoryExists(SET_DIR)) {
      LOG_ERR("WSYNC", "cannot create %s", SET_DIR);
      return false;
    }
    dest = std::string(SET_DIR) + "/" + entry.filename;
  }

  if (!moveIntoPlace(part, dest)) return false;

  if (!recordState(entry.id, "done", state)) {
    // The wallpaper is in place and will render; only the record failed. It will
    // be re-fetched next window and land on the same path -- a wasted window, not
    // a wrong screen, and the least bad outcome of an SD write failure here.
    LOG_ERR("WSYNC", "wallpaper %s applied but not recorded", entry.id.c_str());
  }
  if (entry.primary) adoptCustomSleepMode();

  LOG_INF("WSYNC", "Applied %s (%u B) as %s", entry.id.c_str(), static_cast<unsigned>(entry.bytes), dest.c_str());
  return true;
}

}  // namespace

// REMOVAL AUDIT -- three Storage::remove() sites are reachable from this file and
// no others. Two are discardPartial(), which only ever targets the staging file
// /.crosspoint/wallpaper/{id}. The third is inside moveIntoPlace(), which clears
// the destination slot immediately before renaming the new file onto it -- that
// is the replace the named slot exists for, and it happens only after the
// replacement is byte-complete AND has parsed as a BMP. Nothing here removes a
// wallpaper that is not being replaced in the same breath: an entry that has
// dropped off the manifest, or one the manifest never named, stays exactly where
// it is. /sleep.bmp and /.sleep are user space.
bool WallpaperSync::syncOnLink(const std::string& base, const uint32_t deadline, const Progress& progress) {
  if (base.empty()) return false;
  if (remainingMs(deadline) < static_cast<int32_t>(MIN_USEFUL_MS)) {
    LOG_DBG("WSYNC", "skipping wallpaper window: %d ms left", static_cast<int>(remainingMs(deadline)));
    return false;
  }

  const std::string state = loadState();
  ManifestEntry entry;
  if (!selectTarget(base, state, deadline, entry)) return false;

  // The manifest fetch is itself part of the window; re-check before paying for a
  // transfer that could not make useful progress.
  if (remainingMs(deadline) < static_cast<int32_t>(MIN_USEFUL_MS)) {
    LOG_DBG("WSYNC", "manifest used the window, %s deferred", entry.id.c_str());
    return false;
  }

  const std::string part = std::string(INCOMING_DIR) + "/" + entry.id;
  // Before anything touches the network: reclaim the space of any id that has
  // left the manifest since the last window. See pruneStaging().
  pruneStaging(entry.id);

  bool sizeKnown = false;
  size_t have = existingSize(part, sizeKnown);
  if (!sizeKnown) return false;  // cannot read the partial: nothing safe to do this window
  if (have > entry.bytes) {
    // Longer than the manifest says: the blob was replaced under the same id
    // (same-size replacement is undetectable -- there is no ETag). Unusable.
    LOG_ERR("WSYNC", "partial %s is %u B > manifest %u, restarting", entry.id.c_str(), static_cast<unsigned>(have),
            static_cast<unsigned>(entry.bytes));
    if (!discardPartial(part)) return false;
    have = 0;
  }

  if (have < entry.bytes) {
    // openFileForWrite does not create parents, so both levels must exist before
    // the first staging write.
    if (!Storage.ensureDirectoryExists(SCRATCH_DIR) || !Storage.ensureDirectoryExists(INCOMING_DIR)) {
      LOG_ERR("WSYNC", "cannot create staging directory");
      return false;
    }

    // Name the target before the first byte, so a foreground caller's screen says
    // what it is doing rather than going quiet for a window.
    if (progress.onTarget) progress.onTarget(entry.filename, entry.primary, entry.bytes, have);

    // A user-requested abort is delivered as HttpDownloader's cancelFlag, which
    // the body read loop polls in the same place it polls the deadline, so it
    // lands in the ABORTED case below and is handled by the existing "the bytes
    // so far are good, keep the partial" rule.
    bool aborted = false;
    HttpDownloader::ProgressCallback abortTick;
    if (progress.shouldAbort) {
      abortTick = [&aborted, &progress](size_t, size_t) {
        if (!aborted && progress.shouldAbort()) aborted = true;
      };
    }

    // At most two attempts, and the second one ONLY for an ignored Range --
    // identical to BookSync's loop and for the identical reason (a range-stripping
    // proxy delivers zero bytes at a resume offset, so deferring the restart burns
    // the next window too). Attempt 2 always runs with have == 0, which sends no
    // Range header at all, so it cannot come back RANGE_IGNORED and the loop
    // terminates.
    bool transferred = false;
    for (int attempt = 0; attempt < 2 && !transferred; ++attempt) {
      if (attempt > 0 && remainingMs(deadline) < static_cast<int32_t>(MIN_USEFUL_MS)) {
        LOG_DBG("WSYNC", "no useful budget left to restart %s", entry.id.c_str());
        return false;
      }
      LOG_INF("WSYNC", "Fetching %s from %u/%u B", entry.id.c_str(), static_cast<unsigned>(have),
              static_cast<unsigned>(entry.bytes));
      const HttpDownloader::RangeResult result = HttpDownloader::resumeToFile(
          base + SUFFIX_ITEM + entry.id, part, have, deadline, abortTick, abortTick ? &aborted : nullptr);

      // The ONE signal available that the bytes changed underneath a resume: the
      // total the server reports versus the size the manifest advertised.
      if (result.resourceTotal != 0 && result.resourceTotal != entry.bytes) {
        LOG_ERR("WSYNC", "%s is %u B on the server, manifest says %u: discarding partial", entry.id.c_str(),
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
          LOG_ERR("WSYNC", "416 for %s at %u B, discarding partial", entry.id.c_str(), static_cast<unsigned>(have));
          discardPartial(part);
          return false;
        case HttpDownloader::RANGE_IGNORED:
          LOG_ERR("WSYNC", "server ignored Range for %s, restarting from 0", entry.id.c_str());
          if (!discardPartial(part)) return false;
          have = 0;
          continue;
        default:
          // Transport or SD failure. KEEP the partial: it is the resume state.
          LOG_ERR("WSYNC", "%s transfer failed (%d) after +%u B", entry.id.c_str(), static_cast<int>(result.error),
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
    LOG_INF("WSYNC", "%s at %u/%u B, more windows needed", entry.id.c_str(), static_cast<unsigned>(have),
            static_cast<unsigned>(entry.bytes));
    return false;
  }

  return applyEntry(entry, part, state);
}
