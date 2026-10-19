// ge - ReXGlue Recompiled Project
//
// Post-processing filter overlay implementation. See ge_postfx.h.
//
// The filter is built from standard alpha-blended full-screen primitives (the
// only blend mode ImGui exposes), so it covers tint / brightness / vignette /
// scanlines. Per-pixel curve ops (contrast / saturation / gamma) would need a
// dedicated shader pass and are intentionally out of scope here.

#include "ge_postfx.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/ui/image_decode.h>

#include "ge_win32_compat.h"  // <windows.h>, or its SDL3 stand-in elsewhere

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// --- Tunable cvars (category "PostFX"; persisted via the menu's Save) ---
REXCVAR_DEFINE_BOOL(postfx_enabled, false, "PostFX", "Enable the post-processing filter");
REXCVAR_DEFINE_DOUBLE(postfx_brightness, 0.0, "PostFX", "Brightness offset (-1 dark .. +1 bright)")
    .range(-1.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_tint_r, 1.0, "PostFX", "Tint colour red (0..1)").range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_tint_g, 1.0, "PostFX", "Tint colour green (0..1)").range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_tint_b, 1.0, "PostFX", "Tint colour blue (0..1)").range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_tint, 0.0, "PostFX", "Tint strength (0..1)").range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_vignette, 0.0, "PostFX", "Vignette strength (0..1)").range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_scanlines, 0.0, "PostFX", "Scanline strength (0..1)").range(0.0, 1.0);
// Shader colour-grade params (applied by the GPU grade pass in the D3D12 swap).
REXCVAR_DEFINE_DOUBLE(postfx_contrast, 1.0, "PostFX", "Contrast (1=none)").range(0.0, 2.0);
REXCVAR_DEFINE_DOUBLE(postfx_saturation, 1.0, "PostFX", "Saturation (1=none)").range(0.0, 2.0);
REXCVAR_DEFINE_DOUBLE(postfx_vibrance, 0.0, "PostFX", "Vibrance (-1..1)").range(-1.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_temperature, 0.0, "PostFX", "Temperature, warm/cool (-1..1)")
    .range(-1.0, 1.0);
REXCVAR_DEFINE_DOUBLE(postfx_gamma, 1.0, "PostFX", "Gamma (1=none)").range(0.3, 3.0);

// Run-time state for the renderer (never saved to ge.toml): the window's
// width:height, so it places the menu desk picture the same way as below.
REXCVAR_DEFINE_DOUBLE(ge_backdrop_window_aspect, 0.0, "Transient",
                      "Window width:height, for placing the menu desk picture");

REXCVAR_DEFINE_BOOL(ge_show_fps, false, "Video",
                    "Show the game's frame rate in the top-left corner");

namespace ge {


namespace {

struct Preset {
  const char* name;
  bool enabled;
  float brightness, contrast, saturation, vibrance, temperature, gamma;  // shader grade
  float r, g, b, tint;                                                   // shader tint
  float vignette, scanlines;                                            // overlay
};

// Index 0 must be the neutral "Off" default.
const Preset kPresets[] = {
    // name        en    bri    con   sat   vib   tmp   gam   r     g     b    tint  vig  scan
    {"Off", false, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f},
    {"Cinematic", true, -0.04f, 1.15f, 1.05f, 0.15f, -0.10f, 1.0f, 1.0f, 0.95f, 0.85f, 0.12f, 0.40f,
     0.f},
    {"Sepia", true, -0.02f, 1.05f, 0.20f, 0.f, 0.25f, 1.0f, 1.0f, 0.82f, 0.55f, 0.50f, 0.30f, 0.f},
    {"Noir", true, -0.03f, 1.35f, 0.0f, 0.f, 0.f, 1.0f, 1.f, 1.f, 1.f, 0.f, 0.55f, 0.f},
    {"Cold", true, 0.f, 1.05f, 1.0f, 0.10f, -0.55f, 1.0f, 0.70f, 0.85f, 1.0f, 0.15f, 0.18f, 0.f},
    {"Warm", true, 0.02f, 1.05f, 1.05f, 0.15f, 0.55f, 1.0f, 1.0f, 0.85f, 0.60f, 0.12f, 0.18f, 0.f},
    {"Vibrant", true, 0.f, 1.10f, 1.20f, 0.50f, 0.f, 1.0f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f},
    {"Matrix", true, -0.03f, 1.10f, 0.85f, 0.20f, 0.f, 1.0f, 0.55f, 1.0f, 0.60f, 0.30f, 0.30f,
     0.25f},
    {"CRT", true, -0.02f, 1.05f, 1.10f, 0.10f, 0.f, 1.0f, 0.90f, 1.0f, 0.90f, 0.10f, 0.20f, 0.50f},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

void SetD(const char* name, float v) { rex::cvar::SetFlagByName(name, std::to_string(v)); }

}  // namespace

int PostFxPresetCount() { return kPresetCount; }

const char* PostFxPresetName(int index) {
  if (index < 0 || index >= kPresetCount) return "";
  return kPresets[index].name;
}

void ApplyPostFxPreset(int index) {
  if (index < 0 || index >= kPresetCount) return;
  const Preset& p = kPresets[index];
  rex::cvar::SetFlagByName("postfx_enabled", p.enabled ? "true" : "false");
  SetD("postfx_brightness", p.brightness);
  SetD("postfx_contrast", p.contrast);
  SetD("postfx_saturation", p.saturation);
  SetD("postfx_vibrance", p.vibrance);
  SetD("postfx_temperature", p.temperature);
  SetD("postfx_gamma", p.gamma);
  SetD("postfx_tint_r", p.r);
  SetD("postfx_tint_g", p.g);
  SetD("postfx_tint_b", p.b);
  SetD("postfx_tint", p.tint);
  SetD("postfx_vignette", p.vignette);
  SetD("postfx_scanlines", p.scanlines);
}

void ResetPostFx() { ApplyPostFxPreset(0); }

PostFxOverlay::PostFxOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}
PostFxOverlay::~PostFxOverlay() = default;

void PostFxOverlay::OnDraw(ImGuiIO& io) {
  if (!REXCVAR_GET(postfx_enabled)) return;

  const float W = io.DisplaySize.x, H = io.DisplaySize.y;
  if (W <= 0.0f || H <= 0.0f) return;

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoBringToFrontOnFocus |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;
  if (!ImGui::Begin("##ge_postfx", nullptr, flags)) {
    ImGui::End();
    ImGui::PopStyleVar();
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0(0, 0), p1(W, H);

  auto a8 = [](float f) {
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return static_cast<int>(f * 255.0f + 0.5f);
  };

  // NOTE: brightness / contrast / saturation / vibrance / temperature / gamma /
  // tint are all done per-pixel by the GPU grade shader (ge_grade.cs.hlsl) in
  // the D3D12 swap. This overlay only adds the spatial effects ImGui can do:
  // vignette and scanlines.

  // Vignette: four edge gradients (transparent inner -> dark edge) stack at the
  // corners to approximate a radial darkening.
  const float vig = static_cast<float>(REXCVAR_GET(postfx_vignette));
  if (vig > 0.001f) {
    const ImU32 dark = IM_COL32(0, 0, 0, a8(vig));
    const ImU32 clear = IM_COL32(0, 0, 0, 0);
    const float ext = 0.42f;  // fraction of the screen each edge band covers
    const float bw = W * ext, bh = H * ext;
    // top (dark at top edge -> clear downward)
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, bh), dark, dark, clear, clear);
    // bottom
    dl->AddRectFilledMultiColor(ImVec2(0, H - bh), ImVec2(W, H), clear, clear, dark, dark);
    // left
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(bw, H), dark, clear, clear, dark);
    // right
    dl->AddRectFilledMultiColor(ImVec2(W - bw, 0), ImVec2(W, H), clear, dark, dark, clear);
  }

  // Scanlines (CRT): thin dark horizontal lines every few pixels.
  const float scan = static_cast<float>(REXCVAR_GET(postfx_scanlines));
  if (scan > 0.001f) {
    const ImU32 line = IM_COL32(0, 0, 0, a8(scan * 0.7f));
    for (float y = 0.0f; y < H; y += 3.0f) {
      dl->AddRectFilled(ImVec2(0, y), ImVec2(W, y + 1.0f), line);
    }
  }

  ImGui::End();
  ImGui::PopStyleVar();
}

// Smooth font for the FPS counter (the SDK's own is a small pixel font).
static ImFont* g_overlay_font = nullptr;

void AddOverlayFont(ImFontAtlas* atlas) {
  if (!atlas) return;
  static const ImWchar kRanges[] = {0x0020, 0x007E, 0};
  ImFontConfig config;
  config.OversampleH = config.OversampleV = 2;
#if defined(_WIN32)
  for (const char* path : {"C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\arialbd.ttf"}) {
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
#else
  for (const char* path : {"/System/Library/Fonts/Supplemental/Arial Bold.ttf",
                           "/System/Library/Fonts/Helvetica.ttc",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"}) {
    if (access(path, R_OK) != 0) continue;
#endif
    g_overlay_font = atlas->AddFontFromFileTTF(path, 48.0f, &config, kRanges);
    if (g_overlay_font) return;
  }
}

BackdropBars::BackdropBars(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}
BackdropBars::~BackdropBars() = default;
bool MatchStarting();  // ge_hooks.cpp: the lobby's match is loading
bool BackdropBars::WantsContinuousRedraw() const { return MatchStarting(); }

bool MatchStarting();  // ge_hooks.cpp: the lobby's match is loading
uint32_t GuestPresentCount();  // ge_hooks.cpp: frames the game has presented
void GuestFrameTimes(double window_ms, std::vector<double>& out);  // ge_hooks.cpp
bool CenterDotVisible();  // ge_hooks.cpp: Show Crosshair is on and Bond is in control

namespace {
// "STARTING MATCH" across the middle of the screen while a multiplayer match
// loads (a few seconds of black on every machine, which looked like nothing
// had happened). Drawn on top of everything.
void DrawMatchStarting(float W, float H) {
  if (!ge::MatchStarting()) return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  const float size = std::max(18.0f, H * 0.06f);
  const char* text = "STARTING MATCH";
  const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  const float band = size * 2.2f;
  const float y0 = (H - band) * 0.5f;
  dl->AddRectFilled(ImVec2(0, y0), ImVec2(W, y0 + band), IM_COL32(0, 0, 0, 200));
  dl->AddText(font, size, ImVec2((W - ts.x) * 0.5f, (H - ts.y) * 0.5f), IM_COL32(236, 222, 180, 255), text);
}

// Show FPS: the game's own frames per second (guest presents, not the window's
// repaints) over the last second, in the top-right corner, refreshed twice a
// second. (vblank_timing_log in ge.toml logs the detailed frame times.)
void DrawFps(float W, float H) {
  static uint64_t last_ms = 0;
  static char line[32] = "";
  if (!REXCVAR_GET(ge_show_fps)) {
    last_ms = 0;
    return;
  }
  const uint64_t now = GetTickCount64();
  if (!last_ms || now - last_ms >= 500) {
    last_ms = now;
    std::vector<double> ms;
    ge::GuestFrameTimes(1000.0, ms);
    double sum = 0.0;
    for (double v : ms) sum += v;
    const double mean = ms.empty() ? 0.0 : sum / double(ms.size());
    std::snprintf(line, sizeof(line), "%.0f FPS", mean > 0.0 ? 1000.0 / mean : 0.0);
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = g_overlay_font ? g_overlay_font : ImGui::GetFont();
  const float size = std::max(16.0f, H * 0.03f);
  const float margin = size * 0.6f;
  const float shadow = std::max(1.0f, size * 0.06f);
  const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, line);
  const ImVec2 pos(W - ts.x - margin, margin * 0.6f);
  dl->AddText(font, size, ImVec2(pos.x + shadow, pos.y + shadow), IM_COL32(0, 0, 0, 200), line);
  dl->AddText(font, size, pos, IM_COL32(236, 222, 180, 255), line);
}

// Show Crosshair: a small white dot at the centre of the picture (where shots
// go when not aiming), with a faint dark edge so it shows on snow and sky.
void DrawCenterDot(float W, float H) {
  if (!ge::CenterDotVisible()) return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 c(std::floor(W * 0.5f) + 0.5f, std::floor(H * 0.5f) + 0.5f);
  const float r = std::max(2.0f, H * 0.0025f);
  dl->AddCircleFilled(c, r + 1.0f, IM_COL32(0, 0, 0, 110), 16);
  dl->AddCircleFilled(c, r, IM_COL32(255, 255, 255, 235), 16);
}
}  // namespace

void BackdropBars::OnDraw(ImGuiIO& io) {
  const float W = io.DisplaySize.x, H = io.DisplaySize.y;
  if (W <= 0.0f || H <= 0.0f) return;
  DrawMatchStarting(W, H);
  DrawFps(W, H);
  DrawCenterDot(W, H);
  const float window = W / H;
  if (std::fabs(window - reported_aspect_) > 0.0005f) {
    reported_aspect_ = window;
    rex::cvar::SetFlagByName("ge_backdrop_window_aspect", std::to_string(window));
  }
  if (rex::cvar::GetFlagByName("ge_backdrop_active") != "true" ||
      rex::cvar::GetFlagByName("ge_backdrop_ready") != "true" ||
      rex::cvar::GetFlagByName("present_letterbox") != "true") {
    return;
  }
  // The picture for this window's shape, as the renderer picks it.
  int kind = window >= 2.05f ? 1 : 0;
  auto load = [&](int k) {
    if (texture_[k]) return true;
    if (load_tried_[k]) return false;
    load_tried_[k] = true;
    rex::ui::ImmediateDrawer* immediate = imgui_drawer()->immediate_drawer();
    if (!immediate) {
      load_tried_[k] = false;  // not attached yet: try again next frame
      return false;
    }
    // Game\resources (beside the exe in older layouts).
    const auto exe_folder = rex::filesystem::GetExecutableFolder();
    std::ifstream file;
    for (const auto& folder : {exe_folder / "resources", exe_folder}) {
      for (const char* name : {k ? "menu-backdrop-219.png" : "menu-backdrop-169.png",
                               "menu-backdrop.png"}) {
        file.open(folder / name, std::ios::binary);
        if (file.is_open()) break;
        file.clear();  // clear() makes the stream test true again, so test is_open()
      }
      if (file.is_open()) break;
    }
    if (!file.is_open()) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    int w = 0, h = 0;
    std::vector<uint8_t> pixels = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), w, h);
    if (pixels.empty() || w <= 0 || h <= 0) return false;
    texture_[k] = immediate->CreateTexture(uint32_t(w), uint32_t(h),
                                           rex::ui::ImmediateTextureFilter::kLinear, false,
                                           pixels.data());
    if (!texture_[k]) return false;
    image_aspect_[k] = float(w) / float(h);
    return true;
  };
  if (!load(kind)) {
    kind = 1 - kind;
    if (!load(kind)) return;
  }
  const float image_aspect = image_aspect_[kind];

  // Where the presenter shows the front end's picture (16:9, or the
  // present_aspect_x/y override), centred with bars on the long side.
  float shown = 16.0f / 9.0f;
  const float ax = float(std::atof(rex::cvar::GetFlagByName("present_aspect_x").c_str()));
  const float ay = float(std::atof(rex::cvar::GetFlagByName("present_aspect_y").c_str()));
  if (ax > 0.0f && ay > 0.0f) shown = ax / ay;
  if (std::fabs(window - shown) < 0.01f) return;  // no bars

  // The picture covers the whole window, cropped evenly (as the renderer does).
  float scale_x = 1.0f, scale_y = 1.0f;
  if (window > image_aspect) {
    scale_y = image_aspect / window;
  } else {
    scale_x = window / image_aspect;
  }
  const float off_x = (1.0f - scale_x) * 0.5f, off_y = (1.0f - scale_y) * 0.5f;
  auto uv = [&](float x, float y) {
    return ImVec2(x / W * scale_x + off_x, y / H * scale_y + off_y);
  };

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoBringToFrontOnFocus |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;
  if (ImGui::Begin("##ge_backdrop_bars", nullptr, flags)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImTextureID tex = reinterpret_cast<ImTextureID>(texture_[kind].get());
    auto bar = [&](float x0, float y0, float x1, float y1) {
      if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) return;
      dl->AddImage(tex, ImVec2(x0, y0), ImVec2(x1, y1), uv(x0, y0), uv(x1, y1));
    };
    // One pixel of overlap hides any rounding seam against the picture.
    if (window > shown) {
      const float bar_w = std::floor((W - H * shown) * 0.5f) + 1.0f;
      bar(0.0f, 0.0f, bar_w, H);
      bar(W - bar_w, 0.0f, W, H);
    } else {
      const float bar_h = std::floor((H - W / shown) * 0.5f) + 1.0f;
      bar(0.0f, 0.0f, W, bar_h);
      bar(0.0f, H - bar_h, W, H);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

}  // namespace ge
