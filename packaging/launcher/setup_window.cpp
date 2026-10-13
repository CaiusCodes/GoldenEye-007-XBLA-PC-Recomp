// GoldenEye 007 XBLA PC Recomp - the setup window.
//
// A classified dossier, after the game's own main menu: a manila folder on a
// dark desk, a ruled sheet inside it, typewriter text in brown-black ink and red
// rubber-stamp ink for what matters. One page: the package line (Browse Package
// or drag and drop), the import progress, the three optional extras and Play.
// Everything is painted with GDI+ in 96-DPI layout units and scaled for the
// monitor's DPI, so the window stays sharp at any Windows scaling.
#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus

#include "launcher.h"

#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <windowsx.h>
#include <shobjidl.h>

#include <memory>
#include <thread>

#include "resource.h"

namespace ge {
namespace {

using namespace Gdiplus;

// ------------------------------------------------------------- layout --
constexpr float kW = 730, kH = 650;
constexpr float kUiScale = 1.18f;  // drawn 18% larger than the 96-DPI layout
constexpr float kSheetX = 36, kSheetY = 54, kSheetW = 630, kSheetH = 566;
constexpr float kFolderX = 22, kFolderY = 40, kFolderW = 658, kFolderH = 592;
constexpr float kTabX = kFolderX + kFolderW, kTabTop = 72, kTabPitch = 140, kTabH = 130;
constexpr float kRowsY = 242, kRowPitch = 30;   // candidate rows when several packages are found
constexpr int kMaxRows = 4;
constexpr float kExtrasY = 404, kExtraFirst = 432, kExtraPitch = 42;

enum Control { kNone = -1, kBrowse, kLeft, kPlay, kExtra0, kExtra1, kExtra2, kRow0 };

enum class Mode { Idle, Choosing, Importing, Ready };

constexpr UINT WM_APP_START = WM_APP + 1;
constexpr UINT WM_APP_PROGRESS = WM_APP + 2;
constexpr UINT WM_APP_DONE = WM_APP + 3;

const wchar_t* const kClassName = L"GoldenEye007Setup";
const wchar_t kDot[] = L"·";
const wchar_t kEllipsis[] = L"…";

Color Hex(uint32_t rgb, BYTE alpha = 255) {
  return Color(alpha, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

namespace ink {
const uint32_t Desk = 0x1E150E, DeskLight = 0x3A2A1C, Manila = 0xD8C38E, ManilaDark = 0xC3A96C, TabOn = 0xE8D6A0,
               Paper = 0xF2EAD2, Text = 0x2B2418, Soft = 0x4A3F2A, Muted = 0x8A7A58, Phase = 0x6B5A38,
               Red = 0xAC1C22, Ok = 0x3F6B2A;
}

struct ProgressMsg {
  int percent;
  std::wstring phase, detail;
};

struct RectL {
  float x, y, w, h;
  bool Contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct ExtraDef {
  const wchar_t* title;
  const wchar_t* detail;
};
const ExtraDef kExtraDefs[3] = {
    {L"START IN FULLSCREEN", L"You can switch at any time in Help & Options > Video Settings."},
    {L"CREATE DESKTOP SHORTCUT", L"Adds a GoldenEye 007 icon to your desktop that opens this folder's game."},
    {L"100% GAME COMPLETION", L"Unlocks every mission on every difficulty, 007 mode and all cheats."},
};

class SetupWindow {
 public:
  SetupWindow(const Paths& p, const SetupOptions& o) : p_(p), options_(o) {}
  int Run(HINSTANCE instance);

 private:
  static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

  void CreateFonts();
  void ApplyScale(UINT dpi, const RECT* suggested);
  void Paint(HDC dc);
  void PaintButton(Graphics& g, Control id, const wchar_t* text, bool enabled, bool primary, Font* font);
  void PaintText(Graphics& g, const std::wstring& text, Font* font, uint32_t color, RectF rect,
                 StringAlignment align = StringAlignmentNear, bool wrap = false);
  void PaintStamp(Graphics& g, const wchar_t* text, float cx, float cy, float angle, BYTE alpha);

  RectL ControlRect(Control id) const;
  bool Enabled(Control id) const;
  Control HitTest(float x, float y) const;
  void Click(Control id);

  void Start();
  void Browse();
  void SelectPath(const fs::path& path);
  void StartImport(Source source);  // by value: it may come from candidates_, which is cleared
  void OnProgress(ProgressMsg* msg);
  void OnDone(ImportResult* result);
  void Play();
  void RequestClose();
  void Invalidate() { InvalidateRect(hwnd_, nullptr, FALSE); }

  const Paths& p_;
  SetupOptions options_;
  HWND hwnd_ = nullptr;
  HICON icon_ = nullptr;
  float scale_ = 1;
  Mode mode_ = Mode::Idle;
  bool installed_ = false;
  bool imported_now_ = false;
  bool close_after_import_ = false;
  std::vector<Source> candidates_;
  std::wstring heading_, hint_, path_text_, status_, phase_, stats_, detail_;
  uint32_t status_color_ = ink::Muted, phase_color_ = ink::Phase;
  bool path_set_ = false;
  int progress_ = 0;
  bool show_bar_ = false;
  Extras want_, was_;
  Control hover_ = kNone, pressed_ = kNone;
  bool tracking_ = false;
  std::atomic<bool> cancel_{false};
  std::thread worker_;
  std::unique_ptr<Font> brand_, tag_, title_, caps_, body_, small_, foot_, button_, play_, row_title_, row_detail_,
      tab_, seal_big_, seal_small_, stamp_, file_tab_;
};

// ------------------------------------------------------------ helpers --
void RoundedPath(GraphicsPath& path, float x, float y, float w, float h, float r) {
  const float d = std::max(1.0f, r * 2);
  path.AddArc(x, y, d, d, 180, 90);
  path.AddArc(x + w - d, y, d, d, 270, 90);
  path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
  path.AddArc(x, y + h - d, d, d, 90, 90);
  path.CloseFigure();
}

// Fonts are sized in 96-DPI pixels (points * 96 / 72); the paint transform scales them.
std::unique_ptr<Font> MakeFont(const wchar_t* family, float points, int style) {
  return std::make_unique<Font>(family, points * 96.0f / 72.0f, style, UnitPixel);
}

void SetupWindow::CreateFonts() {
  const wchar_t* type = L"Courier New";
  brand_ = MakeFont(L"Arial Black", 24, FontStyleRegular);
  tag_ = MakeFont(type, 9, FontStyleBold);
  title_ = MakeFont(type, 11, FontStyleBold);
  caps_ = MakeFont(type, 9.5f, FontStyleBold);
  body_ = MakeFont(L"Segoe UI Semibold", 10.5f, FontStyleRegular);
  small_ = MakeFont(L"Segoe UI", 9.75f, FontStyleRegular);
  foot_ = MakeFont(type, 8.5f, FontStyleRegular);
  button_ = MakeFont(type, 9.5f, FontStyleBold);
  play_ = MakeFont(type, 14, FontStyleBold);
  row_title_ = MakeFont(type, 10.5f, FontStyleBold);
  row_detail_ = MakeFont(L"Segoe UI", 9.5f, FontStyleRegular);
  tab_ = MakeFont(type, 10.5f, FontStyleBold);
  seal_big_ = MakeFont(L"Arial Black", 16, FontStyleRegular);
  seal_small_ = MakeFont(type, 7.5f, FontStyleBold);
  stamp_ = MakeFont(L"Arial Black", 16, FontStyleRegular);
  file_tab_ = MakeFont(type, 8, FontStyleBold);
}

// --------------------------------------------------------------- scale --
// The layout scale for a monitor: its DPI times the 18% enlargement, reduced
// when the window would not fit the monitor's work area (small laptop screens).
void SetupWindow::ApplyScale(UINT dpi, const RECT* suggested) {
  HMONITOR monitor = suggested ? MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST)
                               : (hwnd_ ? MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST)
                                        : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
  MONITORINFO info = {sizeof(info)};
  GetMonitorInfoW(monitor, &info);
  const RECT work = info.rcWork;
  const float base = dpi / 96.0f;
  const float fit = std::min((work.right - work.left) * 0.95f / kW, (work.bottom - work.top) * 0.95f / kH);
  scale_ = std::max(0.5f, std::min(base * kUiScale, fit));
  const int w = static_cast<int>(kW * scale_ + 0.5f), h = static_cast<int>(kH * scale_ + 0.5f);
  int x, y;
  if (suggested) {
    x = suggested->left;
    y = suggested->top;
  } else {
    x = work.left + ((work.right - work.left) - w) / 2;
    y = work.top + ((work.bottom - work.top) - h) / 2;
  }
  SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
  const int r = static_cast<int>(12 * scale_);
  SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, w + 1, h + 1, r * 2, r * 2), TRUE);
  Invalidate();
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
    case kBrowse: return mode_ != Mode::Importing && !imported_now_;
    case kLeft: return !(mode_ == Mode::Importing && cancel_.load());
    case kPlay: return mode_ == Mode::Ready && installed_;
    case kExtra0:
    case kExtra1:
    case kExtra2: return true;
    default: return mode_ == Mode::Choosing && id - kRow0 < static_cast<int>(candidates_.size());
  }
}

Control SetupWindow::HitTest(float x, float y) const {
  std::vector<Control> ids = {kBrowse, kLeft, kPlay, kExtra0, kExtra1, kExtra2};
  if (mode_ == Mode::Choosing) {
    for (int i = 0; i < static_cast<int>(candidates_.size()) && i < kMaxRows; ++i)
      ids.push_back(static_cast<Control>(kRow0 + i));
  }
  for (Control id : ids) {
    if (ControlRect(id).Contains(x, y) && Enabled(id)) return id;
  }
  return kNone;
}

// --------------------------------------------------------------- paint --
void SetupWindow::PaintText(Graphics& g, const std::wstring& text, Font* font, uint32_t color, RectF rect,
                            StringAlignment align, bool wrap) {
  if (text.empty()) return;
  StringFormat format;
  format.SetAlignment(align);
  format.SetLineAlignment(StringAlignmentNear);
  format.SetTrimming(wrap ? StringTrimmingWord : StringTrimmingEllipsisPath);
  if (!wrap) format.SetFormatFlags(StringFormatFlagsNoWrap);
  SolidBrush brush(Hex(color));
  g.DrawString(text.c_str(), -1, font, rect, &format, &brush);
}

void SetupWindow::PaintStamp(Graphics& g, const wchar_t* text, float cx, float cy, float angle, BYTE alpha) {
  GraphicsState state = g.Save();
  g.TranslateTransform(cx, cy);
  g.RotateTransform(angle);
  RectF bounds;
  g.MeasureString(text, -1, stamp_.get(), PointF(0, 0), &bounds);
  const float w = bounds.Width + 16, h = bounds.Height + 4;
  Pen pen(Hex(ink::Red, alpha), 3);
  g.DrawRectangle(&pen, -w / 2, -h / 2, w, h);
  StringFormat center;
  center.SetAlignment(StringAlignmentCenter);
  center.SetLineAlignment(StringAlignmentCenter);
  SolidBrush brush(Hex(ink::Red, alpha));
  g.DrawString(text, -1, stamp_.get(), RectF(-w / 2, -h / 2, w, h), &center, &brush);
  g.Restore(state);
}

// Buttons are typed labels on card: ink on paper, the main action in stamp red.
void SetupWindow::PaintButton(Graphics& g, Control id, const wchar_t* text, bool enabled, bool primary, Font* font) {
  const RectL r = ControlRect(id);
  uint32_t back, fore, border;
  if (!enabled) {
    back = 0xE4DAC0, fore = 0xB2A585, border = 0xC8BA95;
  } else if (primary) {
    back = pressed_ == id ? 0xE3C2A9 : hover_ == id ? 0xEDD3BF : 0xF4E3D2, fore = ink::Red, border = ink::Red;
  } else {
    back = pressed_ == id ? 0xD1C29A : hover_ == id ? 0xDDD0AE : 0xEAE0C5, fore = ink::Text, border = 0x3D3322;
  }
  SolidBrush fill(Hex(back));
  g.FillRectangle(&fill, r.x, r.y, r.w, r.h);
  Pen pen(Hex(border), primary && enabled ? 3.0f : 2.0f);
  g.DrawRectangle(&pen, r.x + 1, r.y + 1, r.w - 2, r.h - 2);
  StringFormat center;
  center.SetAlignment(StringAlignmentCenter);
  center.SetLineAlignment(StringAlignmentCenter);
  center.SetFormatFlags(StringFormatFlagsNoWrap);
  SolidBrush brush(Hex(fore));
  g.DrawString(text, -1, font, RectF(r.x, r.y + 1, r.w, r.h), &center, &brush);
}

void SetupWindow::Paint(HDC dc) {
  RECT client;
  GetClientRect(hwnd_, &client);
  HDC memory = CreateCompatibleDC(dc);
  HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
  HGDIOBJ old = SelectObject(memory, bitmap);
  {
    Graphics g(memory);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.ScaleTransform(scale_, scale_);

    // The desk, lit from the top left.
    LinearGradientBrush desk(RectF(0, 0, kW, kH), Hex(ink::DeskLight), Hex(ink::Desk), 60.0f);
    g.FillRectangle(&desk, RectF(-1, -1, kW + 2, kH + 2));

    // Step tabs down the folder's right edge; done steps get a tick.
    const int stage = mode_ == Mode::Ready ? 2 : mode_ == Mode::Importing ? 1 : 0;
    const wchar_t* tabs[3] = {L"PACKAGE", L"IMPORT", L"PLAY"};
    for (int i = 0; i < 3; ++i) {
      const float ty = kTabTop + i * kTabPitch;
      const float reach = i == stage ? 42.0f : 34.0f;
      GraphicsPath tab;
      RoundedPath(tab, kTabX - 10, ty, reach + 10, kTabH, 6);
      SolidBrush fill(Hex(i == stage ? ink::TabOn : i < stage ? ink::Manila : ink::ManilaDark));
      g.FillPath(&fill, &tab);
      Pen edge(Color(90, 60, 45, 20), 1);
      g.DrawPath(&edge, &tab);
      const std::wstring label = tabs[i];
      if (i < stage) {
        // A done step gets a tick above its name (drawn, as Courier New has no tick glyph).
        Pen tick(Hex(ink::Ok), 2.4f);
        tick.SetStartCap(LineCapRound);
        tick.SetEndCap(LineCapRound);
        tick.SetLineJoin(LineJoinRound);
        const float cx = kTabX + reach / 2, top = ty + 12;
        const PointF points[3] = {PointF(cx - 6, top + 6), PointF(cx - 1.5f, top + 11), PointF(cx + 7, top)};
        g.DrawLines(&tick, points, 3);
      }
      GraphicsState state = g.Save();
      g.TranslateTransform(kTabX + reach / 2, ty + kTabH / 2);
      g.RotateTransform(90);
      StringFormat center;
      center.SetAlignment(StringAlignmentCenter);
      center.SetLineAlignment(StringAlignmentCenter);
      SolidBrush tab_ink(Hex(i == stage ? ink::Text : i < stage ? ink::Soft : ink::Muted));
      g.DrawString(label.c_str(), -1, tab_.get(), RectF(-kTabH / 2, -9, kTabH, 18), &center, &tab_ink);
      g.Restore(state);
    }

    // The folder: its file tab, a soft shadow, then the manila board.
    GraphicsPath shadow;
    RoundedPath(shadow, kFolderX + 4, kFolderY + 7, kFolderW, kFolderH, 8);
    SolidBrush shadow_fill(Color(110, 0, 0, 0));
    g.FillPath(&shadow_fill, &shadow);
    GraphicsPath file_tab;
    RoundedPath(file_tab, kFolderX + 30, kFolderY - 22, 214, 30, 8);
    SolidBrush file_tab_fill(Hex(0xD0B87C));
    g.FillPath(&file_tab_fill, &file_tab);
    SolidBrush file_ink(Hex(0x5A4A2A));
    g.DrawString((std::wstring(L"FILE 584108A9  ") + kDot + L"  MI6").c_str(), -1, file_tab_.get(),
                 PointF(kFolderX + 44, kFolderY - 16), &file_ink);
    GraphicsPath board;
    RoundedPath(board, kFolderX, kFolderY, kFolderW, kFolderH, 8);
    LinearGradientBrush board_fill(RectF(kFolderX, kFolderY, kFolderW, kFolderH), Hex(ink::Manila),
                                   Hex(ink::ManilaDark), 80.0f);
    g.FillPath(&board_fill, &board);

    // The sheet: cream paper, blue rules, a red margin line.
    SolidBrush paper(Hex(ink::Paper));
    g.FillRectangle(&paper, kSheetX, kSheetY, kSheetW, kSheetH);
    Pen rule(Color(55, 150, 170, 200), 1);
    for (float y = kSheetY + 30; y < kSheetY + kSheetH - 6; y += 30) g.DrawLine(&rule, kSheetX, y, kSheetX + kSheetW, y);
    Pen margin(Color(70, 172, 28, 34), 1);
    g.DrawLine(&margin, kSheetX + 10, kSheetY, kSheetX + 10, kSheetY + kSheetH);
    Pen sheet_edge(Color(60, 60, 45, 20), 1);
    g.DrawRectangle(&sheet_edge, kSheetX, kSheetY, kSheetW, kSheetH);

    // The MI6 seal where a photo would go.
    Pen seal(Hex(0x5B4B2D), 2);
    g.DrawEllipse(&seal, 60.0f, 68.0f, 90.0f, 90.0f);
    g.DrawEllipse(&seal, 66.0f, 74.0f, 78.0f, 78.0f);
    StringFormat center;
    center.SetAlignment(StringAlignmentCenter);
    center.SetLineAlignment(StringAlignmentCenter);
    SolidBrush seal_ink(Hex(0x5B4B2D));
    g.DrawString(L"MI6", -1, seal_big_.get(), RectF(60, 92, 90, 28), &center, &seal_ink);
    g.DrawString(L"PC", -1, seal_small_.get(), RectF(60, 118, 90, 16), &center, &seal_ink);

    // Heading.
    PaintText(g, L"GOLDENEYE 007", brand_.get(), ink::Text, RectF(164, 64, 400, 48));
    PaintText(g, std::wstring(L"PC CONVERSION  ") + kDot + L"  V" + kVersion, tag_.get(), ink::Muted,
              RectF(170, 114, 340, 20));
    const wchar_t* title = mode_ == Mode::Ready      ? L"RE: READY FOR DUTY"
                           : mode_ == Mode::Importing ? L"RE: IMPORTING YOUR GAME"
                                                      : L"RE: SET UP YOUR GAME";
    PaintText(g, title, title_.get(), ink::Text, RectF(170, 140, 380, 22));
    PaintStamp(g, L"CLASSIFIED", 578, 150, -6, 200);

    // Package section.
    PaintText(g, heading_, body_.get(), ink::Text, RectF(56, 190, 450, 24));
    PaintText(g, hint_, small_.get(), ink::Muted, RectF(56, 214, 610, 22));
    if (mode_ == Mode::Choosing) {
      for (int i = 0; i < static_cast<int>(candidates_.size()) && i < kMaxRows; ++i) {
        const RectL r = ControlRect(static_cast<Control>(kRow0 + i));
        if (hover_ == kRow0 + i) {
          SolidBrush hover(Color(60, 172, 28, 34));
          g.FillRectangle(&hover, r.x, r.y, r.w, r.h);
        }
        Pen ring(Hex(ink::Text), 1.6f);
        g.DrawEllipse(&ring, r.x + 6, r.y + 7, 14.0f, 14.0f);
        const Source& s = candidates_[i];
        std::wstring text = (s.kind == SourceKind::PreviousInstall ? L"Previous install  " : L"") + s.path.wstring();
        PaintText(g, text, small_.get(), ink::Text, RectF(r.x + 28, r.y + 5, r.w - 120, 22));
        PaintText(g, FormatMB(s.bytes), small_.get(), ink::Muted, RectF(r.x + r.w - 92, r.y + 5, 88, 22),
                  StringAlignmentFar);
      }
    } else {
      Pen line(Hex(ink::Text), 1.6f);
      g.DrawLine(&line, 56.0f, 273.0f, 500.0f, 273.0f);
      PaintText(g, path_text_, small_.get(), path_set_ ? ink::Text : ink::Muted, RectF(56, 248, 444, 24));
      PaintText(g, status_, small_.get(), status_color_, RectF(56, 280, 610, 42), StringAlignmentNear, true);
    }
    const wchar_t* browse = imported_now_ ? L"IMPORTED" : L"BROWSE PACKAGE";
    PaintButton(g, kBrowse, browse, Enabled(kBrowse), false, button_.get());

    // Progress: phase on the left, live counts on the right, an inked bar.
    if (mode_ != Mode::Choosing) {
      PaintText(g, phase_, caps_.get(), phase_color_, RectF(56, 330, 380, 22));
      PaintText(g, stats_, small_.get(), ink::Muted, RectF(346, 330, 320, 22), StringAlignmentFar);
      if (show_bar_) {
        Pen bar(Hex(ink::Text), 1.4f);
        g.DrawRectangle(&bar, 56.0f, 354.0f, 610.0f, 16.0f);
        const float width = 606.0f * progress_ / 100.0f;
        if (width > 1) {
          HatchBrush fill(HatchStyleDarkUpwardDiagonal, Hex(ink::Text), Hex(0x3D3322));
          g.FillRectangle(&fill, 58.0f, 356.0f, width, 12.0f);
        }
      }
      PaintText(g, detail_, small_.get(), ink::Muted, RectF(56, 374, 610, 22));
    }
    if (mode_ == Mode::Ready) PaintStamp(g, L"APPROVED", 592, 318, -9, 170);

    // Extras: a typed box that gets a red X, a title and a one-line explanation.
    PaintText(g, std::wstring(L"OPTIONAL EXTRAS  ") + kDot + L"  APPLIED WHEN YOU PRESS PLAY", caps_.get(), ink::Text,
              RectF(56, kExtrasY, 610, 22));
    Pen divider(Color(120, 43, 36, 24), 1);
    g.DrawLine(&divider, 56.0f, kExtrasY + 22, 666.0f, kExtrasY + 22);
    const bool on[3] = {want_.fullscreen, want_.shortcut, want_.complete};
    for (int i = 0; i < 3; ++i) {
      const float y = kExtraFirst + i * kExtraPitch;
      if (hover_ == kExtra0 + i) {
        SolidBrush hover(Color(40, 107, 90, 56));
        const RectL r = ControlRect(static_cast<Control>(kExtra0 + i));
        g.FillRectangle(&hover, r.x, r.y, r.w, r.h);
      }
      Pen box(Hex(ink::Text), 1.6f);
      g.DrawRectangle(&box, 58.0f, y + 3, 20.0f, 20.0f);
      if (on[i]) {
        Pen x(Hex(ink::Red), 2.6f);
        x.SetStartCap(LineCapRound);
        x.SetEndCap(LineCapRound);
        g.DrawLine(&x, 60.0f, y + 5, 77.0f, y + 22);
        g.DrawLine(&x, 77.0f, y + 4, 61.0f, y + 23);
      }
      PaintText(g, kExtraDefs[i].title, row_title_.get(), ink::Text, RectF(94, y - 1, 570, 22));
      PaintText(g, kExtraDefs[i].detail, row_detail_.get(), ink::Soft, RectF(94, y + 19, 570, 20));
    }

    // Footer and the two buttons.
    PaintText(g, std::wstring(L"XBOX LIVE ARCADE  ") + kDot + L"  TITLE ID 584108A9", foot_.get(), ink::Muted,
              RectF(56, 584, 340, 18));
    const wchar_t* left = mode_ == Mode::Importing ? (cancel_.load() ? L"CANCELLING" : L"CANCEL") : L"CLOSE";
    PaintButton(g, kLeft, left, Enabled(kLeft), false, button_.get());
    PaintButton(g, kPlay, L"PLAY", Enabled(kPlay), true, play_.get());
  }
  BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
  SelectObject(memory, old);
  DeleteObject(bitmap);
  DeleteDC(memory);
}

// ---------------------------------------------------------------- flow --
void SetupWindow::Start() {
  std::wstring missing;
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
    heading_ = L"GoldenEye 007 is installed in this folder";
    hint_ = L"Press Play, change your extras, or import your package again to repair the game data.";
    path_text_ = L"resources\\assets";
    path_set_ = true;
    status_ = L"✓  Game data imported. Saves and settings are in resources\\userdata.";
    status_color_ = ink::Ok;
    phase_.clear();
    Invalidate();
    return;
  }
  heading_ = L"Select your Xbox 360 GoldenEye 007 package";
  hint_ = std::wstring(L"Looking beside GoldenEye 007.exe") + kEllipsis;
  path_text_ = L"No package selected";
  path_set_ = false;
  Invalidate();
  UpdateWindow(hwnd_);
  std::vector<Source> found = DetectSources(p_);
  Log(L"Detected " + std::to_wstring(found.size()) + L" package(s) or previous install(s).");
  hint_ = L"Drop the package (the 30BA9271... file) or its folder onto this window, or browse for it.";
  if (found.size() == 1) {
    StartImport(found[0]);
  } else if (found.size() > 1) {
    candidates_ = found;
    if (candidates_.size() > kMaxRows) candidates_.resize(kMaxRows);
    mode_ = Mode::Choosing;
    heading_ = L"More than one was found. Choose the one to import:";
    Invalidate();
  } else {
    status_ = L"No package was found beside GoldenEye 007.exe or in a \"Game Package\" folder.";
    status_color_ = ink::Muted;
    Invalidate();
  }
}

void SetupWindow::Browse() {
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
  DWORD flags = 0;
  dialog->GetOptions(&flags);
  dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_DONTADDTORECENT);
  dialog->SetTitle(L"Select your GoldenEye 007 Xbox 360 package");
  COMDLG_FILTERSPEC filter = {L"Xbox 360 package (any file)", L"*.*"};
  dialog->SetFileTypes(1, &filter);
  IShellItem* folder = nullptr;
  if (SUCCEEDED(SHCreateItemFromParsingName(p_.root.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
    dialog->SetFolder(folder);
    folder->Release();
  }
  if (SUCCEEDED(dialog->Show(hwnd_))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        SelectPath(path);
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
}

void SetupWindow::SelectPath(const fs::path& path) {
  if (mode_ == Mode::Importing || imported_now_) return;
  path_text_ = path.wstring();
  path_set_ = true;
  phase_.clear();
  stats_.clear();
  detail_.clear();
  show_bar_ = false;
  SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  std::wstring why;
  std::vector<Source> found = SourcesIn(path, &why);
  if (found.empty()) {
    Log(L"Rejected " + path.wstring() + L": " + why);
    if (mode_ == Mode::Choosing) mode_ = Mode::Idle;
    if (installed_) mode_ = Mode::Ready;
    status_ = why;
    status_color_ = ink::Red;
    Invalidate();
    return;
  }
  if (found.size() == 1) {
    StartImport(found[0]);
    return;
  }
  candidates_ = found;
  if (candidates_.size() > kMaxRows) candidates_.resize(kMaxRows);
  mode_ = Mode::Choosing;
  heading_ = L"That folder holds more than one. Choose the one to import:";
  Invalidate();
}

void SetupWindow::StartImport(Source source) {
  if (worker_.joinable()) worker_.join();
  mode_ = Mode::Importing;
  candidates_.clear();
  path_text_ = Describe(source);
  path_set_ = true;
  heading_ = L"Importing your game data";
  hint_ = L"Your package is only read, never changed. Nothing is installed outside this folder.";
  status_ = L"✓  GoldenEye 007  " + std::wstring(kDot) + L"  " +
            (source.kind == SourceKind::PreviousInstall ? L"previous install" : L"Xbox LIVE Arcade package") +
            L"  " + kDot + L"  " + FormatMB(source.bytes);
  status_color_ = ink::Ok;
  phase_ = L"PREPARING";
  phase_color_ = ink::Phase;
  stats_.clear();
  detail_.clear();
  progress_ = 0;
  show_bar_ = true;
  cancel_ = false;
  Invalidate();
  const fs::path saves_from = source.kind == SourceKind::PreviousInstall ? source.path : FindPreviousInstall(p_);
  HWND hwnd = hwnd_;
  worker_ = std::thread([this, hwnd, source, saves_from]() {
    ImportResult* result = new ImportResult(Import(p_, source, saves_from, cancel_, [hwnd](int percent, const std::wstring& phase, const std::wstring& detail) {
      PostMessageW(hwnd, WM_APP_PROGRESS, 0, reinterpret_cast<LPARAM>(new ProgressMsg{percent, phase, detail}));
    }));
    PostMessageW(hwnd, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(result));
  });
}

void SetupWindow::OnProgress(ProgressMsg* msg) {
  if (mode_ == Mode::Importing) {
    progress_ = std::clamp(msg->percent, 0, 100);
    phase_ = msg->phase;
    stats_ = msg->detail;
    Invalidate();
  }
  delete msg;
}

void SetupWindow::OnDone(ImportResult* result) {
  if (worker_.joinable()) worker_.join();
  stats_.clear();
  if (result->ok) {
    mode_ = Mode::Ready;
    installed_ = true;
    imported_now_ = true;
    heading_ = L"GoldenEye 007 is ready";
    hint_ = L"Your package is no longer needed. Next time GoldenEye 007.exe starts the game straight away.";
    progress_ = 100;
    phase_ = L"READY FOR DUTY";
    phase_color_ = ink::Ok;
    detail_ = result->migrated.empty() ? L"Game data imported into resources\\assets." : result->migrated;
    status_ = L"✓  Imported and verified. Press Play when you are ready.";
    status_color_ = ink::Ok;
    if (GetForegroundWindow() != hwnd_) {
      FLASHWINFO flash = {sizeof(flash), hwnd_, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
      FlashWindowEx(&flash);
    }
  } else {
    mode_ = installed_ ? Mode::Ready : Mode::Idle;
    show_bar_ = false;
    phase_ = result->cancelled ? L"IMPORT CANCELLED" : L"IMPORT DID NOT COMPLETE";
    phase_color_ = ink::Red;
    status_ = result->message;
    status_color_ = result->cancelled ? ink::Muted : ink::Red;
    detail_ = result->cancelled ? L"" : L"Your package was not changed. Details: resources\\runtime\\logs\\setup.log";
    heading_ = installed_ ? L"GoldenEye 007 is installed in this folder" : L"Select your Xbox 360 GoldenEye 007 package";
    hint_ = L"Drop the package (the 30BA9271... file) or its folder onto this window, or browse for it.";
  }
  delete result;
  cancel_ = false;
  Invalidate();
  if (close_after_import_) DestroyWindow(hwnd_);
}

void SetupWindow::Play() {
  const std::wstring note = ApplyExtras(p_, want_, was_, options_.shortcut_folder, [this](const fs::path& link) {
    const std::wstring text = L"There is already a GoldenEye 007 shortcut on your desktop that opens a different copy:\n\n" +
                              link.wstring() + L"\n\nReplace it with one for this folder?";
    return MessageBoxW(hwnd_, text.c_str(), L"GoldenEye 007", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
  });
  was_ = want_;
  if (!note.empty()) MessageBoxW(hwnd_, note.c_str(), L"GoldenEye 007", MB_OK | MB_ICONINFORMATION);
  std::wstring error;
  if (!LaunchGame(p_, options_.game_args, &error)) {
    Log(L"FAILED to start the game: " + error);
    MessageBoxW(hwnd_, error.c_str(), L"GoldenEye 007", MB_OK | MB_ICONERROR);
    return;
  }
  Log(L"Play: game started.");
  DestroyWindow(hwnd_);
}

void SetupWindow::RequestClose() {
  if (mode_ == Mode::Importing) {
    // Closing stops the import first; the window goes once it has cleaned up.
    close_after_import_ = true;
    cancel_ = true;
    Invalidate();
    return;
  }
  DestroyWindow(hwnd_);
}

void SetupWindow::Click(Control id) {
  switch (id) {
    case kBrowse: Browse(); break;
    case kLeft:
      if (mode_ == Mode::Importing) {
        cancel_ = true;
        Invalidate();
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
  Invalidate();
}

// ------------------------------------------------------------- messages --
LRESULT CALLBACK SetupWindow::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  SetupWindow* self = reinterpret_cast<SetupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    self = static_cast<SetupWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SetupWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd_, &ps);
      Paint(dc);
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DPICHANGED: ApplyScale(HIWORD(wp), reinterpret_cast<RECT*>(lp)); return 0;
    case WM_APP_START: Start(); return 0;
    case WM_APP_PROGRESS: OnProgress(reinterpret_cast<ProgressMsg*>(lp)); return 0;
    case WM_APP_DONE: OnDone(reinterpret_cast<ImportResult*>(lp)); return 0;
    case WM_MOUSEMOVE: {
      if (!tracking_) {
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd_, 0};
        tracking_ = TrackMouseEvent(&track) != FALSE;
      }
      const Control hit = HitTest(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_);
      if (hit != hover_) {
        hover_ = hit;
        Invalidate();
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      tracking_ = false;
      if (hover_ != kNone) {
        hover_ = kNone;
        Invalidate();
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, hover_ != kNone ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_LBUTTONDOWN: {
      const Control hit = HitTest(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_);
      if (hit == kNone) {
        // Borderless window: drag it from anywhere that is not a control.
        ReleaseCapture();
        SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
      }
      pressed_ = hit;
      SetCapture(hwnd_);
      Invalidate();
      return 0;
    }
    case WM_LBUTTONUP: {
      const Control was = pressed_;
      pressed_ = kNone;
      ReleaseCapture();
      const Control hit = HitTest(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_);
      if (was != kNone && was == hit) Click(hit);
      Invalidate();
      return 0;
    }
    case WM_KEYDOWN:
      if (wp == VK_ESCAPE) RequestClose();
      if (wp == VK_RETURN && Enabled(kPlay)) Play();
      return 0;
    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wp);
      wchar_t path[32768] = {};
      if (DragQueryFileW(drop, 0, path, 32768) > 0) SelectPath(path);
      DragFinish(drop);
      return 0;
    }
    case WM_CLOSE: RequestClose(); return 0;
    case WM_DESTROY:
      if (worker_.joinable()) {
        cancel_ = true;
        worker_.join();
      }
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

int SetupWindow::Run(HINSTANCE instance) {
  ULONG_PTR token = 0;
  GdiplusStartupInput input;
  if (GdiplusStartup(&token, &input, nullptr) != Ok) return 1;
  CreateFonts();
  icon_ = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.style = CS_DROPSHADOW;
  wc.lpfnWndProc = Proc;
  wc.hInstance = instance;
  wc.hIcon = icon_;
  wc.hIconSm = icon_;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);
  hwnd_ = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_ACCEPTFILES, kClassName, L"GoldenEye 007 Setup",
                          WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, 0, 0, 100, 100, nullptr, nullptr, instance, this);
  if (!hwnd_) return 1;
  // Explorer runs unelevated like us, but allow drops from it either way.
  ChangeWindowMessageFilterEx(hwnd_, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
  ChangeWindowMessageFilterEx(hwnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
  ChangeWindowMessageFilterEx(hwnd_, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, nullptr);
  ApplyScale(GetDpiForWindow(hwnd_), nullptr);
  heading_ = L"Select your Xbox 360 GoldenEye 007 package";
  ShowWindow(hwnd_, SW_SHOWNORMAL);
  SetForegroundWindow(hwnd_);
  UpdateWindow(hwnd_);
  PostMessageW(hwnd_, WM_APP_START, 0, 0);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  const int code = installed_ ? 0 : 1;
  // Fonts are GDI+ objects: release them before GDI+ shuts down.
  brand_.reset(), tag_.reset(), title_.reset(), caps_.reset(), body_.reset(), small_.reset(), foot_.reset();
  button_.reset(), play_.reset(), row_title_.reset(), row_detail_.reset(), tab_.reset(), seal_big_.reset();
  seal_small_.reset(), stamp_.reset(), file_tab_.reset();
  GdiplusShutdown(token);
  return code;
}

}  // namespace

int RunSetupWindow(HINSTANCE instance, const Paths& p, const SetupOptions& options) {
  Log(L"Setup opened" + (options.reason.empty() ? std::wstring() : L" (" + options.reason + L")"));
  SetupWindow window(p, options);
  return window.Run(instance);
}

}  // namespace ge
