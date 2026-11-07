// ge - ReXGlue Recompiled Project
//
// Setup window shown before the game starts: graphics and control settings to
// try out, then Play. It runs before the runtime and GPU are created (GeApp
// holds back OnFinalizePaths until Play), so settings read at startup, such as
// the internal resolution, apply without a restart.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <rex/ui/imgui_dialog.h>

#include <imgui.h>

#include <functional>

namespace ge {
// Whether the setup window opens at start (ge_show_setup).
bool ShowSetupAtStart();
}  // namespace ge

// Lifetime: like GeMenuDialog, created with `new` and owned by the drawer; it
// deletes itself when it closes.
class GeSetupDialog : public rex::ui::ImGuiDialog {
 public:
  struct Callbacks {
    std::function<void()> play;            // close and start the game
    std::function<void()> quit;            // close and quit
    std::function<void()> persist_config;  // write ge.toml
    std::function<void(int, int)> resize_window;  // windowed size (deferred by the owner)
  };

  GeSetupDialog(rex::ui::ImGuiDrawer* drawer, Callbacks callbacks);

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  void DrawGraphics(float width);
  void DrawControls(float width);

  Callbacks callbacks_;
  enum class Exit { kNone, kPlay, kQuit } exit_ = Exit::kNone;
  const char* rebinding_cvar_ = nullptr;  // key binding waiting for a key
  int rebind_skip_ = 0;                   // frames to ignore after the click that started it
};
