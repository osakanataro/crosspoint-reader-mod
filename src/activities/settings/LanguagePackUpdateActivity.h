#pragma once

#include <I18nKeys.h>

#include <string>

#include "activities/Activity.h"

// Fetches the site's copy of every UI language pack on the card and replaces
// the packs that differ. Runs only when the user asks; the language list never
// checks on its own.
class LanguagePackUpdateActivity final : public Activity {
 public:
  LanguagePackUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LanguagePackUpdate", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  bool preventAutoSleep() override { return state_ == CHECKING; }
  void render(RenderLock&&) override;

 private:
  enum State { WIFI_SELECTION, CHECKING, DONE, FAILED };

  State state_ = WIFI_SELECTION;
  StrId error_ = StrId::STR_UPDATE_FAILED;
  // Names of the languages whose packs were replaced, comma-separated.
  std::string updated_;

  void onWifiSelectionComplete(bool connected);
  // Returns false when a pack could not be fetched or replaced.
  bool updatePacks();
};
