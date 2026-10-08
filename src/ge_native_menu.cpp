// ge - native PC settings pages inside the game's own Help & Options menu.
//
// Help & Options (menu 33) normally lists five rows from a table of text ids at
// 0x8272C344. We give it eight:
//   How to Play | Gamepad Settings | Keyboard & Mouse | Video Settings |
//   Online Settings | Other Settings | Credits | Done
// The three PC rows open the game's Other Settings page (menu 35) with a page
// flag set; while the flag is set, the Other Settings draw and update
// functions are skipped and our own rows are drawn with the game's own text,
// highlight and sound routines, and driven by the game's menu cursor and
// buttons, so the pages look and behave like the rest of the menus.
//
// New text uses ids 0xF000 and up, answered by a hook on the game's string
// lookup (sub_82136AD8) from UTF-16 strings kept in guest memory.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "ge_init.h"  // PPCRegister/PPCContext + generated function decls

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/kernel/xam/ge_online_control.h>
#include <rex/system/xam/user_profile.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ge {
void SetRebindCapturing(bool v);  // ge_hooks.cpp: swallow pad input while capturing
std::string TakeWheelNotch();      // ge_hooks.cpp: "WheelUp"/"WheelDown" since last call
void ApplyPlayerName(const std::string& name);  // ge_hooks.cpp
}

namespace {

// ---------------------------------------------------------------- memory ---
inline void getcb(PPCContext*& ctx, uint8_t*& base) {
  ctx = rex::runtime::ThreadState::Get()->context();
  base = rex::system::kernel_state()->memory()->virtual_membase();
}
inline uint32_t LD32(uint8_t* b, uint32_t ga) {
  uint32_t v; std::memcpy(&v, b + ga, 4); return __builtin_bswap32(v);
}
inline void ST32(uint8_t* b, uint32_t ga, uint32_t val) {
  uint32_t v = __builtin_bswap32(val); std::memcpy(b + ga, &v, 4);
}
inline float LDF32(uint8_t* b, uint32_t ga) {
  uint32_t v; std::memcpy(&v, b + ga, 4); v = __builtin_bswap32(v);
  float f; std::memcpy(&f, &v, 4); return f;
}
inline void STF32(uint8_t* b, uint32_t ga, float f) {
  uint32_t v; std::memcpy(&v, &f, 4); v = __builtin_bswap32(v); std::memcpy(b + ga, &v, 4);
}

// Game addresses (see the header comment).
constexpr uint32_t kHelpTable = 0x8272C344;     // 5 text ids (original)
constexpr uint32_t kHelpY0 = 0x8272C35C;        // first row y (float)
constexpr uint32_t kHelpSpacing = 0x8272C360;   // row spacing (float)
constexpr uint32_t kCursorX = 0x8272B37C;       // menu cursor (floats)
constexpr uint32_t kCursorY = 0x8272B380;
constexpr uint32_t kCtrlIndex = 0x82F61070;     // controller driving the menus
constexpr uint32_t kFontPtr = 0x830884BC;       // menu font
constexpr uint32_t kTextScale = 0x820019B4;     // f24 in the Other Settings draw
constexpr uint32_t kHighlightPad = 0x82000BF4;  // f25 in the Other Settings draw
constexpr uint32_t kSoundArg3 = 0x83064DE0;
constexpr uint32_t kSoundArg6 = 0x83064DE8;
constexpr uint32_t kSelectMask = 0xA000;        // "select" as the menus test it
constexpr uint32_t kBackMask = 0x4000;

// -------------------------------------------------------- custom strings ---
constexpr uint32_t kCustomBase = 0xF000;
constexpr uint32_t kCustomCount = 256;
constexpr uint32_t kCustomChars = 64;
uint32_t g_string_block = 0;  // guest address of kCustomCount * kCustomChars UTF-16 chars
uint32_t g_help_table = 0;    // guest address of our 8-entry Help & Options table
std::vector<std::string> g_custom_text(kCustomCount);

bool EnsureGuestBlocks(uint8_t* base) {
  if (g_string_block) return true;
  auto* mem = rex::system::kernel_state()->memory();
  g_string_block = mem->SystemHeapAlloc(kCustomCount * kCustomChars * 2);
  g_help_table = mem->SystemHeapAlloc(8 * 4);
  if (!g_string_block || !g_help_table) return false;
  std::memset(base + g_string_block, 0, kCustomCount * kCustomChars * 2);
  return true;
}

void SetCustomText(uint8_t* base, uint32_t id, const std::string& text) {
  const uint32_t slot = id - kCustomBase;
  if (slot >= kCustomCount || g_custom_text[slot] == text) return;
  g_custom_text[slot] = text;
  const uint32_t at = g_string_block + slot * kCustomChars * 2;
  const size_t n = std::min<size_t>(text.size(), kCustomChars - 1);
  for (size_t i = 0; i < n; ++i) {
    base[at + i * 2] = 0;
    base[at + i * 2 + 1] = static_cast<uint8_t>(text[i]);
  }
  base[at + n * 2] = 0;
  base[at + n * 2 + 1] = 0;
}

// Fixed text ids.
enum : uint32_t {
  kTxtGamepad = kCustomBase,
  kTxtKeyboard,
  kTxtVideo,
  kTxtOnline,
  kTxtRefresh,
  kTxtVersion,
  kTxtFirstDynamic = kCustomBase + 16,  // page labels and values, rewritten per frame
};

// ------------------------------------------------------------ callbacks ---
std::function<void(bool)> g_request_fullscreen;
std::function<void()> g_persist;
std::function<void()> g_restart;
std::function<void(int, int)> g_window_size;

// ---------------------------------------------------------------- pages ---
enum class Page { None, Keyboard, Video, PostFx, Online, Bindings1, Bindings2 };
Page g_page = Page::None;
bool g_leave_page = false;  // a Done row asked to go back to Help & Options

enum class RowKind { Toggle, Choice, Slider, Action, Text, Key, Label };

struct Choice {
  const char* label;
  const char* value;
};

struct Row {
  RowKind kind;
  std::string label;
  const char* cvar = nullptr;
  std::vector<Choice> choices;          // Choice
  float min = 0, max = 1, step = 0.1f;  // Slider
  std::function<void()> action;         // Action
  std::function<std::string()> value;   // Label / custom value text
  int max_len = 15;                     // Text
};

std::string GetS(const char* cvar) { return rex::cvar::GetFlagByName(cvar); }
bool GetB(const char* cvar) { return GetS(cvar) == "true"; }
// Online settings apply without a restart: the name goes straight to the
// profile, and the online client reconnects with the new server on next use.
void ApplyLive(const char* cvar, const std::string& v) {
  if (std::strcmp(cvar, "ge_username") == 0) {
    ge::ApplyPlayerName(v);
  } else if (std::strncmp(cvar, "ge_online_", 10) == 0) {
    rex::kernel::xam::GeOnlineReconnect();
  }
}
void SetS(const char* cvar, const std::string& v) {
  rex::cvar::SetFlagByName(cvar, v);
  ApplyLive(cvar, v);
  if (g_persist) g_persist();
}
float GetF(const char* cvar) { return static_cast<float>(std::atof(GetS(cvar).c_str())); }

std::vector<Row> BuildPage(Page page);

// Editing state.
const char* g_capture_cvar = nullptr;  // key capture or text entry target
bool g_capture_text = false;
int g_capture_max = 15;
std::string g_text_buffer;
bool g_capture_armed = false;  // all keys released since capture began
int g_slider_row = -1;         // slider being dragged
int g_hover = -1;

// Layout (menu units, as the game's own pages).
constexpr float kLabelX = 70.0f;
constexpr float kValueX = 245.0f;
constexpr float kRowY0 = 88.0f;
constexpr float kRowSpacing = 24.0f;
constexpr float kSliderW = 100.0f;
constexpr float kSliderH = 12.0f;
constexpr float kVersionX = 42.0f;   // main menu version label (menu space 440x330)
constexpr float kVersionY = 309.0f;
constexpr uint32_t kVersionAlpha = 105;  // black at this opacity over the paper
constexpr float kVersionScale = 0.85f;  // of the menu font's size
constexpr uint32_t kBackdropColor = 0x2B2117FFu;  // RGBA, the desk behind the folder
constexpr uint32_t kBackdropKey = 0x100010FFu;    // RGBA 16,0,16: the renderer's picture key

// Bind names as a PC player reads them ("RMB,Control" -> "Right Click, Ctrl").
std::string KeyText(const char* cvar) {
  const std::string v = GetS(cvar);
  std::string out;
  size_t start = 0;
  while (start <= v.size()) {
    const size_t comma = v.find(',', start);
    std::string one = v.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    while (!one.empty() && one.front() == ' ') one.erase(one.begin());
    while (!one.empty() && one.back() == ' ') one.pop_back();
    if (one == "LMB") one = "Left Click";
    else if (one == "RMB") one = "Right Click";
    else if (one == "MMB") one = "Middle Click";
    else if (one == "Control") one = "Ctrl";
    else if (one == "WheelUp") one = "Wheel Up";
    else if (one == "WheelDown") one = "Wheel Down";
    if (!one.empty()) out += (out.empty() ? "" : ", ") + one;
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out.empty() ? "(none)" : out;
}

void BindingRows(std::vector<Row>& rows, std::initializer_list<std::pair<const char*, const char*>> list) {
  for (const auto& b : list) {
    Row r{RowKind::Key, b.first, b.second};
    rows.push_back(r);
  }
}

void AddChoice(std::vector<Row>& rows, const char* label, const char* cvar,
               std::vector<Choice> choices) {
  Row r{RowKind::Choice, label, cvar};
  r.choices = std::move(choices);
  rows.push_back(r);
}

void AddSlider(std::vector<Row>& rows, const char* label, const char* cvar, float mn, float mx,
               float step) {
  Row r{RowKind::Slider, label, cvar};
  r.min = mn;
  r.max = mx;
  r.step = step;
  rows.push_back(r);
}

void AddAction(std::vector<Row>& rows, const char* label, std::function<void()> fn,
               std::function<std::string()> value = nullptr) {
  Row r{RowKind::Action, label};
  r.action = std::move(fn);
  r.value = std::move(value);
  rows.push_back(r);
}

std::vector<Row> BuildPage(Page page) {
  std::vector<Row> rows;
  switch (page) {
    case Page::Keyboard: {
      rows.push_back(Row{RowKind::Toggle, "Invert Look Up/Down:", "ge_invert_y"});
      rows.push_back(Row{RowKind::Toggle, "Invert Look Left/Right:", "ge_invert_x"});
      rows.push_back(Row{RowKind::Toggle, "Mouse Look:", "ge_mouselook_enable"});
      AddSlider(rows, "Mouse Sensitivity:", "ge_mouse_sens", 0.1f, 5.0f, 0.05f);
      AddSlider(rows, "Precise Aim Sensitivity:", "ge_aim_sens", 0.1f, 1.5f, 0.05f);
      rows.push_back(Row{RowKind::Toggle, "Keyboard Controls:", "ge_keyboard_enable"});
      AddAction(rows, "Key Bindings: Movement", [] { g_page = Page::Bindings1; });
      AddAction(rows, "Key Bindings: Buttons", [] { g_page = Page::Bindings2; });
      AddAction(rows, "Done", [] { g_leave_page = true; });
      break;
    }
    case Page::Bindings1:
      BindingRows(rows, {{"Move Forward:", "ge_key_mv_up"},
                         {"Move Back:", "ge_key_mv_down"},
                         {"Move Left:", "ge_key_mv_left"},
                         {"Move Right:", "ge_key_mv_right"},
                         {"Fire (RT):", "ge_key_rt"},
                         {"Aim (LT):", "ge_key_lt"},
                         {"Start:", "ge_key_start"},
                         {"Objectives / Scores:", "ge_key_back"}});
      AddAction(rows, "Done", [] { g_page = Page::Keyboard; });
      break;
    case Page::Bindings2:
      // Game actions; each still drives its controller button (A, B, X, Y,
      // LB, RB, L3) underneath.
      BindingRows(rows, {{"Activate:", "ge_key_a"},
                         {"Cycle Gadgets:", "ge_key_b"},
                         {"Reload:", "ge_key_x"},
                         {"Cycle Weapons:", "ge_key_y"},
                         {"Precise Aim:", "ge_key_lb"},
                         {"Toggle Graphics:", "ge_key_rb"},
                         {"Crouch:", "ge_key_l3"}});
      AddAction(rows, "Done", [] { g_page = Page::Keyboard; });
      break;
    case Page::Video: {
      AddAction(
          rows, "Display Mode:",
          [] {
            const bool fs = !GetB("fullscreen");
            rex::cvar::SetFlagByName("fullscreen", fs ? "true" : "false");
            if (g_request_fullscreen) g_request_fullscreen(fs);
            if (g_persist) g_persist();
          },
          [] { return std::string(GetB("fullscreen") ? "Fullscreen" : "Windowed"); });
      rows.push_back(Row{RowKind::Toggle, "V-Sync:", "vsync"});
      AddChoice(rows, "Frame Limit:", "max_fps",
                {{"30", "30"}, {"60", "60"}, {"120", "120"}, {"144", "144"}, {"165", "165"},
                 {"240", "240"}, {"Uncapped", "0"}});
      AddSlider(rows, "Field of View:", "ge_fov_extra", 0.0f, 30.0f, 1.0f);
      AddChoice(rows, "Anti-Aliasing:", "swap_post_effect",
                {{"Off", "none"}, {"FXAA", "fxaa"}, {"FXAA Extreme", "fxaa_extreme"}});
      AddChoice(rows, "Texture Filtering:", "anisotropic_override",
                {{"Bilinear", "0"}, {"4x", "3"}, {"8x", "4"}, {"16x", "5"}});
      // Window size in windowed mode (fullscreen always uses the display). The
      // game keeps rendering the same way; the picture is scaled to the window.
      AddAction(
          rows, "Resolution:",
          [] {
            if (GetB("fullscreen")) return;  // fullscreen always uses the screen's size
            // 4:3, 16:9 and 21:9 sizes; Screen Ratio follows the window shape.
            static const int kSizes[][2] = {{1024, 768},  {1280, 720},  {1920, 1080},
                                            {2560, 1080}, {2560, 1440}, {3440, 1440},
                                            {3840, 2160}};
            constexpr int kCount = 7;
            const int w = std::atoi(GetS("ge_window_width").c_str());
            const int h = std::atoi(GetS("ge_window_height").c_str());
            int next = 1;  // 1280x720 unless the current size is in the list
            for (int i = 0; i < kCount; ++i)
              if (kSizes[i][0] == w && kSizes[i][1] == h) next = (i + 1) % kCount;
            rex::cvar::SetFlagByName("ge_window_width", std::to_string(kSizes[next][0]));
            rex::cvar::SetFlagByName("ge_window_height", std::to_string(kSizes[next][1]));
            if (g_persist) g_persist();
            if (g_window_size) g_window_size(kSizes[next][0], kSizes[next][1]);
          },
          [] {
            if (GetB("fullscreen")) {  // the size of the screen the game is on
              MONITORINFO mi{sizeof(mi)};
              HMONITOR mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
              if (mon && GetMonitorInfoW(mon, &mi))
                return std::to_string(mi.rcMonitor.right - mi.rcMonitor.left) + "x" +
                       std::to_string(mi.rcMonitor.bottom - mi.rcMonitor.top) + " (screen)";
              return std::string("Screen size");
            }
            return GetS("ge_window_width") + "x" + GetS("ge_window_height");
          });
      AddAction(rows, "Post-Processing", [] { g_page = Page::PostFx; });
      AddAction(rows, "Done", [] { g_leave_page = true; });
      break;
    }
    case Page::PostFx:
      rows.push_back(Row{RowKind::Toggle, "Post-Processing:", "postfx_enabled"});
      AddSlider(rows, "Brightness:", "postfx_brightness", -1.0f, 1.0f, 0.02f);
      AddSlider(rows, "Contrast:", "postfx_contrast", 0.0f, 2.0f, 0.02f);
      AddSlider(rows, "Saturation:", "postfx_saturation", 0.0f, 2.0f, 0.02f);
      AddSlider(rows, "Vibrance:", "postfx_vibrance", -1.0f, 1.0f, 0.02f);
      AddSlider(rows, "Temperature:", "postfx_temperature", -1.0f, 1.0f, 0.02f);
      AddSlider(rows, "Gamma:", "postfx_gamma", 0.3f, 3.0f, 0.02f);
      AddSlider(rows, "Vignette:", "postfx_vignette", 0.0f, 1.0f, 0.02f);
      AddSlider(rows, "Scanlines:", "postfx_scanlines", 0.0f, 1.0f, 0.02f);
      AddAction(rows, "Done", [] { g_page = Page::Video; });
      break;
    case Page::Online: {
      Row name{RowKind::Text, "Player Name:", "ge_username"};
      name.max_len = 15;
      rows.push_back(name);
      rows.push_back(Row{RowKind::Toggle, "Online Server Play:", "ge_online_enable"});
      Row server{RowKind::Text, "Server Address:", "ge_online_server"};
      server.max_len = 40;
      rows.push_back(server);
      Row port{RowKind::Text, "Server Port:", "ge_online_port"};
      port.max_len = 5;
      rows.push_back(port);
      AddAction(rows, "Done", [] { g_leave_page = true; });
      break;
    }
    case Page::None:
      break;
  }
  return rows;
}

std::string ValueText(const Row& r, int index) {
  switch (r.kind) {
    case RowKind::Toggle:
      return GetB(r.cvar) ? "ON" : "OFF";
    case RowKind::Choice: {
      const std::string cur = GetS(r.cvar);
      for (const auto& c : r.choices)
        if (cur == c.value) return c.label;
      return r.choices.empty() ? "" : r.choices.front().label;
    }
    case RowKind::Slider: {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.2f", GetF(r.cvar));
      if (std::strcmp(r.cvar, "ge_fov_extra") == 0)
        std::snprintf(buf, sizeof(buf), "+%d", static_cast<int>(std::lround(GetF(r.cvar))));
      return buf;
    }
    case RowKind::Text:
      if (g_capture_text && g_capture_cvar == r.cvar) return g_text_buffer + "_";
      return GetS(r.cvar);
    case RowKind::Key:
      if (!g_capture_text && g_capture_cvar == r.cvar) return "Press a key...";
      return KeyText(r.cvar);
    case RowKind::Action:
    case RowKind::Label:
      return r.value ? r.value() : "";
  }
  (void)index;
  return "";
}

// --------------------------------------------------------- guest drawing ---
struct GuestFrame {
  PPCContext& ctx;
  uint64_t saved_r1;
  uint32_t sp;
  GuestFrame(PPCContext& c) : ctx(c), saved_r1(c.r1.u64) {
    sp = (c.r1.u32 - 0x600) & ~0xFu;
    ctx.r1.u64 = sp;
  }
  ~GuestFrame() { ctx.r1.u64 = saved_r1; }
};

uint32_t LookupText(PPCContext& ctx, uint8_t* base, uint32_t id) {
  ctx.r3.u64 = id;
  sub_82136AD8(ctx, base);
  return ctx.r3.u32;
}

void MeasureText(PPCContext& ctx, uint8_t* base, uint32_t sp, uint32_t str, int& w, int& h) {
  ctx.r3.u64 = sp + 0x100;
  ctx.r4.u64 = LD32(base, kFontPtr);
  ctx.r5.u64 = str;
  ctx.f1.f64 = LDF32(base, kTextScale);
  ctx.r6.u64 = sp + 0x110;
  ctx.r7.u64 = sp + 0x114;
  sub_8209BD50(ctx, base);
  ctx.r3.u64 = sp + 0x100;
  sub_8209B890(ctx, base);
  w = static_cast<int32_t>(LD32(base, sp + 0x110));
  h = static_cast<int32_t>(LD32(base, sp + 0x114));
}

void DrawBox(PPCContext& ctx, uint8_t* base, uint32_t alpha, float x0, float y0, float x1,
             float y1) {
  ctx.r3.u64 = alpha;
  ctx.f1.f64 = x0;
  ctx.f2.f64 = y0;
  ctx.f3.f64 = x1;
  ctx.f4.f64 = y1;
  ctx.r8.u64 = 0;
  ctx.r9.u64 = 0;
  sub_820972F8(ctx, base);
}

void DrawText(PPCContext& ctx, uint8_t* base, uint32_t sp, uint32_t str, float x, float y,
              uint32_t alpha, float scale = 0.0f) {
  ctx.r3.u64 = sp + 0x120;
  ctx.r4.u64 = LD32(base, kFontPtr);
  ctx.r5.u64 = str;
  sub_8209BCD8(ctx, base);
  ctx.r4.u64 = ctx.r3.u64;
  ctx.r3.u64 = sp + 0x130;
  ctx.f1.f64 = x;
  ctx.f2.f64 = y;
  sub_8209BA00(ctx, base);
  ctx.r3.u64 = sp + 0x140;
  ctx.r4.u64 = sp + 0x130;
  ctx.r5.u64 = alpha;
  sub_8209BB58(ctx, base);
  if (scale > 0.0f) {
    // Like the game's own text draws: set the font's scale before drawing.
    ctx.r4.u64 = ctx.r3.u64;
    ctx.r3.u64 = sp + 0x150;
    ctx.f1.f64 = scale;
    sub_8209BBB8(ctx, base);
    ctx.r3.u64 = sp + 0x150;
    sub_8209B890(ctx, base);
  }
  for (uint32_t off : {0x140u, 0x130u, 0x120u}) {
    ctx.r3.u64 = sp + off;
    sub_8209B890(ctx, base);
  }
}

void PlaySelectSound(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = LD32(base, kSoundArg3);
  ctx.r4.u64 = 199;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = LD32(base, kSoundArg6);
  ctx.r7.u64 = 0x820061A4;
  ctx.r8.u64 = 0;
  sub_82144920(ctx, base);
}

bool ButtonPressed(PPCContext& ctx, uint8_t* base, uint32_t mask) {
  ctx.r3.u64 = LD32(base, kCtrlIndex);
  ctx.r4.u64 = mask;
  sub_820A05B8(ctx, base);
  return ctx.r3.u32 != 0;
}

bool ButtonHeld(PPCContext& ctx, uint8_t* base, uint32_t mask) {
  ctx.r3.u64 = LD32(base, kCtrlIndex);
  ctx.r4.u64 = mask;
  sub_820A0548(ctx, base);
  return ctx.r3.u32 != 0;
}

float RowY(int i) { return kRowY0 + kRowSpacing * static_cast<float>(i); }

int RowAtCursor(uint8_t* base, int count) {
  const float y = LDF32(base, kCursorY);
  for (int i = 0; i < count; ++i) {
    if (y >= RowY(i) - 4.0f && y < RowY(i) - 4.0f + kRowSpacing) return i;
  }
  return -1;
}

// ------------------------------------------------------ key/text capture ---
std::string CaptureKey() {
  std::string wheel = ge::TakeWheelNotch();
  if (!wheel.empty()) return wheel;
  // Shift/Ctrl/Alt come back as the generic keys; their L/R forms have no names.
  for (int vk = 1; vk <= 0xFE; ++vk) {
    if (GetAsyncKeyState(vk) & 0x8000) {
      std::string name = rex::ui::VirtualKeyToString(static_cast<rex::ui::VirtualKey>(vk));
      if (!name.empty()) return name;
    }
  }
  return {};
}

bool AnyKeyDown() {
  for (int vk = 1; vk <= 0xFE; ++vk)
    if (GetAsyncKeyState(vk) & 0x8000) return true;
  return false;
}

void EndCapture() {
  g_capture_cvar = nullptr;
  g_capture_text = false;
  ge::SetRebindCapturing(false);
}

void UpdateCapture() {
  if (!g_capture_cvar) return;
  if (!g_capture_armed) {
    if (!AnyKeyDown()) g_capture_armed = true;
    if (!g_capture_text) ge::TakeWheelNotch();
    return;
  }
  if (!g_capture_text) {
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {  // Esc cancels, bind unchanged
      EndCapture();
      g_capture_armed = false;
      return;
    }
    std::string key = CaptureKey();
    if (!key.empty()) {
      SetS(g_capture_cvar, key);
      EndCapture();
      g_capture_armed = false;
    }
    return;
  }
  // Text entry: letters, digits, . - _ and space; Backspace; Enter or Esc ends.
  static bool was_down[256] = {};
  for (int vk = 1; vk <= 0xFE; ++vk) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = down && !was_down[vk];
    was_down[vk] = down;
    if (!edge) continue;
    if (vk == VK_RETURN || vk == VK_ESCAPE) {
      if (vk == VK_RETURN) SetS(g_capture_cvar, g_text_buffer);
      EndCapture();
      g_capture_armed = false;
      std::memset(was_down, 0, sizeof(was_down));
      return;
    }
    if (vk == VK_BACK) {
      if (!g_text_buffer.empty()) g_text_buffer.pop_back();
      continue;
    }
    char c = 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (vk >= 'A' && vk <= 'Z') c = static_cast<char>(shift ? vk : vk + 32);
    else if (vk >= '0' && vk <= '9') c = static_cast<char>(vk);
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) c = static_cast<char>('0' + vk - VK_NUMPAD0);
    else if (vk == VK_OEM_PERIOD || vk == VK_DECIMAL) c = '.';
    else if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) c = shift ? '_' : '-';
    else if (vk == VK_SPACE) c = ' ';
    if (c && static_cast<int>(g_text_buffer.size()) < g_capture_max) g_text_buffer.push_back(c);
  }
}

void BeginCapture(const Row& r) {
  g_capture_cvar = r.cvar;
  g_capture_text = (r.kind == RowKind::Text);
  g_capture_max = r.max_len;
  g_capture_armed = false;
  ge::TakeWheelNotch();  // drop notches from before the capture
  if (g_capture_text) g_text_buffer = GetS(r.cvar);
  ge::SetRebindCapturing(true);
}

// ------------------------------------------------------------ the page ---
void DrawPage(PPCContext& ctx, uint8_t* base) {
  if (!EnsureGuestBlocks(base)) return;
  const std::vector<Row> rows = BuildPage(g_page);
  GuestFrame frame(ctx);
  const uint32_t sp = frame.sp;
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& r = rows[i];
    const uint32_t label_id = kTxtFirstDynamic + static_cast<uint32_t>(i) * 2;
    const uint32_t value_id = label_id + 1;
    SetCustomText(base, label_id, r.label);
    SetCustomText(base, value_id, ValueText(r, static_cast<int>(i)));
    const float y = RowY(static_cast<int>(i));
    const uint32_t label = LookupText(ctx, base, label_id);
    if (static_cast<int>(i) == g_hover) {
      int w = 0, h = 0;
      MeasureText(ctx, base, sp, label, w, h);
      const float pad = LDF32(base, kHighlightPad);
      DrawBox(ctx, base, 50, kLabelX - pad, y - pad, kLabelX - pad + w + 4, y - pad + h + 4);
    }
    DrawText(ctx, base, sp, label, kLabelX, y, 255);
    if (r.kind == RowKind::Slider) {
      const float t = std::clamp((GetF(r.cvar) - r.min) / (r.max - r.min), 0.0f, 1.0f);
      DrawBox(ctx, base, 60, kValueX, y + 2, kValueX + kSliderW, y + 2 + kSliderH);
      DrawBox(ctx, base, 200, kValueX, y + 2, kValueX + kSliderW * t, y + 2 + kSliderH);
      DrawText(ctx, base, sp, LookupText(ctx, base, value_id), kValueX + kSliderW + 10, y, 255);
    } else {
      DrawText(ctx, base, sp, LookupText(ctx, base, value_id), kValueX, y, 255);
    }
  }
}

void Activate(Row& r) {
  switch (r.kind) {
    case RowKind::Toggle:
      SetS(r.cvar, GetB(r.cvar) ? "false" : "true");
      break;
    case RowKind::Choice: {
      const std::string cur = GetS(r.cvar);
      size_t idx = 0;
      for (size_t i = 0; i < r.choices.size(); ++i)
        if (cur == r.choices[i].value) idx = i;
      idx = (idx + 1) % r.choices.size();
      SetS(r.cvar, r.choices[idx].value);
      break;
    }
    case RowKind::Action:
      if (r.action) r.action();
      break;
    case RowKind::Key:
    case RowKind::Text:
      BeginCapture(r);
      break;
    case RowKind::Slider:
    case RowKind::Label:
      break;
  }
}

// Returns true when the page asks to go back to Help & Options.
bool UpdatePage(PPCContext& ctx, uint8_t* base) {
  UpdateCapture();
  std::vector<Row> rows = BuildPage(g_page);
  if (g_capture_cvar) return false;  // typing: the menu waits
  g_hover = RowAtCursor(base, static_cast<int>(rows.size()));

  // Sliders follow the cursor while select is held, like the game's volume bars.
  if (g_slider_row >= 0) {
    if (ButtonHeld(ctx, base, kSelectMask) && g_slider_row < static_cast<int>(rows.size())) {
      Row& r = rows[g_slider_row];
      float t = (LDF32(base, kCursorX) - kValueX) / kSliderW;
      t = std::clamp(t, 0.0f, 1.0f);
      float v = r.min + t * (r.max - r.min);
      v = std::round(v / r.step) * r.step;
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.3f", v);
      rex::cvar::SetFlagByName(r.cvar, buf);
      return false;
    }
    g_slider_row = -1;
    if (g_persist) g_persist();
  }

  if (ButtonPressed(ctx, base, kBackMask)) {
    const Page parent = (g_page == Page::PostFx)                                   ? Page::Video
                        : (g_page >= Page::Bindings1 && g_page <= Page::Bindings2) ? Page::Keyboard
                                                                                   : Page::None;
    PlaySelectSound(ctx, base);
    if (parent != Page::None) {
      g_page = parent;
      return false;
    }
    return true;
  }
  if (g_hover >= 0 && ButtonPressed(ctx, base, kSelectMask)) {
    Row& r = rows[g_hover];
    PlaySelectSound(ctx, base);
    if (r.kind == RowKind::Slider) {
      g_slider_row = g_hover;
    } else {
      Activate(r);
    }
  }
  if (g_leave_page) {
    g_leave_page = false;
    return true;
  }
  return false;
}

}  // namespace

namespace ge {
void SetNativeMenuCallbacks(std::function<void(bool)> fullscreen, std::function<void()> persist,
                            std::function<void()> restart) {
  g_request_fullscreen = std::move(fullscreen);
  g_persist = std::move(persist);
  g_restart = std::move(restart);
}
void SetWindowSizeHandler(std::function<void(int, int)> handler) {
  g_window_size = std::move(handler);
}
}  // namespace ge

// ================================================================ hooks ===

// String lookup (sub_82136AD8 entry): our ids come from guest memory.
bool ge_str_lookup(PPCRegister& r3) {
  const uint32_t id = r3.u32;
  if (id < kCustomBase || id >= kCustomBase + kCustomCount || !g_string_block) return false;
  r3.u64 = g_string_block + (id - kCustomBase) * kCustomChars * 2;
  return true;
}

// --- Help & Options (menu 33) ---------------------------------------------
// Rows: 0 How to Play, 1 Gamepad, 2 Keyboard & Mouse, 3 Video, 4 Online,
//       5 Other Settings, 6 Credits, 7 Done.
namespace {
constexpr int kHelpRows = 8;
void PrepareHelpTable(uint8_t* base) {
  if (!EnsureGuestBlocks(base)) return;
  static bool done = false;
  if (done) return;
  done = true;
  SetCustomText(base, kTxtGamepad, "Gamepad Settings");
  SetCustomText(base, kTxtKeyboard, "Keyboard & Mouse");
  SetCustomText(base, kTxtVideo, "Video Settings");
  SetCustomText(base, kTxtOnline, "Online Settings");
  const uint32_t ids[kHelpRows] = {0x9D55, kTxtGamepad, kTxtKeyboard, kTxtVideo,
                                   kTxtOnline, 0x9D54, 0x9D67, 0x9D66};
  for (int i = 0; i < kHelpRows; ++i) ST32(base, g_help_table + i * 4, ids[i]);
  // Eight rows need a tighter column than the original five.
  STF32(base, kHelpY0, 118.0f);
  STF32(base, kHelpSpacing, 24.0f);
}
}  // namespace

// Draw (sub_820FDD50): after `addi r26,r11,-15548` point r26 at our table.
void ge_help_draw_table(PPCRegister& r26) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  PrepareHelpTable(base);
  if (g_help_table) r26.u64 = g_help_table;
}
// Draw loop end: after `addi r11,r26,20` make it eight entries.
void ge_help_draw_end(PPCRegister& r11, PPCRegister& r26) {
  r11.u64 = r26.u32 + kHelpRows * 4;
}
// Update (sub_820F1248): the default row when the cursor is below the list.
void ge_help_default_row(PPCRegister& r11) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  PrepareHelpTable(base);
  r11.u64 = kHelpRows - 1;
}
// Update: the hover loop runs over eight rows (was `cmpwi r11,5`).
bool ge_help_row_loop(PPCRegister& r11) { return r11.s32 < kHelpRows; }
// Update: the chosen row (r9) is mapped onto the original handlers; the three
// PC rows use the Other Settings path with our page flag set.
void ge_help_route(PPCRegister& r9) {
  switch (r9.s32) {
    case 0: r9.u64 = 0; break;                                  // How to Play
    case 1: r9.u64 = 1; break;                                  // Gamepad
    case 2: g_page = Page::Keyboard; r9.u64 = 2; break;         // -> menu 35, ours
    case 3: g_page = Page::Video; r9.u64 = 2; break;
    case 4: g_page = Page::Online; r9.u64 = 2; break;
    case 5: g_page = Page::None; r9.u64 = 2; break;             // Other Settings
    case 6: r9.u64 = 3; break;                                  // Credits
    default: r9.u64 = 4; break;                                 // Done
  }
  g_hover = -1;
  g_slider_row = -1;
}

// --- Other Settings (menu 35) draw/update take our page when one is set ---
// Draw (sub_820FE1B8) after its two common calls; true = skip to its end.
bool ge_other_draw(PPCRegister& r3) {
  (void)r3;
  if (g_page == Page::None) return false;
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  DrawPage(*ctx, base);
  return true;
}
// Update (sub_820F1DA8) after `li r25,1`; true = skip to its common tail.
bool ge_other_update(PPCRegister& r25) {
  if (g_page == Page::None) return false;
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  const uint64_t keep25 = r25.u64;
  const bool back = UpdatePage(*ctx, base);
  r25.u64 = keep25;
  if (back) {
    // As the game's own Other Settings does on B: 0x82F61014 = 1, 0x82F61018 = 7.
    ST32(base, 0x82F61014u, 1);
    ST32(base, 0x82F61018u, 7);
  }
  return true;
}

// --- LAN join screen: a Refresh row ------------------------------------------
// The join screen (draw sub_821019F8, update sub_820F3310) searches once when
// it opens: state 0x82F610D4 goes 0 (start search) -> 1 (searching) -> 2
// (results). Refresh puts it back to 0, the same thing its own "search again"
// does after a failed join. The row sits under Create Match, or under the list
// of games, and is pointed at and clicked like the game's own rows.
namespace {
constexpr uint32_t kLanState = 0x82F610D4;
constexpr float kLanRefreshX = 50.0f;     // lines up with Create Match
constexpr uint32_t kLanHoveredGame = 0x8272C614;  // game row under the crosshair (-1 none)
int g_lan_refresh_w = 60;  // width of the Refresh label, measured when drawn

float LanRefreshY(PPCContext& ctx, uint8_t* base) {
  sub_8215E5B0(ctx, base);  // number of games found
  int count = static_cast<int32_t>(ctx.r3.u32);
  if (count > 9) count = 9;
  return count > 0 ? 114.0f + 20.0f * static_cast<float>(count) + 10.0f : 175.0f;
}

bool LanRefreshHover(uint8_t* base, float y, int w) {
  const float cx = LDF32(base, kCursorX);
  const float cy = LDF32(base, kCursorY);
  return cx >= kLanRefreshX - 6.0f && cx <= kLanRefreshX + static_cast<float>(w) + 10.0f &&
         cy >= y - 6.0f && cy < y + 18.0f;
}
}  // namespace

// Main menu: the release version in the bottom left corner, from version.txt
// in Game\resources (written by Make-Release). Dev builds have none and
// show nothing. Called by the main menu's row draw (ge_f2_driver).
namespace {
std::string ReadVersionText() {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  std::wstring dir(path, n);
  dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
  for (const std::wstring& file : {dir + L"resources\\version.txt", dir + L"version.txt"}) {
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) continue;
    char buf[32] = {};
    const size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::string v(buf, got);
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
    if (!v.empty()) return "v" + v;
  }
  return {};
}
}  // namespace

// Run-time state shared with the renderer (never saved to ge.toml).
REXCVAR_DEFINE_BOOL(ge_backdrop_active, false, "Transient",
                    "The front end is up: the renderer shows the menu backdrop picture");
REXCVAR_DEFINE_BOOL(ge_backdrop_ready, false, "Transient",
                    "Set by the renderer once menu-backdrop.png is loaded");

// Called every controller poll: tell the renderer when the front end is up.
void ge_backdrop_tick(bool front_end) {
  static int last = -1;
  if (last == static_cast<int>(front_end)) return;
  last = static_cast<int>(front_end);
  rex::cvar::SetFlagByName("ge_backdrop_active", front_end ? "true" : "false");
}

// Front-end menus: the open folder is a 3D model (sub_820F5898, drawn first by
// every menu) framed for a 4:3 screen, so a wide window showed black where the
// model ends. Draw the desk it lies on first: a full-screen box in the
// Setup's desk brown, which the model then covers.
void ge_frontend_backdrop() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  // Starting a mission: the game blacks out the folder (menu 12) and loads the
  // stage with no controller polls, so the desk goes too here, all at once.
  const uint32_t menu = LD32(base, 0x8272B35Cu);
  const uint32_t stage = LD32(base, 0x82423E00u);
  if (menu == 12u || stage != 90u) {
    static uint32_t logged = 0;
    if (logged != (menu << 8 | stage)) {
      logged = menu << 8 | stage;
      REXKRNL_INFO("GEBACKDROP off: menu {} stage {}", menu, stage);
    }
    ge_backdrop_tick(false);
    return;
  }
  const PPCContext saved = *ctx;  // entry hook: keep lr and argument registers
  {
    GuestFrame frame(*ctx);
    // With menu-backdrop.png loaded by the renderer, paint the key colour it
    // replaces with the picture; otherwise the plain desk brown.
    const uint32_t color = REXCVAR_GET(ge_backdrop_ready) ? kBackdropKey : kBackdropColor;
    DrawBox(*ctx, base, color, -400.0f, -200.0f, 840.0f, 530.0f);
  }
  *ctx = saved;
}

// Text for other files (ge_portraits.cpp: character names), id 0xF000 + slot.
namespace ge {
bool SetGameText(uint8_t* base, uint32_t id, const std::string& text) {
  if (!EnsureGuestBlocks(base)) return false;
  SetCustomText(base, id, text);
  return true;
}
}  // namespace ge

// Pause menu Help & Options rows (text ids 0xF0F0 / 0xF0F1, see ge_hooks.cpp).
namespace ge {
bool WatchKeyboardPage();  // ge_hooks.cpp
void SetWatchHelpStrings(uint8_t* base) {
  if (!EnsureGuestBlocks(base)) return;
  // Lower case: the watch font draws lower case as small capitals, which is how the
  // game's own rows look; capitals would come out larger.
  SetCustomText(base, 0xF0F0u, "gamepad controls");
  SetCustomText(base, 0xF0F1u, "keyboard controls");
}
}  // namespace ge

// The keyboard binds on the watch page; rows 1.. can be rebound there (pick a
// row, press a key; Esc cancels), as on the main menu's Key Bindings pages.
struct WatchKbdItem {
  const char* label;
  const char* cvar;
};
const WatchKbdItem kWatchKbdItems[] = {
    {"Look", nullptr},
    {"Move Forward", "ge_key_mv_up"},   {"Move Back", "ge_key_mv_down"},
    {"Move Left", "ge_key_mv_left"},    {"Move Right", "ge_key_mv_right"},
    {"Fire", "ge_key_rt"},              {"Aim", "ge_key_lt"},
    {"Precise Aim", "ge_key_lb"},       {"Activate", "ge_key_a"},
    {"Cycle Gadgets", "ge_key_b"},      {"Reload", "ge_key_x"},
    {"Cycle Weapons", "ge_key_y"},      {"Toggle Graphics", "ge_key_rb"},
    {"Crouch", "ge_key_l3"},            {"Pause", "ge_key_start"},
    {"Objectives", "ge_key_back"},
};
constexpr int kWatchKbdRows = static_cast<int>(std::size(kWatchKbdItems));
int g_wk_sel = 1;
bool g_wk_capture = false;  // the capture below belongs to the watch page

namespace ge {
int WatchKbdRows() { return kWatchKbdRows; }
int WatchKbdSelected() { return g_wk_sel; }
void WatchKbdSelect(int row) {
  if (row >= 1 && row < kWatchKbdRows && !g_wk_capture) g_wk_sel = row;
}
bool WatchKbdCapturing() { return g_wk_capture; }
void WatchKbdBegin(int row) {
  if (g_wk_capture || row < 1 || row >= kWatchKbdRows) return;
  g_wk_sel = row;
  Row r{RowKind::Key, kWatchKbdItems[row].label, kWatchKbdItems[row].cvar};
  BeginCapture(r);
  g_wk_capture = true;
}
// Each controller poll (also while capturing, when the rest of the poll is skipped).
void WatchKbdCaptureTick() {
  if (!g_wk_capture) return;
  UpdateCapture();
  if (!g_capture_cvar) {
    g_wk_capture = false;
    if (g_persist) g_persist();
  }
}
}  // namespace ge

// Pause menu > Help & Options > Keyboard Controls: the watch's Controls page
// (tab 2, drawn by sub_820C8420) shows the keyboard binds instead of the pad
// diagram while ge::WatchKeyboardPage() is set. Hooked right after the page's
// setup (0x820C8444); returning true skips the pad diagram (jumps to the epilogue).
// The watch face is drawn in a smaller space than the menus: about 427 x 240,
// where x 160 is the middle of the face (one unit is three pixels at 720p).
bool ge_watch_keyboard_page() {
  if (!ge::WatchKeyboardPage()) return false;
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (!EnsureGuestBlocks(base)) return false;
  const PPCContext saved = *ctx;
  {
    GuestFrame frame(*ctx);
    const uint32_t sp = frame.sp;
    const uint32_t font = LD32(base, 0x830884C8u);  // the watch's text font
    const float text_scale = LDF32(base, kTextScale);
    // The watch's own colours (0x8242463C): light green labels, white values.
    constexpr uint32_t kLabelColour = 0xA0FFA0F0u, kValueColour = 0xFFFFFFFFu;
    auto measure = [&](uint32_t str) {
      ctx->r3.u64 = sp + 0x100;
      ctx->r4.u64 = font;
      ctx->r5.u64 = str;
      ctx->f1.f64 = text_scale;
      ctx->r6.u64 = sp + 0x110;
      ctx->r7.u64 = sp + 0x114;
      sub_8209BD50(*ctx, base);
      ctx->r3.u64 = sp + 0x100;
      sub_8209B890(*ctx, base);
      return static_cast<int32_t>(LD32(base, sp + 0x110));
    };
    auto draw = [&](uint32_t str, float x, float y, uint32_t colour) {
      ctx->r3.u64 = sp + 0x120;
      ctx->r4.u64 = font;
      ctx->r5.u64 = str;
      sub_8209BCD8(*ctx, base);
      ctx->r4.u64 = ctx->r3.u64;
      ctx->r3.u64 = sp + 0x130;
      ctx->f1.f64 = x;
      ctx->f2.f64 = y;
      sub_8209BA00(*ctx, base);
      ctx->r3.u64 = sp + 0x140;
      ctx->r4.u64 = sp + 0x130;
      ctx->r5.u64 = colour;
      sub_8209BB58(*ctx, base);
      for (uint32_t off : {0x140u, 0x130u, 0x120u}) {
        ctx->r3.u64 = sp + off;
        sub_8209B890(*ctx, base);
      }
    };
    constexpr uint32_t kSlotTitle = kCustomBase + 100, kSlotItems = kCustomBase + 102;
    // Lower case is drawn as small capitals by this font, like the game's own text.
    SetCustomText(base, kSlotTitle, "Keyboard Controls");
    const uint32_t title = LookupText(*ctx, base, kSlotTitle);
    draw(title, 160.0f - measure(title) * 0.5f, 34.0f, kValueColour);
    constexpr float kLabelX = 62.0f, kValueX = 152.0f, kFirstY = 54.0f, kStep = 10.5f;
    // The chosen row: a soft bar like the watch's own lists.
    DrawBox(*ctx, base, 0x50C05050u, kLabelX - 4.0f, kFirstY + kStep * g_wk_sel - 1.5f, 268.0f,
            kFirstY + kStep * g_wk_sel + 8.0f);
    int n = 0;
    for (const WatchKbdItem& it : kWatchKbdItems) {
      std::string value = it.cvar ? KeyText(it.cvar) : "Mouse";
      if (value == "Wheel Up, Wheel Down") value = "Mouse Wheel";
      if (g_wk_capture && n == g_wk_sel) value = "Press a key";
      SetCustomText(base, kSlotItems + n * 2, it.label);
      SetCustomText(base, kSlotItems + n * 2 + 1, value);
      const float y = kFirstY + kStep * n;
      draw(LookupText(*ctx, base, kSlotItems + n * 2), kLabelX, y, kLabelColour);
      draw(LookupText(*ctx, base, kSlotItems + n * 2 + 1), kValueX, y, kValueColour);
      ++n;
    }
  }
  *ctx = saved;
  return true;
}

REXCVAR_DEFINE_BOOL(ge_boot_cover, true, "Transient",
                    "Paint the output black until the screen ratio is set (first frames of the run)");
REXCVAR_DEFINE_BOOL(ge_hide_button_icons, false, "Transient",
                    "No controller is connected: the renderer blanks the A / B button icons");

// Called every controller poll: once a second, look for a connected gamepad and
// tell the renderer (which blanks the menus' A / B icons when there is none).
void ge_controller_tick() {
  static int countdown = 0;
  if (--countdown > 0) return;
  countdown = 60;
  typedef DWORD(WINAPI * GetStateFn)(DWORD, void*);
  static GetStateFn get_state = [] {
    HMODULE m = LoadLibraryW(L"xinput1_4.dll");
    if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
    return m ? reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState")) : nullptr;
  }();
  if (!get_state) return;  // cannot tell: leave the icons alone
  bool connected = false;
  alignas(8) unsigned char state[16] = {};
  for (DWORD i = 0; i < 4 && !connected; ++i) connected = get_state(i, state) == ERROR_SUCCESS;
  // Testing aid: a file named fakepad.txt beside the exe stands for a controller.
  if (!connected) {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      std::wstring dir(exe, n);
      dir = dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"fakepad.txt";
      connected = GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
  }
  static int last = -1;
  if (last == static_cast<int>(connected)) return;
  last = static_cast<int>(connected);
  rex::cvar::SetFlagByName("ge_hide_button_icons", connected ? "false" : "true");
  REXKRNL_INFO("GECONTROLLER {}", connected ? "connected: button icons shown" : "none: button icons hidden");
}

void ge_main_menu_version() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  static const std::string text = ReadVersionText();
  if (text.empty() || !EnsureGuestBlocks(base)) return;
  GuestFrame frame(*ctx);
  SetCustomText(base, kTxtVersion, text);
  // The menu font, a little smaller, in translucent black so it reads as a
  // quiet grey on the paper. The font's scale is state the menu pages rely on
  // (they never set it), so the draw restores it. The size the game asks the
  // font for is its own 16 (+8 of the font object); 8238E0D0 turns a size into
  // the scale kept at +6540 / +6544 of the font data.
  const uint32_t font = LD32(base, kFontPtr);
  const uint32_t font_data = LD32(base, font);
  const float saved_x = LDF32(base, font_data + 6540), saved_y = LDF32(base, font_data + 6544);
  const float size = LDF32(base, font + 8);
  DrawText(*ctx, base, frame.sp, LookupText(*ctx, base, kTxtVersion), kVersionX, kVersionY,
           kVersionAlpha, size > 1.0f ? size * kVersionScale : 0.0f);
  STF32(base, font_data + 6540, saved_x);
  STF32(base, font_data + 6544, saved_y);
}

// Draw: before the crosshair is drawn at the end of the results screen
// (0x82101C50 with games listed, 0x82101D70 with none).
void ge_lan_draw_refresh(PPCRegister& /*r3*/) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (!EnsureGuestBlocks(base) || LD32(base, kLanState) != 2) return;
  const uint64_t keep3 = ctx->r3.u64, keep4 = ctx->r4.u64, keep5 = ctx->r5.u64;
  GuestFrame frame(*ctx);
  const uint32_t sp = frame.sp;
  SetCustomText(base, kTxtRefresh, "Refresh");
  const float y = LanRefreshY(*ctx, base);
  const uint32_t label = LookupText(*ctx, base, kTxtRefresh);
  int w = 0, h = 0;
  MeasureText(*ctx, base, sp, label, w, h);
  g_lan_refresh_w = w;
  if (LanRefreshHover(base, y, w)) {
    const float pad = LDF32(base, kHighlightPad);
    DrawBox(*ctx, base, 50, kLanRefreshX - pad, y - pad, kLanRefreshX - pad + w + 4,
            y - pad + h + 4);
  }
  DrawText(*ctx, base, sp, label, kLanRefreshX, y, 255);
  ctx->r3.u64 = keep3;
  ctx->r4.u64 = keep4;
  ctx->r5.u64 = keep5;
}

// Update (sub_820F3310 entry): a select press on Refresh restarts the search
// and skips this frame's update, so the press does not also join or create.
bool ge_lan_refresh_update(PPCRegister& /*r3*/) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (!g_string_block || LD32(base, kLanState) != 2) return false;
  const uint64_t keep3 = ctx->r3.u64;
  GuestFrame frame(*ctx);
  const uint32_t sp = frame.sp;
  const float y = LanRefreshY(*ctx, base);
  int w = 0, h = 0;
  MeasureText(*ctx, base, sp, LookupText(*ctx, base, kTxtRefresh), w, h);
  // With no games listed the screen treats a select anywhere (except on the
  // Previous tab) as Create Match, so while the crosshair is on Refresh the
  // screen's own update is skipped (after moving the crosshair and letting Back
  // through).
  bool skip = false;
  if (LanRefreshHover(base, y, w) && !ButtonPressed(*ctx, base, kBackMask)) {
    skip = true;
    if (ButtonPressed(*ctx, base, kSelectMask)) {
      PlaySelectSound(*ctx, base);
      ST32(base, kLanState, 0);
      REXKRNL_INFO("GELAN join screen: refresh");
    } else {
      ctx->r3.u64 = 0;
      sub_820EC448(*ctx, base);  // the screen's crosshair update
      ST32(base, kLanHoveredGame, 0xFFFFFFFFu);  // no game row highlighted
    }
  }
  ctx->r3.u64 = keep3;
  return skip;
}

// Draw: with no games, Create Match is highlighted whenever the crosshair is
// not on the Previous tab; not while it is on Refresh (0x82101CD4, jumps past
// the highlight to 0x82101D34).
bool ge_lan_skip_create_highlight(PPCRegister& /*r11*/) {
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  return LD32(base, kLanState) == 2 && LanRefreshHover(base, 175.0f, g_lan_refresh_w);
}
