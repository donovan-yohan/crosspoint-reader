#include "MessageDisplayActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include "network/MessageSync.h"

namespace {
constexpr const char* MESSAGE_FRAME_PATH = "/.love-notes/current.frame";
}

void MessageDisplayActivity::onEnter() {
  Activity::onEnter();

  // The live framebuffer is the only large buffer we can afford on the C3, so
  // read the staged frame directly into it instead of allocating a second
  // ~51 KB block (heap discipline). Bail out safely on any mismatch.
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  const size_t bufferSize = renderer.getBufferSize();

  HalFile file;
  if (frameBuffer == nullptr || !Storage.openFileForRead("MSG", MESSAGE_FRAME_PATH, file)) {
    finish();
    return;
  }
  if (file.size() != bufferSize) {
    LOG_ERR("MSG", "Frame size mismatch: %u != %u", static_cast<unsigned>(file.size()),
            static_cast<unsigned>(bufferSize));
    finish();
    return;
  }
  const size_t bytesRead = file.read(frameBuffer, bufferSize);
  if (bytesRead != bufferSize) {
    LOG_ERR("MSG", "Frame read short: %u of %u", static_cast<unsigned>(bytesRead),
            static_cast<unsigned>(bufferSize));
    finish();
    return;
  }

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  // Dedup: record this note's id so it will not reshow on the next wake.
  MessageSync::markCurrentNoteShown();
}

void MessageDisplayActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}
