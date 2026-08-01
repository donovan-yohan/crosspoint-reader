#pragma once

#include "activities/Activity.h"

// Milestone 1 (messenger): display a pre-placed, full-screen 1-bit frame staged
// on the SD card at /.love-notes/current.frame, then dismiss back to the
// underlying activity (Back button). The frame is the raw panel framebuffer
// (row-major, MSB-first, 1=white/0=black) and must be exactly the live
// framebuffer size, so it is read straight into the framebuffer with no second
// allocation. Missing or wrong-sized file: finish() immediately (safe no-op).
class MessageDisplayActivity final : public Activity {
 public:
  explicit MessageDisplayActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("MessageDisplay", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
};
