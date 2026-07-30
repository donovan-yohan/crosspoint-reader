#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

class CrossPointState : public PersistableStore<CrossPointState> {
  CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  // M2 #4 (contract 3A, display-once): the id of the last note actually PAINTED
  // as a sleep image. A note gets exactly one turn on the panel; every sleep after
  // that reverts to the configured wallpaper until a newer note arrives.
  //
  // This is NOT the old messageLastShownId, which round 1 deliberately removed:
  // that field gated DOWNLOADING (it could suppress a note's bytes forever). This
  // one gates DISPLAY only. Download dedup stays exactly where it is -- the
  // current.id sidecar, answering "do I already hold these bytes" -- and must
  // never be re-coupled to this field.
  //
  // Written at the moment of the paint, never at staging time, so a note that was
  // staged but never made it onto the panel (sync failed after the promote, a
  // quick-resume sleep) still gets its turn at the next normal sleep.
  //
  // messageCheckMinuteOfDay went with the wall-clock wake throttle (contract 3A's
  // recommendation on that throttle is "don't": it fails open without an RTC, at
  // midnight and on any clock set-back, and it cost an SD write on the wake path).
  // The structural throttle, one check per launcher wake, stands alone. Old keys in
  // state.json are simply ignored by fromJson.
  std::string messageLastDisplayedId;
  // Contract appendix A3: the passphrase of the reader's own "Sync with app" AP.
  //
  // PER DEVICE, NOT PER SESSION. The phone app saves this once, under "Reader AP
  // password", and every later session joins with the saved value -- so the
  // passphrase MUST survive reboots or the save-once pairing model is dead and the
  // user retypes a fresh 10 characters off the panel every single sync.
  //
  // Minted lazily on the FIRST AP session (MailboxSyncActivity::devicePsk), never
  // at boot: a device whose owner only ever syncs over a saved network never
  // generates one, and the mint needs the radio powered for the hardware RNG.
  //
  // Empty here means "not minted yet", which is also the recovery path: clear this
  // key in state.json and the next AP session mints a new one.
  std::string mailboxApPsk;
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;

  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
};

// Helper macro to access state
#define APP_STATE CrossPointState::getInstance()
