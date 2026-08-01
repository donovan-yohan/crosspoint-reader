#pragma once

#include <functional>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, MAILBOX_SYNC, SYNC_WITH_APP };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 * - "Mailbox Sync" - Drain the mailbox now over a saved network (contract appendix A4)
 * - "Sync with App" - Drain the mailbox now via a phone on the reader's own AP (appendix A3)
 *
 * The last two are ONE activity with two transports, and A4 is explicit that they
 * must not fork: MailboxSyncActivity takes the transport as a constructor argument
 * and the base URL is the only thing that differs downstream.
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 */
class NetworkModeSelectionActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;

 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("NetworkModeSelection", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  void onModeSelected(NetworkMode mode);
  void onCancel();
};
