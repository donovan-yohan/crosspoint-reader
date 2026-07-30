#include "MessageDisplayActivity.h"

#include <GfxRenderer.h>

#include "network/MessageSync.h"

// M2 #2: NOT the delivery path any more -- a newly staged note is painted as the
// sleep screen and nothing ever pushes this activity on wake. It survives only as
// an optional viewer that re-opens the current note full-screen, because it
// already blits the staged frame straight into the live framebuffer with no
// second ~51 KB allocation (heap discipline on the C3) and Back dismisses it.
//
// M2 #4: deliberately does NOT call markStagedNoteDisplayed(). Display-once
// consumes the AUTOMATIC sleep-image precedence, nothing else -- the frame stays
// staged and this viewer stays available for as long as it is the current note,
// however many times the user opens it. Opening it here also must not cost a
// note the sleep-screen turn it has not had yet.
void MessageDisplayActivity::onEnter() {
  Activity::onEnter();

  if (!MessageSync::loadStagedNote(renderer.getFrameBuffer(), renderer.getBufferSize())) {
    finish();
    return;
  }

  // FULL_REFRESH is correct here and only here: this is a foreground activity the
  // user asked for. The lock screen paints HALF (see SleepActivity).
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

void MessageDisplayActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}
