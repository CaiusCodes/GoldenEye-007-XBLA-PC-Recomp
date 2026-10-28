// GoldenEye 007 XBLA PC Recomp - the setup window (Linux: SDL3 + Dear ImGui).
//
// The same classified dossier as the Windows window (setup_window.cpp): a manila
// folder on a dark desk, a ruled sheet inside it, typewriter text in brown-black
// ink and red rubber-stamp ink for what matters. One page: the package line
// (Browse Package or drag and drop), the import progress, the three optional
// extras and Play. Everything is painted with ImDrawList in the Windows window's
// 730x650 layout units, scaled to the window; ImGui only supplies the input
// (hover / press / keyboard navigation through invisible buttons).
#include "launcher.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace ge {
namespace {

// ------------------------------------------------------------- layout --
// Layout units of the Windows window (730x650); the window is 860x767 logical
// pixels, so one unit is about 1.18 pixels (the Windows kUiScale).
constexpr float kW = 730, kH = 650;
constexpr int kWindowW = 860, kWindowH = 767;
constexpr float kSheetX = 36, kSheetY = 54, kSheetW = 630, kSheetH = 566;
constexpr float kFolderX = 22, kFolderY = 40, kFolderW = 658, kFolderH = 592;
constexpr float kTabX = kFolderX + kFolderW, kTabTop = 72, kTabPitch = 140, kTabH = 130;
constexpr float kRowsY = 242, kRowPitch = 30;  // candidate rows when several packages are found
constexpr int kMaxRows = 4;
constexpr float kExtrasY = 404, kExtraFirst = 432, kExtraPitch = 42;

// Font sizes in layout units (the Windows point sizes * 96 / 72).
constexpr float kFBrand = 32, kFTag = 12, kFTitle = 14.667f, kFCaps = 12.667f, kFBody = 14, kFSmall = 13,
                kFFoot = 11.333f, kFButton = 12.667f, kFPlay = 18.667f, kFRowTitle = 14, kFRowDetail = 12.667f,
                kFTab = 14, kFSealBig = 21.333f, kFSealSmall = 10, kFStamp = 21.333f, kFFileTab = 10.667f;

enum Control { kNone = -1, kBrowse, kLeft, kPlay, kExtra0, kExtra1, kExtra2, kRow0 };

enum class Mode { Idle, Choosing, Importing, Ready };

enum class Align { Near, Center, Far };

namespace ink {
const uint32_t Desk = 0x1E150E, DeskLight = 0x3A2A1C, Manila = 0xD8C38E, ManilaDark = 0xC3A96C, TabOn = 0xE8D6A0,
               Paper = 0xF2EAD2, Text = 0x2B2418, Soft = 0x4A3F2A, Muted = 0x8A7A58, Phase = 0x6B5A38,
               Red = 0xAC1C22, Ok = 0x3F6B2A;
}

ImU32 Hex(uint32_t rgb, int alpha = 255) {
  return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}

ImU32 Lerp(uint32_t a, uint32_t b, float t) {
  auto ch = [&](int shift) { return static_cast<int>(((a >> shift) & 0xFF) * (1 - t) + ((b >> shift) & 0xFF) * t + 0.5f); };
  return IM_COL32(ch(16), ch(8), ch(0), 255);
}

struct RectL {
  float x, y, w, h;
};

struct ExtraDef {
  const char* title;
  const char* detail;
};
const ExtraDef kExtraDefs[3] = {
    {"START IN FULLSCREEN", "You can switch at any time in Help & Options > Video Settings."},
    {"CREATE DESKTOP ENTRY", "Adds GoldenEye 007 to your application menu; the entry opens this folder's game."},
    {"100% GAME COMPLETION", "Unlocks every mission on every difficulty, 007 mode and all cheats."},
};

const char* const kFontDirs[] = {"/usr/share/fonts/TTF",
                                 "/usr/share/fonts/truetype/dejavu",
                                 "/usr/share/fonts/dejavu",
                                 "/usr/share/fonts/truetype/liberation",
                                 "/usr/share/fonts/liberation",
                                 "/usr/share/fonts/liberation-fonts"};

const SDL_DialogFileFilter kBrowseFilters[] = {{"Xbox 360 package (any file)", "*"}};

struct Hit {
  bool clicked = false, hover = false, down = false, focus = false;
};

class SetupWindow {
 public:
  SetupWindow(const Paths& p, const SetupOptions& o) : p_(p), options_(o) {}
  int Run();

 private:
  // window / fonts
  bool Init();
  void Shutdown();
  void LoadIcon();
  void LoadFonts();
  ImFont* FindFont(std::initializer_list<const char*> names);
  void ApplyScale();

  // frame
  void Frame();
  void Draw();
  void DrawBackdrop();
  void DrawControls();
  Hit Place(Control id, const RectL& r, bool enabled);
  void Button(Control id, const char* text, bool enabled, bool primary, ImFont* font, float size);
  RectL ControlRect(Control id) const;
  bool Enabled(Control id) const;

  // drawing helpers (layout units)
  ImVec2 P(float x, float y) const { return ImVec2(x * k_, y * k_); }
  void Text(ImFont* font, float size, uint32_t color, const RectL& r, const std::string& s, Align a = Align::Near,
            bool wrap = false, bool vcenter = false, int alpha = 255);
  std::string Fit(ImFont* font, float px, const std::string& s, float max_width) const;
  void RotateFrom(int first_vertex, ImVec2 centre, float degrees);
  void Stamp(const char* text, float cx, float cy, float angle, int alpha);
  void Line(float x0, float y0, float x1, float y1, ImU32 color, float thickness);
  void Rect(float x, float y, float w, float h, ImU32 color, float thickness);
  void Fill(float x, float y, float w, float h, ImU32 color);

  // flow
  void Start();
  void Browse();
  void SelectPath(const fs::path& path);
  void StartImport(Source source);  // by value: it may come from candidates_, which is cleared
  void Poll();
  void OnProgress(int percent, const std::string& phase, const std::string& detail);
  void OnDone(const ImportResult& result);
  void Play();
  void RequestClose();
  void Click(Control id);
  void Wake();
  static void SDLCALL OnFileChosen(void* userdata, const char* const* files, int filter);

  const Paths& p_;
  SetupOptions options_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  Uint32 wake_event_ = 0;
  float k_ = 1;  // pixels per layout unit
  bool glyphs_checked_ = false;
  std::string dot_ = "·", ellipsis_ = "…", tick_ = "✓";
  ImFont *mono_ = nullptr, *brand_ = nullptr, *body_ = nullptr, *default_font_ = nullptr;
  ImDrawList* dl_ = nullptr;

  Mode mode_ = Mode::Idle;
  bool installed_ = false;
  bool imported_now_ = false;
  bool close_after_import_ = false;
  bool quit_ = false;
  int exit_code_ = 0;
  std::vector<Source> candidates_;
  std::string heading_, hint_, path_text_, status_, phase_, stats_, detail_;
  uint32_t status_color_ = ink::Muted, phase_color_ = ink::Phase;
  bool path_set_ = false;
  int progress_ = 0;
  bool show_bar_ = false;
  Extras want_, was_;
  Control clicked_ = kNone;
  bool escape_ = false, enter_ = false;

  std::atomic<bool> cancel_{false};
  std::thread worker_;
  std::atomic<bool> wake_pending_{false};
  std::atomic<bool> browsing_{false};
  std::mutex mutex_;  // guards everything below: written by the import thread / file dialog thread
  bool progress_dirty_ = false;
  int pending_percent_ = 0;
  std::string pending_phase_, pending_detail_;
  std::unique_ptr<ImportResult> pending_done_;
  std::string pending_path_;
  bool pending_path_ready_ = false;
};

// -------------------------------------------------------------- window --
void SetupWindow::LoadIcon() {
  const fs::path icon = p_.runtime / "icon.png";
  std::error_code ec;
  if (!fs::exists(icon, ec)) return;
  int w = 0, h = 0, channels = 0;
  unsigned char* pixels = stbi_load(Utf8(icon).c_str(), &w, &h, &channels, 4);
  if (!pixels) return;
  if (SDL_Surface* surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4)) {
    SDL_SetWindowIcon(window_, surface);
    SDL_DestroySurface(surface);
  }
  stbi_image_free(pixels);
}

ImFont* SetupWindow::FindFont(std::initializer_list<const char*> names) {
  for (const char* name : names) {
    for (const char* dir : kFontDirs) {
      const fs::path path = fs::path(dir) / name;
      std::error_code ec;
      if (fs::is_regular_file(path, ec)) {
        if (ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), 0.0f)) return font;
      }
    }
  }
  if (!default_font_) default_font_ = ImGui::GetIO().Fonts->AddFontDefault();
  return default_font_;
}

void SetupWindow::LoadFonts() {
  mono_ = FindFont({"DejaVuSansMono-Bold.ttf", "LiberationMono-Bold.ttf"});
  brand_ = FindFont({"DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf"});
  body_ = FindFont({"DejaVuSans.ttf", "LiberationSans-Regular.ttf"});
}

// One layout unit in pixels follows the window. On a desktop that scales
// without giving the window a denser framebuffer (X11) the window itself is
// made larger, so the page keeps its proportions and stays readable.
void SetupWindow::ApplyScale() {
  const float display = SDL_GetWindowDisplayScale(window_);
  const float density = SDL_GetWindowPixelDensity(window_);
  float s = display > 0 && density > 0 ? display / density : 1.0f;
  if (s < 1.05f) s = 1.0f;
  int w = 0, h = 0;
  SDL_GetWindowSize(window_, &w, &h);
  const int want_w = static_cast<int>(kWindowW * s + 0.5f), want_h = static_cast<int>(kWindowH * s + 0.5f);
  if (w != want_w || h != want_h) {
    SDL_SetWindowSize(window_, want_w, want_h);
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  }
}

bool SetupWindow::Init() {
  window_ = SDL_CreateWindow("GoldenEye 007 Setup", kWindowW, kWindowH, SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window_) {
    Log(std::string("Setup window: SDL_CreateWindow failed: ") + SDL_GetError());
    return false;
  }
  renderer_ = SDL_CreateRenderer(window_, nullptr);
  if (!renderer_) {
    Log(std::string("Setup window: SDL_CreateRenderer failed: ") + SDL_GetError());
    SDL_DestroyWindow(window_);
    window_ = nullptr;
    return false;
  }
  SDL_SetRenderVSync(renderer_, 1);
  wake_event_ = SDL_RegisterEvents(1);
  SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  LoadIcon();

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
  ImGui_ImplSDLRenderer3_Init(renderer_);
  LoadFonts();
  ApplyScale();
  return true;
}

void SetupWindow::Shutdown() {
  if (worker_.joinable()) {
    cancel_ = true;
    worker_.join();
  }
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer_);
  SDL_DestroyWindow(window_);
  renderer_ = nullptr;
  window_ = nullptr;
}

void SetupWindow::Wake() {
  if (wake_pending_.exchange(true)) return;
  SDL_Event event = {};
  event.type = wake_event_;
  SDL_PushEvent(&event);
}

// ------------------------------------------------------------ controls --
RectL SetupWindow::ControlRect(Control id) const {
  switch (id) {
    case kBrowse: return {516, 238, 150, 36};
    case kLeft: return {412, 571, 96, 40};
    case kPlay: return {520, 566, 146, 50};
    case kExtra0:
    case kExtra1:
    case kExtra2: return {54, kExtraFirst + (id - kExtra0) * kExtraPitch - 4, 612, kExtraPitch - 2};
    default: {
      const int row = id - kRow0;
      return {54, kRowsY + row * kRowPitch, 448, kRowPitch - 2};
    }
  }
}

bool SetupWindow::Enabled(Control id) const {
  switch (id) {
    case kBrowse: return mode_ != Mode::Importing && !imported_now_ && !browsing_.load();
    case kLeft: return !(mode_ == Mode::Importing && cancel_.load());
    case kPlay: return mode_ == Mode::Ready && installed_;
    case kExtra0:
    case kExtra1:
    case kExtra2: return true;
    default: return mode_ == Mode::Choosing && id - kRow0 < static_cast<int>(candidates_.size());
  }
}

// An invisible button over a control's rectangle: ImGui tracks hover, press
// and keyboard focus for it. Disabled controls are not placed at all.
Hit SetupWindow::Place(Control id, const RectL& r, bool enabled) {
  Hit hit;
  if (!enabled) return hit;
  ImGui::PushID(static_cast<int>(id));
  ImGui::SetCursorPos(P(r.x, r.y));
  hit.clicked = ImGui::InvisibleButton("##control", ImVec2(r.w * k_, r.h * k_));
  hit.hover = ImGui::IsItemHovered();
  hit.down = ImGui::IsItemActive() && hit.hover;
  hit.focus = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
  ImGui::PopID();
  if (hit.hover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (hit.focus) {
    // Keyboard focus: a dashed-looking inset outline in stamp red.
    dl_->AddRect(P(r.x + 3, r.y + 3), P(r.x + r.w - 3, r.y + r.h - 3), Hex(ink::Red, 200), 0, 0, 1.5f * k_);
  }
  if (hit.clicked) clicked_ = id;
  return hit;
}

// Buttons are typed labels on card: ink on paper, the main action in stamp red.
void SetupWindow::Button(Control id, const char* text, bool enabled, bool primary, ImFont* font, float size) {
  const RectL r = ControlRect(id);
  const Hit hit = Place(id, r, enabled);
  uint32_t back, fore, border;
  if (!enabled) {
    back = 0xE4DAC0, fore = 0xB2A585, border = 0xC8BA95;
  } else if (primary) {
    back = hit.down ? 0xE3C2A9 : hit.hover ? 0xEDD3BF : 0xF4E3D2, fore = ink::Red, border = ink::Red;
  } else {
    back = hit.down ? 0xD1C29A : hit.hover ? 0xDDD0AE : 0xEAE0C5, fore = ink::Text, border = 0x3D3322;
  }
  Fill(r.x, r.y, r.w, r.h, Hex(back));
  Rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, Hex(border), primary && enabled ? 3.0f : 2.0f);
  Text(font, size, fore, {r.x, r.y + 1, r.w, r.h}, text, Align::Center, false, true);
}

// ------------------------------------------------------------- drawing --
void SetupWindow::Fill(float x, float y, float w, float h, ImU32 color) {
  dl_->AddRectFilled(P(x, y), P(x + w, y + h), color);
}

void SetupWindow::Rect(float x, float y, float w, float h, ImU32 color, float thickness) {
  dl_->AddRect(P(x, y), P(x + w, y + h), color, 0, 0, thickness * k_);
}

void SetupWindow::Line(float x0, float y0, float x1, float y1, ImU32 color, float thickness) {
  dl_->AddLine(P(x0, y0), P(x1, y1), color, thickness * k_);
}

// The middle of a path is replaced by an ellipsis when it does not fit.
std::string SetupWindow::Fit(ImFont* font, float px, const std::string& s, float max_width) const {
  if (font->CalcTextSizeA(px, FLT_MAX, 0, s.c_str()).x <= max_width) return s;
  auto shorten = [&](size_t keep) {
    size_t head = keep / 2;
    while (head > 0 && (static_cast<unsigned char>(s[head]) & 0xC0) == 0x80) --head;
    size_t tail_start = s.size() - (keep - keep / 2);
    while (tail_start < s.size() && (static_cast<unsigned char>(s[tail_start]) & 0xC0) == 0x80) ++tail_start;
    return s.substr(0, head) + ellipsis_ + s.substr(tail_start);
  };
  size_t lo = 0, hi = s.size();
  while (lo < hi) {
    const size_t mid = (lo + hi + 1) / 2;
    if (font->CalcTextSizeA(px, FLT_MAX, 0, shorten(mid).c_str()).x <= max_width) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return shorten(lo);
}

void SetupWindow::Text(ImFont* font, float size, uint32_t color, const RectL& r, const std::string& s, Align a,
                       bool wrap, bool vcenter, int alpha) {
  if (s.empty()) return;
  const float px = size * k_, max_width = r.w * k_;
  const ImVec2 top_left = P(r.x, r.y);
  if (wrap) {
    dl_->AddText(font, px, top_left, Hex(color, alpha), s.c_str(), nullptr, max_width);
    return;
  }
  const std::string text = Fit(font, px, s, max_width);
  const ImVec2 extent = font->CalcTextSizeA(px, FLT_MAX, 0, text.c_str());
  float x = top_left.x, y = top_left.y;
  if (a == Align::Far) x += max_width - extent.x;
  if (a == Align::Center) x += (max_width - extent.x) / 2;
  if (vcenter) y += (r.h * k_ - extent.y) / 2;
  dl_->AddText(font, px, ImVec2(std::floor(x + 0.5f), std::floor(y + 0.5f)), Hex(color, alpha), text.c_str());
}

// Vertices added since first_vertex are turned about `centre` (clockwise, like
// the Windows window's RotateTransform): the ImGui wiki's rotate snippet.
void SetupWindow::RotateFrom(int first_vertex, ImVec2 centre, float degrees) {
  const float a = degrees * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
  for (int i = first_vertex; i < dl_->VtxBuffer.Size; ++i) {
    ImVec2& pos = dl_->VtxBuffer[i].pos;
    const float x = pos.x - centre.x, y = pos.y - centre.y;
    pos = ImVec2(centre.x + x * c - y * s, centre.y + x * s + y * c);
  }
}

void SetupWindow::Stamp(const char* text, float cx, float cy, float angle, int alpha) {
  const float px = kFStamp * k_;
  const ImVec2 extent = brand_->CalcTextSizeA(px, FLT_MAX, 0, text);
  const float w = extent.x + 16 * k_, h = extent.y + 4 * k_;
  const ImVec2 c = P(cx, cy);
  const int first = dl_->VtxBuffer.Size;
  dl_->AddRect(ImVec2(c.x - w / 2, c.y - h / 2), ImVec2(c.x + w / 2, c.y + h / 2), Hex(ink::Red, alpha), 0, 0, 3 * k_);
  dl_->AddText(brand_, px, ImVec2(c.x - extent.x / 2, c.y - extent.y / 2), Hex(ink::Red, alpha), text);
  RotateFrom(first, c, angle);
}

void SetupWindow::Frame() {
  ImGui_ImplSDLRenderer3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
  Draw();
  ImGui::Render();
  // Draw in window units; SDL scales them to pixels (a 150% Wayland display has
  // a 1.5 framebuffer scale). Fonts are rasterised at that density by ImGui.
  const ImGuiIO& io = ImGui::GetIO();
  SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
  SDL_SetRenderDrawColor(renderer_, 0x1E, 0x15, 0x0E, 255);
  SDL_RenderClear(renderer_);
  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
  SDL_RenderPresent(renderer_);
}

void SetupWindow::Draw() {
  ImGuiIO& io = ImGui::GetIO();
  k_ = io.DisplaySize.x / kW;
  if (!glyphs_checked_) {
    glyphs_checked_ = true;
    if (!body_->IsGlyphInFont(0x2713)) tick_ = "OK";
    if (!body_->IsGlyphInFont(0x2026)) ellipsis_ = "...";
    if (!body_->IsGlyphInFont(0x00B7)) dot_ = "-";
  }
  escape_ = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
  enter_ = ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !io.NavVisible;

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::Begin("##setup", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
  dl_ = ImGui::GetWindowDrawList();
  DrawBackdrop();
  DrawControls();
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void SetupWindow::DrawBackdrop() {
  // The desk, lit from the top left.
  const ImU32 mid = Lerp(ink::DeskLight, ink::Desk, 0.5f);
  dl_->AddRectFilledMultiColor(P(0, 0), P(kW, kH), Hex(ink::DeskLight), mid, Hex(ink::Desk), mid);

  // Step tabs down the folder's right edge; done steps get a tick.
  const int stage = mode_ == Mode::Ready ? 2 : mode_ == Mode::Importing ? 1 : 0;
  const char* tabs[3] = {"PACKAGE", "IMPORT", "PLAY"};
  for (int i = 0; i < 3; ++i) {
    const float ty = kTabTop + i * kTabPitch;
    const float reach = i == stage ? 42.0f : 34.0f;
    const ImU32 fill = Hex(i == stage ? ink::TabOn : i < stage ? ink::Manila : ink::ManilaDark);
    dl_->AddRectFilled(P(kTabX - 10, ty), P(kTabX + reach, ty + kTabH), fill, 6 * k_);
    dl_->AddRect(P(kTabX - 10, ty), P(kTabX + reach, ty + kTabH), IM_COL32(60, 45, 20, 90), 6 * k_, 0, 1.0f);
    if (i < stage) {
      // A done step gets a tick above its name (drawn: no font needs a tick glyph).
      const float cx = kTabX + reach / 2, top = ty + 12;
      const ImVec2 points[3] = {P(cx - 6, top + 6), P(cx - 1.5f, top + 11), P(cx + 7, top)};
      dl_->AddPolyline(points, 3, Hex(ink::Ok), 0, 2.4f * k_);
    }
    const float px = kFTab * k_;
    const ImVec2 extent = mono_->CalcTextSizeA(px, FLT_MAX, 0, tabs[i]);
    const ImVec2 c = P(kTabX + reach / 2, ty + kTabH / 2);
    const int first = dl_->VtxBuffer.Size;
    dl_->AddText(mono_, px, ImVec2(c.x - extent.x / 2, c.y - extent.y / 2),
                 Hex(i == stage ? ink::Text : i < stage ? ink::Soft : ink::Muted), tabs[i]);
    RotateFrom(first, c, 90);
  }

  // The folder: its file tab, a soft shadow, then the manila board.
  dl_->AddRectFilled(P(kFolderX + 4, kFolderY + 7), P(kFolderX + 4 + kFolderW, kFolderY + 7 + kFolderH),
                     IM_COL32(0, 0, 0, 110), 8 * k_);
  dl_->AddRectFilled(P(kFolderX + 30, kFolderY - 22), P(kFolderX + 30 + 214, kFolderY - 22 + 30), Hex(0xD0B87C), 8 * k_);
  Text(mono_, kFFileTab, 0x5A4A2A, {kFolderX + 44, kFolderY - 16, 190, 16}, "FILE 584108A9  " + dot_ + "  MI6");
  constexpr int kBands = 24;  // the board's gradient, one clipped band at a time
  for (int b = 0; b < kBands; ++b) {
    const float y0 = kFolderY + kFolderH * b / kBands, y1 = kFolderY + kFolderH * (b + 1) / kBands;
    dl_->PushClipRect(P(kFolderX, y0), P(kFolderX + kFolderW, y1), true);
    dl_->AddRectFilled(P(kFolderX, kFolderY), P(kFolderX + kFolderW, kFolderY + kFolderH),
                       Lerp(ink::Manila, ink::ManilaDark, (b + 0.5f) / kBands), 8 * k_);
    dl_->PopClipRect();
  }

  // The sheet: cream paper, blue rules, a red margin line.
  Fill(kSheetX, kSheetY, kSheetW, kSheetH, Hex(ink::Paper));
  for (float y = kSheetY + 30; y < kSheetY + kSheetH - 6; y += 30)
    Line(kSheetX, y, kSheetX + kSheetW, y, IM_COL32(150, 170, 200, 55), 1);
  Line(kSheetX + 10, kSheetY, kSheetX + 10, kSheetY + kSheetH, IM_COL32(172, 28, 34, 70), 1);
  Rect(kSheetX, kSheetY, kSheetW, kSheetH, IM_COL32(60, 45, 20, 60), 1);

  // The MI6 seal where a photo would go.
  dl_->AddCircle(P(105, 113), 45 * k_, Hex(0x5B4B2D), 0, 2 * k_);
  dl_->AddCircle(P(105, 113), 39 * k_, Hex(0x5B4B2D), 0, 2 * k_);
  Text(brand_, kFSealBig, 0x5B4B2D, {60, 92, 90, 28}, "MI6", Align::Center, false, true);
  Text(mono_, kFSealSmall, 0x5B4B2D, {60, 118, 90, 16}, "PC", Align::Center, false, true);

  // Heading.
  Text(brand_, kFBrand, ink::Text, {164, 64, 400, 48}, "GOLDENEYE 007");
  Text(mono_, kFTag, ink::Muted, {170, 114, 340, 20}, "PC CONVERSION  " + dot_ + "  V" + kVersion);
  const char* title = mode_ == Mode::Ready       ? "RE: READY FOR DUTY"
                      : mode_ == Mode::Importing ? "RE: IMPORTING YOUR GAME"
                                                 : "RE: SET UP YOUR GAME";
  Text(mono_, kFTitle, ink::Text, {170, 140, 380, 22}, title);
  Stamp("CLASSIFIED", 578, 150, -6, 200);
}

void SetupWindow::DrawControls() {
  // Package section.
  Text(body_, kFBody, ink::Text, {56, 190, 450, 24}, heading_);
  Text(body_, kFSmall, ink::Muted, {56, 214, 610, 22}, hint_);
  if (mode_ == Mode::Choosing) {
    for (int i = 0; i < static_cast<int>(candidates_.size()) && i < kMaxRows; ++i) {
      const Control id = static_cast<Control>(kRow0 + i);
      const RectL r = ControlRect(id);
      const Hit hit = Place(id, r, Enabled(id));
      if (hit.hover) Fill(r.x, r.y, r.w, r.h, IM_COL32(172, 28, 34, hit.down ? 90 : 60));
      dl_->AddCircle(P(r.x + 13, r.y + 14), 7 * k_, Hex(ink::Text), 0, 1.6f * k_);
      const Source& s = candidates_[i];
      const std::string text = (s.kind == SourceKind::PreviousInstall ? "Previous install  " : "") + Utf8(s.path);
      Text(body_, kFSmall, ink::Text, {r.x + 28, r.y + 5, r.w - 120, 22}, text);
      Text(body_, kFSmall, ink::Muted, {r.x + r.w - 92, r.y + 5, 88, 22}, FormatMB(s.bytes), Align::Far);
    }
  } else {
    Line(56, 273, 500, 273, Hex(ink::Text), 1.6f);
    Text(body_, kFSmall, path_set_ ? ink::Text : ink::Muted, {56, 248, 444, 24}, path_text_);
    Text(body_, kFSmall, status_color_, {56, 280, 610, 42}, status_, Align::Near, true);
  }
  Button(kBrowse, imported_now_ ? "IMPORTED" : "BROWSE PACKAGE", Enabled(kBrowse), false, mono_, kFButton);

  // Progress: phase on the left, live counts on the right, an inked bar.
  if (mode_ != Mode::Choosing) {
    Text(mono_, kFCaps, phase_color_, {56, 330, 380, 22}, phase_);
    Text(body_, kFSmall, ink::Muted, {346, 330, 320, 22}, stats_, Align::Far);
    if (show_bar_) {
      Rect(56, 354, 610, 16, Hex(ink::Text), 1.4f);
      const float width = 606.0f * progress_ / 100.0f;
      if (width > 1) {
        // The inked bar: dark ground with diagonal hatching.
        Fill(58, 356, width, 12, Hex(0x3D3322));
        dl_->PushClipRect(P(58, 356), P(58 + width, 368), true);
        for (float x = 58 - 12; x < 58 + width; x += 4)
          Line(x, 368, x + 12, 356, Hex(ink::Text), 1.5f);
        dl_->PopClipRect();
      }
    }
    Text(body_, kFSmall, ink::Muted, {56, 374, 610, 22}, detail_);
  }
  if (mode_ == Mode::Ready) Stamp("APPROVED", 592, 318, -9, 170);

  // Extras: a typed box that gets a red X, a title and a one-line explanation.
  Text(mono_, kFCaps, ink::Text, {56, kExtrasY, 610, 22}, "OPTIONAL EXTRAS  " + dot_ + "  APPLIED WHEN YOU PRESS PLAY");
  Line(56, kExtrasY + 22, 666, kExtrasY + 22, IM_COL32(43, 36, 24, 120), 1);
  const bool on[3] = {want_.fullscreen, want_.shortcut, want_.complete};
  for (int i = 0; i < 3; ++i) {
    const float y = kExtraFirst + i * kExtraPitch;
    const Control id = static_cast<Control>(kExtra0 + i);
    const RectL r = ControlRect(id);
    const Hit hit = Place(id, r, Enabled(id));
    if (hit.hover) Fill(r.x, r.y, r.w, r.h, IM_COL32(107, 90, 56, hit.down ? 70 : 40));
    Rect(58, y + 3, 20, 20, Hex(ink::Text), 1.6f);
    if (on[i]) {
      Line(60, y + 5, 77, y + 22, Hex(ink::Red), 2.6f);
      Line(77, y + 4, 61, y + 23, Hex(ink::Red), 2.6f);
    }
    Text(mono_, kFRowTitle, ink::Text, {94, y - 1, 570, 22}, kExtraDefs[i].title);
    Text(body_, kFRowDetail, ink::Soft, {94, y + 19, 570, 20}, kExtraDefs[i].detail);
  }

  // Footer and the two buttons.
  Text(mono_, kFFoot, ink::Muted, {56, 584, 340, 18}, "XBOX LIVE ARCADE  " + dot_ + "  TITLE ID 584108A9");
  const char* left = mode_ == Mode::Importing ? (cancel_.load() ? "CANCELLING" : "CANCEL") : "CLOSE";
  Button(kLeft, left, Enabled(kLeft), false, mono_, kFButton);
  Button(kPlay, "PLAY", Enabled(kPlay), true, mono_, kFPlay);
}

// ---------------------------------------------------------------- flow --
void SetupWindow::Start() {
  std::string missing;
  installed_ = IsInstalled(p_, &missing);
  // The extras start unticked on a first setup, where an unticked box changes
  // nothing. Reopened with --setup they show what is set, and unticking one
  // switches it off.
  was_ = installed_ ? CurrentExtras(p_, options_.shortcut_folder) : Extras{};
  want_ = was_;
  if (!options_.package.empty()) {
    SelectPath(options_.package);
    return;
  }
  if (installed_) {
    mode_ = Mode::Ready;
    heading_ = "GoldenEye 007 is installed in this folder";
    hint_ = "Press Play, change your extras, or import your package again to repair the game data.";
    path_text_ = "resources/assets";
    path_set_ = true;
    status_ = tick_ + "  Game data imported. Saves and settings are in resources/userdata.";
    status_color_ = ink::Ok;
    phase_.clear();
    return;
  }
  heading_ = "Select your Xbox 360 GoldenEye 007 package";
  hint_ = "Looking beside GoldenEye 007" + ellipsis_;
  path_text_ = "No package selected";
  path_set_ = false;
  Frame();
  std::vector<Source> found = DetectSources(p_);
  Log("Detected " + std::to_string(found.size()) + " package(s) or previous install(s).");
  hint_ = "Drop the package (the 30BA9271... file) or its folder onto this window, or browse for it.";
  if (found.size() == 1) {
    StartImport(found[0]);
  } else if (found.size() > 1) {
    candidates_ = found;
    if (candidates_.size() > static_cast<size_t>(kMaxRows)) candidates_.resize(kMaxRows);
    mode_ = Mode::Choosing;
    heading_ = "More than one was found. Choose the one to import:";
  } else {
    status_ = "No package was found beside GoldenEye 007 or in a \"Game Package\" folder.";
    status_color_ = ink::Muted;
  }
}

// The dialog runs on another thread: its answer is parked under the mutex and
// the window's loop is woken to pick it up.
void SDLCALL SetupWindow::OnFileChosen(void* userdata, const char* const* files, int) {
  SetupWindow* self = static_cast<SetupWindow*>(userdata);
  if (files && files[0]) {
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->pending_path_ = files[0];
    self->pending_path_ready_ = true;
  }
  self->browsing_ = false;
  self->Wake();
}

void SetupWindow::Browse() {
  if (browsing_.exchange(true)) return;
  const std::string start = Utf8(p_.root) + "/";
  SDL_ShowOpenFileDialog(&SetupWindow::OnFileChosen, this, window_, kBrowseFilters, 1, start.c_str(), false);
}

void SetupWindow::SelectPath(const fs::path& path) {
  if (mode_ == Mode::Importing || imported_now_) return;
  path_text_ = Utf8(path);
  path_set_ = true;
  phase_.clear();
  stats_.clear();
  detail_.clear();
  show_bar_ = false;
  std::string why;
  std::vector<Source> found = SourcesIn(path, &why);
  if (found.empty()) {
    Log("Rejected " + Utf8(path) + ": " + why);
    if (mode_ == Mode::Choosing) mode_ = Mode::Idle;
    if (installed_) mode_ = Mode::Ready;
    status_ = why;
    status_color_ = ink::Red;
    return;
  }
  if (found.size() == 1) {
    StartImport(found[0]);
    return;
  }
  candidates_ = found;
  if (candidates_.size() > static_cast<size_t>(kMaxRows)) candidates_.resize(kMaxRows);
  mode_ = Mode::Choosing;
  heading_ = "That folder holds more than one. Choose the one to import:";
}

void SetupWindow::StartImport(Source source) {
  if (worker_.joinable()) worker_.join();
  mode_ = Mode::Importing;
  candidates_.clear();
  path_text_ = Describe(source);
  path_set_ = true;
  heading_ = "Importing your game data";
  hint_ = "Your package is only read, never changed. Nothing is installed outside this folder.";
  status_ = tick_ + "  GoldenEye 007  " + dot_ + "  " +
            (source.kind == SourceKind::PreviousInstall ? "previous install" : "Xbox LIVE Arcade package") + "  " + dot_ +
            "  " + FormatMB(source.bytes);
  status_color_ = ink::Ok;
  phase_ = "PREPARING";
  phase_color_ = ink::Phase;
  stats_.clear();
  detail_.clear();
  progress_ = 0;
  show_bar_ = true;
  cancel_ = false;
  const fs::path saves_from = source.kind == SourceKind::PreviousInstall ? source.path : FindPreviousInstall(p_);
  worker_ = std::thread([this, source, saves_from]() {
    auto result = std::make_unique<ImportResult>(
        Import(p_, source, saves_from, cancel_, [this](int percent, const std::string& phase, const std::string& detail) {
          {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_percent_ = percent;
            pending_phase_ = phase;
            pending_detail_ = detail;
            progress_dirty_ = true;
          }
          Wake();
        }));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_done_ = std::move(result);
    }
    Wake();
  });
}

// Runs on the UI thread after a wake-up (or a timeout): takes over what the
// import thread and the file dialog left behind.
void SetupWindow::Poll() {
  wake_pending_ = false;
  int percent = 0;
  std::string phase, detail, chosen;
  bool progress = false, have_path = false;
  std::unique_ptr<ImportResult> done;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (progress_dirty_) {
      progress = true;
      progress_dirty_ = false;
      percent = pending_percent_;
      phase = pending_phase_;
      detail = pending_detail_;
    }
    done = std::move(pending_done_);
    if (pending_path_ready_) {
      have_path = true;
      pending_path_ready_ = false;
      chosen = pending_path_;
    }
  }
  if (progress && mode_ == Mode::Importing) OnProgress(percent, phase, detail);
  if (done) OnDone(*done);
  if (have_path) SelectPath(PathFromUtf8(chosen));
}

void SetupWindow::OnProgress(int percent, const std::string& phase, const std::string& detail) {
  progress_ = std::clamp(percent, 0, 100);
  phase_ = phase;
  stats_ = detail;
}

void SetupWindow::OnDone(const ImportResult& result) {
  if (worker_.joinable()) worker_.join();
  stats_.clear();
  if (result.ok) {
    mode_ = Mode::Ready;
    installed_ = true;
    imported_now_ = true;
    heading_ = "GoldenEye 007 is ready";
    hint_ = "Your package is no longer needed. Next time GoldenEye 007 starts the game straight away.";
    progress_ = 100;
    phase_ = "READY FOR DUTY";
    phase_color_ = ink::Ok;
    detail_ = result.migrated.empty() ? std::string("Game data imported into resources/assets.") : result.migrated;
    status_ = tick_ + "  Imported and verified. Press Play when you are ready.";
    status_color_ = ink::Ok;
    SDL_FlashWindow(window_, SDL_FLASH_UNTIL_FOCUSED);
  } else {
    mode_ = installed_ ? Mode::Ready : Mode::Idle;
    show_bar_ = false;
    phase_ = result.cancelled ? "IMPORT CANCELLED" : "IMPORT DID NOT COMPLETE";
    phase_color_ = ink::Red;
    status_ = result.message;
    status_color_ = result.cancelled ? ink::Muted : ink::Red;
    detail_ = result.cancelled ? "" : "Your package was not changed. Details: resources/runtime/logs/setup.log";
    heading_ = installed_ ? "GoldenEye 007 is installed in this folder" : "Select your Xbox 360 GoldenEye 007 package";
    hint_ = "Drop the package (the 30BA9271... file) or its folder onto this window, or browse for it.";
  }
  cancel_ = false;
  if (close_after_import_) quit_ = true;
}

void SetupWindow::Play() {
  const std::string note = ApplyExtras(p_, want_, was_, options_.shortcut_folder, [this](const fs::path& link) {
    const std::string text =
        "There is already a GoldenEye 007 application menu entry that opens a different copy:\n\n" + Utf8(link) +
        "\n\nReplace it with one for this folder?";
    const SDL_MessageBoxButtonData ordered[2] = {
        {0, 1, "Yes"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No"},
    };
    SDL_MessageBoxData box = {};
    box.flags = SDL_MESSAGEBOX_INFORMATION;
    box.window = window_;
    box.title = "GoldenEye 007";
    box.message = text.c_str();
    box.numbuttons = 2;
    box.buttons = ordered;
    int chosen = 0;
    return SDL_ShowMessageBox(&box, &chosen) && chosen == 1;
  });
  was_ = want_;
  if (!note.empty()) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "GoldenEye 007", note.c_str(), window_);
  std::string error;
  if (!LaunchGame(p_, options_.game_args, &error)) {
    Log("FAILED to start the game: " + error);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "GoldenEye 007", error.c_str(), window_);
    return;
  }
  Log("Play: game started.");
  quit_ = true;
}

void SetupWindow::RequestClose() {
  if (mode_ == Mode::Importing) {
    // Closing stops the import first; the window goes once it has cleaned up.
    close_after_import_ = true;
    cancel_ = true;
    return;
  }
  quit_ = true;
}

void SetupWindow::Click(Control id) {
  switch (id) {
    case kBrowse: Browse(); break;
    case kLeft:
      if (mode_ == Mode::Importing) {
        cancel_ = true;
      } else {
        RequestClose();
      }
      break;
    case kPlay: Play(); break;
    case kExtra0: want_.fullscreen = !want_.fullscreen; break;
    case kExtra1: want_.shortcut = !want_.shortcut; break;
    case kExtra2: want_.complete = !want_.complete; break;
    default:
      if (id >= kRow0 && id - kRow0 < static_cast<int>(candidates_.size())) StartImport(candidates_[id - kRow0]);
      break;
  }
}

int SetupWindow::Run() {
  if (!Init()) return -1;
  heading_ = "Select your Xbox 360 GoldenEye 007 package";
  SDL_ShowWindow(window_);
  SDL_RaiseWindow(window_);
  Frame();
  Start();

  while (!quit_) {
    SDL_Event event;
    const bool importing = mode_ == Mode::Importing;
    if (SDL_WaitEventTimeout(&event, importing ? 33 : 250)) {
      do {
        ImGui_ImplSDL3_ProcessEvent(&event);
        switch (event.type) {
          case SDL_EVENT_QUIT:
          case SDL_EVENT_WINDOW_CLOSE_REQUESTED: RequestClose(); break;
          case SDL_EVENT_DROP_FILE:
            if (event.drop.data) SelectPath(PathFromUtf8(event.drop.data));
            break;
          case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: ApplyScale(); break;
          default: break;
        }
      } while (SDL_PollEvent(&event));
    }
    Poll();
    if (quit_) break;
    Frame();
    if (escape_ && Enabled(kLeft)) Click(kLeft);
    if (enter_ && Enabled(kPlay)) Click(kPlay);
    if (clicked_ != kNone) {
      const Control id = clicked_;
      clicked_ = kNone;
      if (Enabled(id)) Click(id);
    }
  }
  Shutdown();
  return exit_code_;
}

}  // namespace

int RunSetupWindow(const Paths& p, const SetupOptions& options) {
  Log("Setup window opened" + (options.reason.empty() ? std::string() : " (" + options.reason + ")"));
  SetupWindow window(p, options);
  return window.Run();
}

}  // namespace ge
