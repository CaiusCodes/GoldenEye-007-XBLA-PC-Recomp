// ge - ReXGlue Recompiled Project
//
// Setup window shown before the game starts (see ge_setup.h). Drawn in the
// same manila briefing-folder colours as the PC settings menu.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#include "ge_setup.h"

#include <rex/cvar.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>

#include <SDL3/SDL.h>

#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <string>

#include "ge_postfx.h"

// On Windows the launcher in the release root comes first, so the setup window
// is off by default there.
#if defined(_WIN32)
REXCVAR_DEFINE_BOOL(ge_show_setup, false, "UI", "Show the setup window before the game starts");
#else
REXCVAR_DEFINE_BOOL(ge_show_setup, true, "UI", "Show the setup window before the game starts");
#endif

namespace ge {
int ImGuiKeyToVirtualKey(ImGuiKey k);  // ge_menu.cpp
bool ShowSetupAtStart() { return REXCVAR_GET(ge_show_setup); }
}  // namespace ge

namespace {

constexpr ImU32 kFolder = IM_COL32(214, 201, 162, 255);
constexpr ImU32 kFolderEdge = IM_COL32(120, 108, 78, 255);
constexpr ImU32 kInk = IM_COL32(52, 44, 32, 255);
constexpr ImU32 kInkDim = IM_COL32(92, 82, 60, 255);
constexpr ImU32 kTitle = IM_COL32(40, 33, 24, 255);
constexpr ImU32 kReticle = IM_COL32(196, 36, 28, 255);

std::string GetS(const char* name) { return rex::cvar::GetFlagByName(name); }
bool GetB(const char* name) { return GetS(name) == "true"; }
void SetB(const char* name, bool v) { rex::cvar::SetFlagByName(name, v ? "true" : "false"); }
float GetF(const char* name) { return static_cast<float>(std::atof(GetS(name).c_str())); }
void SetF(const char* name, float v) { rex::cvar::SetFlagByName(name, std::to_string(v)); }

struct Choice {
  const char* label;
  const char* value;
};

// A combo over fixed values of one cvar; true when the choice changed.
bool CvarCombo(const char* label, const char* cvar, const Choice* choices, int count,
               int fallback = 0) {
  const std::string cur = GetS(cvar);
  int idx = fallback;
  for (int i = 0; i < count; ++i)
    if (cur == choices[i].value) idx = i;
  bool changed = false;
  if (ImGui::BeginCombo(label, choices[idx].label)) {
    for (int i = 0; i < count; ++i) {
      if (ImGui::Selectable(choices[i].label, i == idx)) {
        rex::cvar::SetFlagByName(cvar, choices[i].value);
        changed = true;
      }
      if (i == idx) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

void Hint(const char* text) { ImGui::TextColored(ImColor(kInkDim), "%s", text); }

void Heading(const char* text) {
  ImGui::Spacing();
  ImGui::TextColored(ImColor(kTitle), "%s", text);
  ImGui::Separator();
}

struct Bind {
  const char* label;
  const char* cvar;
};
// The same actions as the PC settings' Key Bindings pages.
constexpr Bind kBinds[] = {
    {"Move Forward", "ge_key_mv_up"},   {"Move Back", "ge_key_mv_down"},
    {"Move Left", "ge_key_mv_left"},    {"Move Right", "ge_key_mv_right"},
    {"Fire (RT)", "ge_key_rt"},         {"Aim (LT)", "ge_key_lt"},
    {"Activate", "ge_key_a"},           {"Cycle Gadgets", "ge_key_b"},
    {"Reload", "ge_key_x"},             {"Cycle Weapons", "ge_key_y"},
    {"Precise Aim", "ge_key_lb"},       {"Toggle Graphics", "ge_key_rb"},
    {"Crouch", "ge_key_l3"},            {"Start / Pause", "ge_key_start"},
    {"Objectives / Scores", "ge_key_back"},
};

}  // namespace

GeSetupDialog::GeSetupDialog(rex::ui::ImGuiDrawer* drawer, Callbacks callbacks)
    : rex::ui::ImGuiDialog(drawer), callbacks_(std::move(callbacks)) {}

void GeSetupDialog::OnClose() {
  if (callbacks_.persist_config) callbacks_.persist_config();
  if (exit_ == Exit::kPlay && callbacks_.play) callbacks_.play();
  if (exit_ == Exit::kQuit && callbacks_.quit) callbacks_.quit();
}

void GeSetupDialog::OnDraw(ImGuiIO& io) {
  const ImVec2 disp = io.DisplaySize;
  const float w = std::floor(std::min(disp.x * 0.9f, std::max(560.0f, disp.y * 0.95f)));
  const float h = std::floor(disp.y * 0.9f);
  const ImVec2 origin(std::floor((disp.x - w) * 0.5f), std::floor((disp.y - h) * 0.5f));

  // Black behind the folder: nothing else is drawn before the game starts.
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 0, 0, 255));

  ImGui::SetNextWindowPos(origin);
  ImGui::SetNextWindowSize(ImVec2(w, h));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, kFolder);
  ImGui::PushStyleColor(ImGuiCol_Border, kFolderEdge);
  ImGui::PushStyleColor(ImGuiCol_Text, kInk);
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, kInkDim);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(180, 168, 132, 255));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(190, 178, 142, 255));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(170, 158, 122, 255));
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, kReticle);
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(220, 60, 50, 255));
  ImGui::PushStyleColor(ImGuiCol_CheckMark, kReticle);
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(180, 168, 132, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(198, 186, 150, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(166, 154, 118, 255));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(190, 178, 140, 255));
  ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(168, 150, 104, 255));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(184, 162, 112, 255));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(160, 138, 96, 255));
  ImGui::PushStyleColor(ImGuiCol_Tab, IM_COL32(196, 184, 146, 255));
  ImGui::PushStyleColor(ImGuiCol_TabHovered, IM_COL32(230, 219, 184, 255));
  ImGui::PushStyleColor(ImGuiCol_TabSelected, IM_COL32(230, 219, 184, 255));
  ImGui::PushStyleColor(ImGuiCol_Separator, kInkDim);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(172, 160, 124, 200));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(118, 102, 70, 255));
  constexpr int kColors = 24;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 18.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
  constexpr int kVars = 5;

  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoSavedSettings;
  if (ImGui::Begin("##ge_setup", nullptr, flags)) {
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextColored(ImColor(kTitle), "GOLDENEYE 007");
    ImGui::SetWindowFontScale(1.15f);
    Hint("Setup - try settings here, then press Play");
    ImGui::Spacing();

    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    const float inner_w = ImGui::GetContentRegionAvail().x;
    if (ImGui::BeginTabBar("##ge_setup_tabs")) {
      if (ImGui::BeginTabItem("Graphics")) {
        ImGui::BeginChild("##gfx", ImVec2(0, -footer));
        ImGui::PushItemWidth(inner_w * 0.5f);
        DrawGraphics(inner_w);
        ImGui::PopItemWidth();
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Controls")) {
        ImGui::BeginChild("##ctl", ImVec2(0, -footer));
        ImGui::PushItemWidth(inner_w * 0.5f);
        DrawControls(inner_w);
        ImGui::PopItemWidth();
        ImGui::EndChild();
        ImGui::EndTabItem();
      } else {
        rebinding_cvar_ = nullptr;
      }
      ImGui::EndTabBar();
    }

    ImGui::Separator();
    bool show = GetB("ge_show_setup");
    if (ImGui::Checkbox("Show this window at start", &show)) SetB("ge_show_setup", show);
    const float btn_w = 150.0f;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - btn_w * 2 - 12.0f);
    if (ImGui::Button("Quit", ImVec2(btn_w, 0))) {
      exit_ = Exit::kQuit;
      Close();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, kReticle);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(220, 60, 50, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(250, 244, 228, 255));
    if (ImGui::Button("Play", ImVec2(btn_w, 0)) ||
        (!rebinding_cvar_ && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
      exit_ = Exit::kPlay;
      Close();
    }
    ImGui::PopStyleColor(3);
  }
  ImGui::End();
  ImGui::PopStyleVar(kVars);
  ImGui::PopStyleColor(kColors);
}

void GeSetupDialog::DrawGraphics(float /*width*/) {
  Heading("DISPLAY");
  bool fs = GetB("fullscreen");
  if (ImGui::Checkbox("Fullscreen", &fs)) SetB("fullscreen", fs);  // applied by the SDK

  static const int kSizes[][2] = {{1024, 768},  {1280, 720},  {1600, 900}, {1920, 1080},
                                  {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}};
  const int cur_w = std::atoi(GetS("ge_window_width").c_str());
  const int cur_h = std::atoi(GetS("ge_window_height").c_str());
  const std::string cur_label = std::to_string(cur_w) + " x " + std::to_string(cur_h);
  ImGui::BeginDisabled(fs);
  if (ImGui::BeginCombo("Window Size", cur_label.c_str())) {
    for (const auto& s : kSizes) {
      const std::string label = std::to_string(s[0]) + " x " + std::to_string(s[1]);
      const bool sel = s[0] == cur_w && s[1] == cur_h;
      if (ImGui::Selectable(label.c_str(), sel)) {
        rex::cvar::SetFlagByName("ge_window_width", std::to_string(s[0]));
        rex::cvar::SetFlagByName("ge_window_height", std::to_string(s[1]));
        if (callbacks_.resize_window) callbacks_.resize_window(s[0], s[1]);
      }
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  bool vsync = GetB("vsync");
  if (ImGui::Checkbox("V-Sync", &vsync)) SetB("vsync", vsync);
  bool fps = GetB("ge_show_fps");
  if (ImGui::Checkbox("Show FPS", &fps)) SetB("ge_show_fps", fps);

  Heading("QUALITY");
  static const Choice kRes[] = {{"720p (1x, fastest)", "1"},
                                {"1440p (2x)", "2"},
                                {"2160p / 4K (3x)", "3"},
                                {"2880p (4x)", "4"}};
  CvarCombo("Internal Resolution", "resolution_scale", kRes, IM_ARRAYSIZE(kRes), 1);
  Hint("The 3D scene is rendered at this size. Lower it first if the game runs slowly.");
  static const Choice kAA[] = {{"Off", "none"}, {"FXAA", "fxaa"}, {"FXAA (Extreme)", "fxaa_extreme"}};
  CvarCombo("Anti-Aliasing", "swap_post_effect", kAA, IM_ARRAYSIZE(kAA));
  static const Choice kAniso[] = {{"Bilinear", "0"}, {"4x", "3"}, {"8x", "4"}, {"16x", "5"}};
  CvarCombo("Texture Filtering", "anisotropic_override", kAniso, IM_ARRAYSIZE(kAniso), 1);
  float fov = GetF("ge_fov_extra");
  if (ImGui::SliderFloat("Wider Field of View", &fov, 0.0f, 30.0f, "+%.0f degrees")) {
    SetF("ge_fov_extra", std::round(fov));
  }
#if defined(_WIN32)
  static const Choice kApi[] = {{"Auto", "auto"}, {"Direct3D 12", "d3d12"}, {"Vulkan", "vulkan"}};
  CvarCombo("Graphics API", "gpu", kApi, IM_ARRAYSIZE(kApi));
#endif

  Heading("POST-PROCESSING");
  bool pfx = GetB("postfx_enabled");
  if (ImGui::Checkbox("Colour Filter", &pfx)) SetB("postfx_enabled", pfx);
  ImGui::BeginDisabled(!pfx);
  if (ImGui::BeginCombo("Preset", "Choose a look...")) {
    for (int i = 0; i < ge::PostFxPresetCount(); ++i)
      if (ImGui::Selectable(ge::PostFxPresetName(i))) ge::ApplyPostFxPreset(i);
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  Hint("More filter controls: Help & Options > Video Settings in the game.");
}

void GeSetupDialog::DrawControls(float width) {
  Heading("MOUSE");
  bool ml = GetB("ge_mouselook_enable");
  if (ImGui::Checkbox("Mouse Look", &ml)) SetB("ge_mouselook_enable", ml);
  float sens = GetF("ge_mouse_sens");
  if (ImGui::SliderFloat("Sensitivity", &sens, 0.1f, 5.0f, "%.2f")) SetF("ge_mouse_sens", sens);
  bool inv_y = GetB("ge_invert_y");
  if (ImGui::Checkbox("Invert Up/Down", &inv_y)) SetB("ge_invert_y", inv_y);
  ImGui::SameLine();
  bool inv_x = GetB("ge_invert_x");
  if (ImGui::Checkbox("Invert Left/Right", &inv_x)) SetB("ge_invert_x", inv_x);

  Heading("KEYBOARD");
  Hint("Click a key, then press the new one (Esc cancels, right-click clears).");
  const float key_x = width * 0.42f, key_w = width * 0.4f;
  for (const Bind& b : kBinds) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(b.label);
    ImGui::SameLine(key_x);
    ImGui::PushID(b.cvar);
    std::string cur = rebinding_cvar_ == b.cvar ? std::string("press a key...") : GetS(b.cvar);
    if (cur.empty()) cur = "(none)";
    if (ImGui::Button(cur.c_str(), ImVec2(key_w, 0))) {
      rebinding_cvar_ = b.cvar;
      rebind_skip_ = 1;  // not the click that started it
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      rex::cvar::SetFlagByName(b.cvar, "");
      if (rebinding_cvar_ == b.cvar) rebinding_cvar_ = nullptr;
    }
    ImGui::PopID();
  }
  if (rebinding_cvar_) {
    if (rebind_skip_ > 0) {
      --rebind_skip_;
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      rebinding_cvar_ = nullptr;
    } else {
      int vk = 0;
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) vk = 0x01;
      else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) vk = 0x02;
      else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) vk = 0x04;
      for (ImGuiKey k = ImGuiKey_NamedKey_BEGIN; !vk && k < ImGuiKey_NamedKey_END;
           k = static_cast<ImGuiKey>(k + 1))
        if (ImGui::IsKeyPressed(k, false)) vk = ge::ImGuiKeyToVirtualKey(k);
      if (vk) {
        const std::string name = rex::ui::VirtualKeyToString(static_cast<rex::ui::VirtualKey>(vk));
        if (!name.empty()) rex::cvar::SetFlagByName(rebinding_cvar_, name);
        rebinding_cvar_ = nullptr;
      }
    }
  }
  if (ImGui::Button("Reset Keys to Default")) {
    for (const Bind& b : kBinds) rex::cvar::ResetToDefault(b.cvar);
  }

  Heading("CONTROLLER");
  int count = 0;
  SDL_JoystickID* pads = SDL_GetGamepads(&count);
  if (count <= 0) {
    Hint("No controller connected. Connect one (Xbox, PlayStation, ...) and it shows up here.");
  }
  for (int i = 0; i < count; ++i) {
    const char* name = SDL_GetGamepadNameForID(pads[i]);
    ImGui::BulletText("%s", name ? name : "Controller");
  }
  SDL_free(pads);
  if (count > 0)
    Hint("Controllers use the Xbox 360 layout, as on the console. Their buttons cannot be remapped.");
}
