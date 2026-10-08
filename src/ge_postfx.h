// ge - ReXGlue Recompiled Project
//
// Post-processing filter overlay. A passive, always-on ImGui dialog that draws
// a full-screen color filter over the guest image every frame, driven by the
// postfx_* cvars. Lets the user restyle the look (tint / brightness / vignette
// / scanlines) live, pick presets, save, or reset to default.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

#include <memory>

namespace ge {

// Full-screen filter layer. Created once at startup and kept alive; it renders
// under the pause menu (added later) but over the guest image. Non-interactive.
class PostFxOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit PostFxOverlay(rex::ui::ImGuiDrawer* drawer);
  ~PostFxOverlay();

 protected:
  void OnDraw(ImGuiIO& io) override;
};

// Front-end desk picture in the side bars. On a window wider (or taller) than
// the front end's 16:9 picture the presenter leaves bars beside it; this layer
// draws menu-backdrop.png there, placed across the whole window exactly as the
// renderer places it behind the folder, so the desk runs on unbroken. It also
// reports the window shape to the renderer (ge_backdrop_window_aspect).
class BackdropBars : public rex::ui::ImGuiDialog {
 public:
  explicit BackdropBars(rex::ui::ImGuiDrawer* drawer);
  ~BackdropBars();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // [0] menu-backdrop-169.png, [1] menu-backdrop-219.png (as in the renderer).
  std::unique_ptr<rex::ui::ImmediateTexture> texture_[2];
  float image_aspect_[2] = {0.0f, 0.0f};
  bool load_tried_[2] = {false, false};
  float reported_aspect_ = 0.0f;
};

// Built-in presets (index 0 == "Off"/default). Setting a preset writes the
// postfx_* cvars; the overlay picks the new values up next frame.
int PostFxPresetCount();
const char* PostFxPresetName(int index);
void ApplyPostFxPreset(int index);
void ResetPostFx();  // equivalent to ApplyPostFxPreset(0)

}  // namespace ge
