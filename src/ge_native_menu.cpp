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
inline uint16_t LD16(uint8_t* b, uint32_t ga) {
  uint16_t v; std::memcpy(&v, b + ga, 2); return __builtin_bswap16(v);
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
constexpr uint32_t kStartMask = 0x1000;      // Start (Enter on the keyboard)

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
void CycleName(int dir);  // the player card's name list (below)
int g_name_arrow = 0;     // Online Settings' Player Name arrow pointed at: -1, 1, 0 none


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

// Online Settings' Player Name row: "<" and ">" either side of the name step
// through the same name list as the player card. The ">" stays where a
// 15-character name ends, so it does not move as names change length.
// Menu units, as the page's rows.
constexpr float kNameArrowLeftX = kValueX - 18.0f;
constexpr float kNameArrowRightX = kValueX + 112.0f;
int NameArrowAt(uint8_t* base) {
  const float cx = LDF32(base, kCursorX);
  if (cx >= kNameArrowLeftX - 4.0f && cx < kValueX - 3.0f) return -1;
  if (cx >= kNameArrowRightX - 4.0f && cx <= kNameArrowRightX + 16.0f) return 1;
  return 0;
}
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
      rows.push_back(Row{RowKind::Toggle, "Online Server Play:", "ge_online_enable"});
      Row server{RowKind::Text, "Server Address:", "ge_online_server"};
      server.max_len = 40;
      rows.push_back(server);
      Row port{RowKind::Text, "Server Port:", "ge_online_port"};
      port.max_len = 5;
      rows.push_back(port);
      // Last before Done: lower down, the "<" ">" either side of the name are
      // clear of the crest picture at the page's top right.
      Row name{RowKind::Text, "Player Name:", "ge_username"};
      name.max_len = 15;
      rows.push_back(name);
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
    if (r.cvar && std::strcmp(r.cvar, "ge_username") == 0 && !(g_capture_text && g_capture_cvar == r.cvar)) {
      SetCustomText(base, kTxtFirstDynamic + 120, "<");
      SetCustomText(base, kTxtFirstDynamic + 121, ">");
      const float pad = LDF32(base, kHighlightPad);
      if (static_cast<int>(i) == g_hover && g_name_arrow) {
        const float ax = g_name_arrow < 0 ? kNameArrowLeftX : kNameArrowRightX;
        DrawBox(ctx, base, 50, ax - pad, y - pad, ax - pad + 14.0f, y - pad + 20.0f);
      }
      DrawText(ctx, base, sp, LookupText(ctx, base, kTxtFirstDynamic + 120), kNameArrowLeftX, y, 255);
      DrawText(ctx, base, sp, LookupText(ctx, base, kTxtFirstDynamic + 121), kNameArrowRightX, y, 255);
    }
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
  g_name_arrow = 0;
  if (g_hover >= 0 && rows[g_hover].cvar && std::strcmp(rows[g_hover].cvar, "ge_username") == 0) {
    g_name_arrow = NameArrowAt(base);
    int step = 0;
    if (g_name_arrow && ButtonPressed(ctx, base, kSelectMask)) step = g_name_arrow;
    if (ButtonPressed(ctx, base, 0x0202)) step = -1;  // left / right on the row
    if (ButtonPressed(ctx, base, 0x0101)) step = 1;
    if (step) {
      PlaySelectSound(ctx, base);
      CycleName(step);
      return false;
    }
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

// --- Player card (menu 37): the local player's name can be changed ---------
// In the LAN lobby, selecting your own name opens your player card (draw
// sub_820FE9E8, update sub_82106358), which shows the name in large letters
// above Character, Control Style, Look Up/Down and Autoaim. Pointing at the
// name highlights it like a row; selecting it types a new one there (Enter
// keeps it, Esc cancels), the same Player Name the Online Settings page edits.
// Arrows either side of the name (or left / right while on it) step through
// the names the launcher picks a first name from (packaging/player-names.txt),
// so a controller can change it too. Saving goes through ge_username, so the
// profile, ge.toml and (through the game's own rename message) the other
// players all get it.
#include "ge_player_names.h"
namespace {
constexpr uint32_t kCardPlayer = 0x82F610CCu;  // the player the card shows
constexpr uint32_t kCardState = 0x8272C5DCu;   // row / sub-page: 4 handicap, 6 and 7 character list
constexpr uint32_t kCardNameX = 0x8272C608u;   // where the name is drawn (ints, menu units)
constexpr uint32_t kCardNameY = 0x8272C60Cu;
bool g_card_capture = false;   // typing a name on the card
int g_card_release = 0;        // capture over: frames left before the card takes input again
bool g_card_hover = false;
bool g_card_local = false;
int g_card_name_w = 100;       // measured when the name is drawn
int g_card_name_h = 16;
int g_card_arrow = 0;          // arrow pointed at: -1 left, 1 right, 0 none
int g_card_arrow_w = 10;       // measured "<" width
constexpr uint32_t kTextArrowLeft = 0xF0C5u;
constexpr uint32_t kTextArrowRight = 0xF0C6u;
constexpr uint32_t kLeftMask = 0x0202;   // the card's own left / right tests
constexpr uint32_t kRightMask = 0x0101;
constexpr float kArrowGap = 8.0f;        // between the name and an arrow

// Arrow boxes (menu units) for the name at (x, y).
float ArrowLeftX(float x) { return x - kArrowGap - static_cast<float>(g_card_arrow_w); }
// The right arrow stays put while names change length (a click on it must not
// land on the next, shorter name): far enough right for a 15-character name.
constexpr float kNameRoom = 105.0f;
float ArrowRightX(float x) { return x + std::max(kNameRoom, static_cast<float>(g_card_name_w) + kArrowGap); }

// The next (dir 1) or previous (-1) name in the list after the current one.
void CycleName(int dir) {
  constexpr int count = static_cast<int>(sizeof(kGePlayerNames) / sizeof(kGePlayerNames[0]));
  if (count == 0) return;
  const std::string current = GetS("ge_username");
  int index = dir > 0 ? -1 : 0;
  for (int i = 0; i < count; ++i)
    if (current == kGePlayerNames[i]) index = i;
  index = (index + dir + count) % count;
  SetS("ge_username", kGePlayerNames[index]);
  REXKRNL_INFO("GENAME picked '{}' from the name list", kGePlayerNames[index]);
}

// A player is this machine's own when byte +12 of the object at +12 is set (sub_8216C140).
bool LocalPlayer(uint8_t* base, uint32_t player) {
  const uint32_t info = LD32(base, player + 12u);
  return info && base[info + 12u] != 0;
}
}  // namespace

// Update, hooked at 0x82106430 (before the card's row handling); true skips the
// rest of the update (jumps to its epilogue).
bool ge_card_update() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (g_card_capture) {
    UpdateCapture();
    if (!g_capture_cvar) {
      // Enter or Esc ended it; keep the pad input swallowed for a moment and
      // until the key is up, so it does not also press Start, A or Back on the card.
      g_card_capture = false;
      g_card_release = 20;  // about a third of a second, and until the keys are up
      ge::SetRebindCapturing(true);
    }
    return true;
  }
  if (g_card_release > 0) {
    if (--g_card_release > 0 || AnyKeyDown()) {
      g_card_release = std::max(g_card_release, 1);
      return true;
    }
    ge::SetRebindCapturing(false);
  }
  g_card_hover = false;
  g_card_arrow = 0;
  const uint32_t player = LD32(base, kCardPlayer);
  g_card_local = player && LocalPlayer(base, player);
  if (!g_card_local || g_capture_cvar) return false;
  const int32_t state = static_cast<int32_t>(LD32(base, kCardState));
  if (state == 4 || state == 6 || state == 7) return false;  // a sub-page is open
  const float x = static_cast<float>(static_cast<int32_t>(LD32(base, kCardNameX)));
  const float y = static_cast<float>(static_cast<int32_t>(LD32(base, kCardNameY)));
  const float cx = LDF32(base, kCursorX), cy = LDF32(base, kCursorY);
  const bool in_row = cy >= y - 6.0f && cy <= y + static_cast<float>(g_card_name_h) + 4.0f;
  if (in_row && cx >= ArrowLeftX(x) - 4.0f && cx < x - 2.0f) g_card_arrow = -1;
  if (in_row && cx > ArrowRightX(x) - 2.0f && cx <= ArrowRightX(x) + g_card_arrow_w + 4.0f) g_card_arrow = 1;
  g_card_hover = !g_card_arrow && in_row && cx >= x - 2.0f && cx <= x + static_cast<float>(g_card_name_w) + 2.0f;
  if ((!g_card_hover && !g_card_arrow) || ButtonPressed(*ctx, base, kBackMask)) return false;
  int step = 0;
  if (g_card_arrow && ButtonPressed(*ctx, base, kSelectMask)) step = g_card_arrow;
  if (ButtonPressed(*ctx, base, kLeftMask)) step = -1;
  if (ButtonPressed(*ctx, base, kRightMask)) step = 1;
  if (step) {
    PlaySelectSound(*ctx, base);
    CycleName(step);
    return true;
  }
  if (g_card_arrow || !ButtonPressed(*ctx, base, kSelectMask)) {
    // On the name: no card row is highlighted (the card would otherwise pick
    // Character), and only the crosshair moves.
    ST32(base, kCardState, 0xFFFFFFFFu);
    ctx->r3.u64 = 0;
    sub_820EC448(*ctx, base);  // the card's crosshair update (its update's tail)
    return true;
  }
  PlaySelectSound(*ctx, base);
  Row r{RowKind::Text, "Player Name:", "ge_username"};
  r.max_len = 15;
  BeginCapture(r);
  g_card_capture = true;
  REXKRNL_INFO("GENAME typing a new name on the player card");
  return true;
}

// Draw, hooked at 0x820FECAC: the card has just turned the player's name
// (player+768) into the wide string it draws, at r1+240 (17 characters).
// While typing, that string shows the name being typed; the name gets the row
// highlight when pointed at or being typed.
void ge_card_name_draw() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (!g_card_local) return;
  const uint32_t text = ctx->r1.u32 + 240u;
  if (g_card_capture) {
    const std::string shown = g_text_buffer.substr(0, 15) + "_";
    for (size_t i = 0; i < shown.size(); ++i) {
      base[text + i * 2] = 0;
      base[text + i * 2 + 1] = static_cast<uint8_t>(shown[i]);
    }
    base[text + shown.size() * 2] = 0;
    base[text + shown.size() * 2 + 1] = 0;
  }
  const PPCContext saved = *ctx;
  {
    GuestFrame frame(*ctx);
    int w = 0, h = 0;
    MeasureText(*ctx, base, frame.sp, text, w, h);
    if (w > 0) g_card_name_w = w;
    if (h > 0) g_card_name_h = h;
    const float x = static_cast<float>(static_cast<int32_t>(LD32(base, kCardNameX)));
    const float y = static_cast<float>(static_cast<int32_t>(LD32(base, kCardNameY)));
    const float pad = LDF32(base, kHighlightPad);
    if (g_card_hover || g_card_capture)
      DrawBox(*ctx, base, 50, x - pad, y - pad, x - pad + g_card_name_w + 4, y - pad + g_card_name_h + 4);
    if (!g_card_capture && EnsureGuestBlocks(base)) {
      SetCustomText(base, kTextArrowLeft, "<");
      SetCustomText(base, kTextArrowRight, ">");
      const uint32_t left = LookupText(*ctx, base, kTextArrowLeft);
      const uint32_t right = LookupText(*ctx, base, kTextArrowRight);
      int aw = 0, ah = 0;
      MeasureText(*ctx, base, frame.sp, left, aw, ah);
      if (aw > 0) g_card_arrow_w = aw;
      const float lx = ArrowLeftX(x), rx = ArrowRightX(x);
      if (g_card_arrow)
        DrawBox(*ctx, base, 50, (g_card_arrow < 0 ? lx : rx) - pad, y - pad,
                (g_card_arrow < 0 ? lx : rx) - pad + g_card_arrow_w + 4, y - pad + g_card_name_h + 4);
      DrawText(*ctx, base, frame.sp, left, lx, y, 255);
      DrawText(*ctx, base, frame.sp, right, rx, y, 255);
    }
  }
  *ctx = saved;
}

// Test pipe (ge_debug_input_file): "TEXT=name" types into the text field being
// edited and presses Enter, without touching the real keyboard.
namespace ge {
void TestEnterText(const std::string& text) {
  if (!g_capture_cvar || !g_capture_text) return;
  g_text_buffer = text.substr(0, static_cast<size_t>(g_capture_max));
  SetS(g_capture_cvar, g_text_buffer);
  EndCapture();
  g_capture_armed = false;
}
}  // namespace ge


// ===================================================== LAN lobby (menu 31) ===
// The page after a match's settings (update sub_82106F50, draw sub_82107690):
// the players' name tabs, the red status line, and on the host a START tab.
//
// What the original game does: a joining player is marked ready to start every
// lobby frame (sub_8217D900(1) at 0x821003C8), and the host's START marks the
// host ready, after which the session starts the match at once. The START tab
// was hard to press (most of it lies under the name tabs' hit area) and there
// was no sign of anything until the level had loaded.
//
// What it does now:
//   - Joining players start not ready. Start (Enter), or the READY tab drawn
//     where the host has START, toggles it. A player's ready flag is the
//     player's synced byte (player+1232, value at +12), so every machine sees it.
//   - Each name tab shows READY, NOT READY or HOST beside the name.
//   - The host's START works anywhere on the tab, or with Start anywhere on the
//     page, but only once every joining player is ready. It marks the host
//     ready as before, but the session's start (sub_8217FAF0) is held back for
//     3 seconds, so every machine shows "Starting match in 3, 2, 1": the other
//     machines see the host's player become ready (its synced ready byte).
//   - The status line says what is being waited for.
//   - Every machine plays the same music. Most multiplayer levels have no track
//     of their own, and each machine picked one at random (sub_821453C0). Now
//     the host picks it when its countdown starts and sends it in a shared
//     number of its player (player+1804, value at +12, set through
//     sub_82177480) that nothing uses in the lobby; it is set back to 0 a
//     second before the match starts. The other machines read it during the
//     countdown, and every machine plays that track (ge_match_music). A host
//     that sends nothing (an older version) leaves the pick random.
namespace {
constexpr uint32_t kTabHover = 0x82F60FA0u;       // START tab highlighted (set by the page each frame)
constexpr uint32_t kTextLobbyStatus = 0xF0C0u;    // custom texts (see SetCustomText)
constexpr uint32_t kTextReadyTab = 0xF0C1u;
constexpr uint32_t kTextReadyMark = 0xF0C2u;
constexpr uint32_t kTextHostMark = 0xF0C3u;
constexpr uint32_t kTextNotReadyMark = 0xF0C4u;
constexpr uint32_t kTextMoreWaiting = 0x9D3Fu;    // the game's "Waiting for more players"
constexpr uint64_t kCountdownMs = 3000;

bool g_client_ready = false;      // this machine's joining player is ready
uint64_t g_countdown_ms = 0;      // host: when its countdown began (0 = none)
uint64_t g_countdown_seen_ms = 0; // joining player: when the countdown bit appeared
int g_match_track = -1;           // index into the random-music list for the starting match, -1 = random
bool g_track_sent = false;        // host: the track is in its shared number (cleared before the start)
constexpr uint32_t kTrackVar = 1804u;               // host player's shared number carrying the track + 1
constexpr uint32_t kRandomMusicList = 0x82DEEAF0u;  // the multiplayer track ids, ending with 0
bool g_ready_tab_label = false;   // the START tab draw should say READY

uint32_t CustomTextAddr(uint32_t id) { return g_string_block + (id - kCustomBase) * kCustomChars * 2; }

bool IsHost(PPCContext& ctx, uint8_t* base) {
  sub_8215FCA8(ctx, base);
  return ctx.r3.u32 != 0;
}
bool CanStart(PPCContext& ctx, uint8_t* base) {  // enough players for the scenario
  sub_82103278(ctx, base);
  return (ctx.r3.u32 & 0xFF) != 0;
}
uint32_t SlotPlayer(PPCContext& ctx, uint8_t* base, uint32_t slot) {
  ctx.r3.u64 = slot;
  sub_820CA3E8(ctx, base);
  return ctx.r3.u32;
}
uint32_t HostPlayer(PPCContext& ctx, uint8_t* base) {  // as the "Waiting for %s" line finds it
  sub_82151C30(ctx, base);
  return ctx.r3.u32 ? LD32(base, ctx.r3.u32 + 20u) : 0;
}
bool PlayerReady(uint8_t* base, uint32_t player) { return base[player + 1232u + 12u] != 0; }

// Joining players (players of the other machines than the host's) not ready yet.
int PlayersNotReady(PPCContext& ctx, uint8_t* base) {
  const bool host = IsHost(ctx, base);
  const uint32_t host_player = HostPlayer(ctx, base);
  int count = 0;
  for (uint32_t slot = 0; slot < 4; ++slot) {
    const uint32_t p = SlotPlayer(ctx, base, slot);
    if (!p || p == host_player) continue;
    if (host && LocalPlayer(base, p)) continue;  // a second player on the host's machine
    if (!PlayerReady(base, p)) ++count;
  }
  return count;
}

bool OnStartTab(uint8_t* base) {  // the tab at the page's right edge (menu units)
  const float cx = LDF32(base, kCursorX), cy = LDF32(base, kCursorY);
  return cx >= 383.0f && cy >= 42.0f && cy <= 128.0f;
}

// The host's players ready or not (sub_8217D900): what the other machines see.
void SetHostReady(PPCContext& ctx, uint8_t* base, bool ready) {
  ctx.r3.u64 = ready ? 1 : 0;
  sub_8217D900(ctx, base);
}

// The host's shared number carrying the match's music (track index + 1, 0 = none).
void SetTrackVar(PPCContext& ctx, uint8_t* base, uint32_t value) {
  const uint32_t hp = HostPlayer(ctx, base);
  if (!hp) return;
  ctx.r3.u64 = hp + kTrackVar;
  ctx.r4.u64 = value;
  sub_82177480(ctx, base);
}

void CancelCountdown(PPCContext& ctx, uint8_t* base, const char* why) {
  REXKRNL_INFO("GELOBBY countdown cancelled ({})", why);
  g_countdown_ms = 0;
  SetHostReady(ctx, base, false);
  if (g_track_sent) SetTrackVar(ctx, base, 0);
  g_track_sent = false;
}

// Seconds left to show, 0 = no countdown on this machine.
int CountdownSeconds(PPCContext& ctx, uint8_t* base, bool host) {
  uint64_t began = 0;
  const uint32_t hp = HostPlayer(ctx, base);
  if (host) {
    began = g_countdown_ms;
    if (!began && hp && PlayerReady(base, hp)) return 1;  // counted down; the match is starting
    if (!began) g_match_track = -1;
  } else if (hp && PlayerReady(base, hp)) {
    if (!g_countdown_seen_ms) g_countdown_seen_ms = GetTickCount64();
    began = g_countdown_seen_ms;
    const uint32_t sent = LD32(base, hp + kTrackVar + 12u);
    if (sent != 0 && sent < 1000u && g_match_track != static_cast<int>(sent) - 1) {
      g_match_track = static_cast<int>(sent) - 1;  // kept once the host has cleared it
      REXKRNL_INFO("GELOBBY the host picked music track {}", g_match_track);
    }
  } else {
    g_countdown_seen_ms = 0;
    g_match_track = -1;
  }
  if (!began) return 0;
  const uint64_t elapsed = GetTickCount64() - began;
  return elapsed >= kCountdownMs ? 1 : static_cast<int>((kCountdownMs - elapsed + 999) / 1000);
}

// Text in a colour (RGBA) and size, as the lobby draws its status line: the
// text object gets a position, a colour, a size (sub_8209BBB8; the status line
// is 17) and a wrap width (sub_8209BC18, 400) before it is released, which
// draws it.
void DrawSmallText(PPCContext& ctx, uint8_t* base, uint32_t sp, uint32_t str, float x, float y,
                   uint32_t rgba, float size) {
  ctx.r3.u64 = sp + 0x120;
  ctx.r4.u64 = LD32(base, kFontPtr);
  ctx.r5.u64 = str;
  sub_8209BCD8(ctx, base);
  ctx.r4.u64 = ctx.r3.u64;
  ctx.r3.u64 = sp + 0x130;
  ctx.f1.f64 = x;
  ctx.f2.f64 = y;
  sub_8209BA00(ctx, base);
  ctx.r4.u64 = ctx.r3.u64;
  ctx.r3.u64 = sp + 0x140;
  ctx.r5.u64 = rgba;
  sub_8209BB58(ctx, base);
  ctx.r4.u64 = ctx.r3.u64;
  ctx.r3.u64 = sp + 0x150;
  ctx.f1.f64 = size;
  sub_8209BBB8(ctx, base);
  ctx.r4.u64 = ctx.r3.u64;
  ctx.r3.u64 = sp + 0x160;
  ctx.f1.f64 = 400.0;
  sub_8209BC18(ctx, base);
  for (uint32_t off : {0x160u, 0x150u, 0x140u, 0x130u, 0x120u}) {
    ctx.r3.u64 = sp + off;
    sub_8209B890(ctx, base);
  }
}

void ToggleClientReady(PPCContext& ctx, uint8_t* base) {
  g_client_ready = !g_client_ready;
  PlaySelectSound(ctx, base);
  REXKRNL_INFO("GELOBBY this player is {}", g_client_ready ? "ready" : "not ready");
}
}  // namespace

namespace ge {
// Each controller poll: outside the lobby nothing is pending.
void LobbyPollTick(uint8_t* base, uint32_t menu) {
  if (menu == 31u || menu == 37u) return;
  g_client_ready = false;
  g_countdown_ms = 0;
  g_countdown_seen_ms = 0;
  (void)base;
}
// Test pipe "READY": toggles this machine's ready, as Start does in the lobby.
void TestToggleReady() {
  g_client_ready = !g_client_ready;
  REXKRNL_INFO("GELOBBY this player is {} (test)", g_client_ready ? "ready" : "not ready");
}
}  // namespace ge

// Joining player's every-frame "ready" (0x821003C8, r3 = 1 there): only when the
// player said so.
void ge_client_ready(PPCRegister& r3) { r3.u64 = g_client_ready ? 1 : 0; }

// Host page, every frame (0x821070E4, before the name-tab test). Runs the
// countdown; true jumps to the page's START handling (0x82107210), which
// highlights the tab and, on select, asks to start (ge_lobby_start_request).
// Start pressed anywhere counts as select there: Start (0x1000 in the menus'
// button bits) is added to the select mask it tests (r17, normally 0xA000).
bool ge_lobby_host_start(PPCRegister& r17) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (g_countdown_ms) {
    if (!CanStart(*ctx, base)) {
      CancelCountdown(*ctx, base, "not enough players");
    } else if (PlayersNotReady(*ctx, base) > 0) {
      CancelCountdown(*ctx, base, "a player is no longer ready");
    } else if (g_track_sent && GetTickCount64() - g_countdown_ms >= kCountdownMs - 1000) {
      SetTrackVar(*ctx, base, 0);  // the others have it; the match starts with the number as it was
      g_track_sent = false;
    } else if (GetTickCount64() - g_countdown_ms >= kCountdownMs) {
      g_countdown_ms = 0;  // the session may start the match now (ge_session_start_gate)
      REXKRNL_INFO("GELOBBY countdown over: starting the match");
    }
  }
  if (!OnStartTab(base) && !ButtonPressed(*ctx, base, kStartMask)) return false;
  r17.u64 = r17.u32 | kStartMask;
  return true;
}

// The START handling's start (0x821072C0, before `li r3,1; bl 0x8217D900`):
// enough players and select pressed. True skips the immediate start; the
// countdown starts instead, once every joining player is ready.
bool ge_lobby_start_request() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (g_countdown_ms) return true;
  const int waiting = PlayersNotReady(*ctx, base);
  if (waiting > 0) {
    REXKRNL_INFO("GELOBBY START: {} player(s) not ready", waiting);
    return true;
  }
  int tracks = 0;
  while (tracks < 250 && LD16(base, kRandomMusicList + tracks * 2u) != 0) ++tracks;
  g_match_track = tracks > 0 ? static_cast<int>(GetTickCount64() % static_cast<uint64_t>(tracks)) : -1;
  g_countdown_ms = GetTickCount64();
  SetHostReady(*ctx, base, true);
  if (g_match_track >= 0) {
    SetTrackVar(*ctx, base, static_cast<uint32_t>(g_match_track) + 1u);
    g_track_sent = true;
  }
  REXKRNL_INFO("GELOBBY countdown started (music track {})", g_match_track);
  return true;
}

// Joining player's page, every frame (0x821072D8, before the name-tab test).
// The READY tab (where the host has START) and Start toggle ready; true skips
// the rest of the page's handling for this frame (jumps to 0x8210737C).
bool ge_lobby_client_ready() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (OnStartTab(base)) {
    ST32(base, kTabHover, 1);
    if (ButtonPressed(*ctx, base, kSelectMask) || ButtonPressed(*ctx, base, kStartMask)) ToggleClientReady(*ctx, base);
    return true;
  }
  if (ButtonPressed(*ctx, base, kStartMask)) {
    ToggleClientReady(*ctx, base);
    return true;
  }
  return false;
}

// Status line (draw, 0x8210777C: r28 = the text the page chose). The game's
// "Waiting for more players" stays; otherwise it says what the page waits for.
void ge_lobby_status_text(PPCRegister& r28) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (!EnsureGuestBlocks(base)) return;
  const PPCContext saved = *ctx;
  std::string text;
  {
    GuestFrame frame(*ctx);
    if (r28.u32 != LookupText(*ctx, base, kTextMoreWaiting)) {
      const bool host = IsHost(*ctx, base);
      const int seconds = CountdownSeconds(*ctx, base, host);
      const int waiting = PlayersNotReady(*ctx, base);
      if (seconds > 0) {
        text = "Starting match in " + std::to_string(seconds);
      } else if (host) {
        if (waiting > 0)
          text = "Waiting for " + std::to_string(waiting) + (waiting == 1 ? " player" : " players") + " to get ready";
      } else if (!g_client_ready) {
        text = "Waiting for you to ready up";
      } else {
        text = waiting > 0 ? "Ready - waiting for the others" : "Ready - waiting for the host to start";
      }
    }
  }
  *ctx = saved;
  if (text.empty()) return;
  SetCustomText(base, kTextLobbyStatus, text);
  r28.u64 = CustomTextAddr(kTextLobbyStatus);
}

// The START tab's label (0x820EACFC: r3 = the "START" text just looked up).
void ge_lobby_tab_label(PPCRegister& r3) {
  if (g_ready_tab_label && g_string_block) r3.u64 = CustomTextAddr(kTextReadyTab);
}

// Draw, after the status line (0x821078B0): the joining player's
// READY tab, and READY / NOT READY / HOST on the name tabs.
void ge_lobby_draw_extras() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (!EnsureGuestBlocks(base)) return;
  SetCustomText(base, kTextReadyTab, g_client_ready ? "NOT READY" : "READY");
  SetCustomText(base, kTextReadyMark, "READY");
  SetCustomText(base, kTextHostMark, "HOST");
  SetCustomText(base, kTextNotReadyMark, "NOT READY");
  const PPCContext saved = *ctx;
  {
    GuestFrame frame(*ctx);
    sub_8215FC88(*ctx, base);
    const bool lobby = ctx->r3.u32 == 4;
    const bool host = lobby && IsHost(*ctx, base);
    if (lobby && !host && CanStart(*ctx, base)) {
      g_ready_tab_label = true;
      sub_820EACD8(*ctx, base);  // the START tab, labelled READY
      g_ready_tab_label = false;
    }
    if (lobby) {
      const uint32_t host_player = HostPlayer(*ctx, base);
      // Name tabs (menu units): two columns, two rows; the mark sits at the
      // left end of the tab, in small letters level with the name, leaving room
      // for long names.
      static const float kTabX[2] = {64.0f, 218.0f};
      static const float kTabY[2] = {47.5f, 69.5f};  // centred on the names
      for (uint32_t slot = 0; slot < 4; ++slot) {
        const uint32_t p = SlotPlayer(*ctx, base, slot);
        if (!p) continue;
        uint32_t id = 0, color = 0;
        if (p == host_player) {
          id = kTextHostMark, color = 0x5A4A2AFFu;
        } else if (PlayerReady(base, p)) {
          id = kTextReadyMark, color = 0x2E7D32FFu;
        } else {
          id = kTextNotReadyMark, color = 0xC83232FFu;  // the status line's red
        }
        if (!id) continue;
        DrawSmallText(*ctx, base, frame.sp, LookupText(*ctx, base, id), kTabX[slot & 1], kTabY[slot >> 1], color,
                      6.0f);
      }
    }
  }
  *ctx = saved;
}

// The session's match start (sub_8217FAF0 entry): held back while the host's
// countdown runs; true jumps to its return (0x8217FB74).
bool ge_session_start_gate() {
  return g_countdown_ms != 0 && GetTickCount64() - g_countdown_ms < kCountdownMs;
}

// A multiplayer level's music (sub_821453C0, at its random pick 0x82145438:
// r30 = the track list, r31 = its length): in a match started from the LAN
// lobby, the track the host picked, so every machine plays the same one.
// True returns r3 (0x82145468).
bool ge_match_music(PPCRegister& r3, PPCRegister& r30, PPCRegister& r31) {
  if (g_match_track < 0 || static_cast<uint32_t>(g_match_track) >= r31.u32) return false;
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  r3.s64 = static_cast<int16_t>(LD16(base, r30.u32 + static_cast<uint32_t>(g_match_track) * 2u));
  REXKRNL_INFO("GELOBBY match music: track {} of {} (id {})", g_match_track, r31.u32, r3.s64);
  return true;
}
