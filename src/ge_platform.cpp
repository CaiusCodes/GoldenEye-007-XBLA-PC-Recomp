// ge - platform facade (see ge_platform.h). Windows keeps the Win32 implementation the
// hooks and the menu used to carry inline; every other platform uses SDL3 + POSIX.
#include "ge_platform.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <SDL3/SDL.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>

namespace ge::platform {

// ---- common (SDL / std, both platforms) ----

uint64_t TicksMs() { return SDL_GetTicks(); }
int64_t PerfCounter() { return static_cast<int64_t>(SDL_GetPerformanceCounter()); }
int64_t PerfFrequency() { return static_cast<int64_t>(SDL_GetPerformanceFrequency()); }

std::filesystem::path ExeDir() { return rex::filesystem::GetExecutableFolder(); }

SDL_Window* GameWindow() {
  // The game has exactly one window; once found it stays.
  static std::atomic<SDL_Window*> cached{nullptr};
  SDL_Window* w = cached.load(std::memory_order_acquire);
  if (w) return w;
  int n = 0;
  SDL_Window** windows = SDL_GetWindows(&n);
  if (windows) {
    if (n > 0) w = windows[0];
    SDL_free(windows);
  }
  if (w) cached.store(w, std::memory_order_release);
  return w;
}

// Testing aid: a file named fakepad.txt beside the exe stands for a controller.
static bool FakePadPresent() {
  std::error_code ec;
  return std::filesystem::exists(ExeDir() / "fakepad.txt", ec);
}

}  // namespace ge::platform

#if REX_PLATFORM_WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // ShellExecuteW (WIN32_LEAN_AND_MEAN excludes it)

namespace ge::platform {
namespace {

// Cursor-capture state (touched from the mouse thread).
std::atomic<HWND> g_game_hwnd{nullptr};
HCURSOR g_arrow_cursor = nullptr;
HCURSOR g_blank_cursor = nullptr;
bool g_captured = false;
MouseSink g_sink{};

// The game window (found when it has focus); falls back to this process's visible
// captioned top-level window.
HWND GameHwnd() {
  if (HWND h = g_game_hwnd.load()) return h;
  if (SDL_Window* w = GameWindow()) {
    if (void* h = SDL_GetPointerProperty(SDL_GetWindowProperties(w),
                                         SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))
      return static_cast<HWND>(h);
  }
  HWND found = nullptr;
  EnumWindows(
      [](HWND h, LPARAM out) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(h) &&
            (GetWindowLongPtrW(h, GWL_STYLE) & WS_CAPTION)) {
          *reinterpret_cast<HWND*>(out) = h;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&found));
  return found;
}

// The visible game window (the foreground window while it's ours). Cached so we
// can still restore the cursor after focus has moved elsewhere.
HWND ForegroundGameHwnd() {
  HWND fg = GetForegroundWindow();
  if (fg) {
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) g_game_hwnd.store(fg);
  }
  return g_game_hwnd.load();
}

HCURSOR MakeBlankCursor() {
  // 32x32 fully transparent cursor (AND=1 / XOR=0 == transparent everywhere).
  BYTE and_mask[32 * 32 / 8];
  BYTE xor_mask[32 * 32 / 8];
  std::memset(and_mask, 0xFF, sizeof(and_mask));
  std::memset(xor_mask, 0x00, sizeof(xor_mask));
  return CreateCursor(GetModuleHandleW(nullptr), 0, 0, 32, 32, and_mask, xor_mask);
}

// True while Windows is moving or sizing the game window (title bar or border
// drag), or a button is held with the pointer outside the client area (the
// click that starts such a drag). Confining the pointer then would pull it into
// the window and drag the window down with it.
bool WindowDragActive(HWND hwnd) {
  GUITHREADINFO gti{};
  gti.cbSize = sizeof(gti);
  if (GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, nullptr), &gti) &&
      (gti.flags & GUI_INMOVESIZE))
    return true;
  const bool button_down = KeyDown(0x01) || KeyDown(0x02);
  if (!button_down) return false;
  POINT pt;
  RECT rc;
  if (!GetCursorPos(&pt) || !GetClientRect(hwnd, &rc)) return false;
  ScreenToClient(hwnd, &pt);
  return !PtInRect(&rc, pt);
}

// Capture (hide + confine) the cursor during play; release it in menus, when
// unfocused, or when mouse-look is off. Hiding is done by swapping the game
// window's class cursor (works cross-thread, unlike ShowCursor).
void UpdateMouseCaptureNow() {
  HWND hwnd = ForegroundGameHwnd();
  const bool want = g_sink.active && g_sink.active() && hwnd != nullptr && !WindowDragActive(hwnd);
  if (want) {
    if (!g_captured) {
      g_captured = true;
      if (g_blank_cursor) SetClassLongPtrW(hwnd, GCLP_HCURSOR, (LONG_PTR)g_blank_cursor);
      REXKRNL_INFO("GEMOUSE capture ON  hwnd={}", (void*)hwnd);
    }
    RECT rc;
    if (GetClientRect(hwnd, &rc)) {
      POINT tl{rc.left, rc.top}, br{rc.right, rc.bottom};
      ClientToScreen(hwnd, &tl);
      ClientToScreen(hwnd, &br);
      RECT screen{tl.x, tl.y, br.x, br.y};
      ClipCursor(&screen);  // re-confine each tick (window may move/resize)
    }
  } else if (g_captured) {
    g_captured = false;
    if (hwnd && g_arrow_cursor) SetClassLongPtrW(hwnd, GCLP_HCURSOR, (LONG_PTR)g_arrow_cursor);
    ClipCursor(nullptr);
  }
}

LRESULT CALLBACK RawWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_INPUT) {
    RAWINPUT ri;
    UINT sz = sizeof(ri);
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
        ri.header.dwType == RIM_TYPEMOUSE &&
        (ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
      static std::atomic<bool> logged{false};
      bool exp = false;
      if (logged.compare_exchange_strong(exp, true))
        REXKRNL_INFO("GEMOUSE first WM_INPUT dx={} dy={} active={}", ri.data.mouse.lLastX,
                     ri.data.mouse.lLastY, g_sink.active ? g_sink.active() : false);
      // Only collect while active, so deltas don't queue while alt-tabbed/paused
      // and snap the view on return.
      if (g_sink.active && g_sink.active() && g_sink.delta)
        g_sink.delta(ri.data.mouse.lLastX, ri.data.mouse.lLastY);
    }
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
        ri.header.dwType == RIM_TYPEMOUSE && (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) &&
        g_sink.wheel_wanted && g_sink.wheel_wanted() && g_sink.wheel) {
      const short delta = static_cast<short>(ri.data.mouse.usButtonData);
      if (delta != 0) g_sink.wheel(delta > 0 ? 1 : -1);
    }
    return 0;
  }
  if (msg == WM_TIMER) {
    UpdateMouseCaptureNow();
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// Dedicated thread: own a message-only window + RIDEV_INPUTSINK raw mouse so we
// get true mouse deltas without touching the SDK's window/message loop. A timer
// drives the cursor capture/release each ~15ms.
void MouseThread() {
  g_arrow_cursor = LoadCursorA(nullptr, IDC_ARROW);  // IDC_ARROW is an ANSI int-resource
  g_blank_cursor = MakeBlankCursor();
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = RawWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"GeRawMouseWnd";
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                              wc.hInstance, nullptr);
  if (!hwnd) return;
  RAWINPUTDEVICE rid{};
  rid.usUsagePage = 0x01;  // generic desktop
  rid.usUsage = 0x02;      // mouse
  rid.dwFlags = RIDEV_INPUTSINK;
  rid.hwndTarget = hwnd;
  BOOL reg = RegisterRawInputDevices(&rid, 1, sizeof(rid));
  REXKRNL_INFO("GEMOUSE thread up: hwnd={} rawinput_register={} (err={})",
               (void*)hwnd, reg ? "OK" : "FAIL", reg ? 0u : GetLastError());
  if (!reg) return;
  SetTimer(hwnd, 1, 15, nullptr);  // ~60Hz capture-state poll
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
}

}  // namespace

void SleepMs(uint32_t ms) { Sleep(ms); }

void SleepPreciseUs(uint32_t us) {
  static thread_local HANDLE s_wait_timer =
      CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
  LARGE_INTEGER due;
  due.QuadPart = -static_cast<LONGLONG>(us) * 10;  // 100 ns units
  if (s_wait_timer && SetWaitableTimer(s_wait_timer, &due, 0, nullptr, nullptr, FALSE)) {
    WaitForSingleObject(s_wait_timer, INFINITE);
  } else {
    std::this_thread::yield();
  }
}

// Launching a second instance of a running exe is fine on Windows -- the image file
// is opened share-read.
bool RelaunchDetached() {
  wchar_t exe_path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return false;  // can't resolve our own path
  }
  // Start it from the exe's own directory so a normal boot's relative paths hold.
  std::wstring full(exe_path, exe_path + n);
  size_t slash = full.find_last_of(L"\\/");
  std::wstring workdir = (slash == std::wstring::npos) ? std::wstring() : full.substr(0, slash);
  const HINSTANCE r = ShellExecuteW(nullptr, L"open", exe_path, nullptr,
                                    workdir.empty() ? nullptr : workdir.c_str(), SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR>(r) > 32;
}

void HardExit() {
  TerminateProcess(GetCurrentProcess(), 0);
  for (;;) {
  }
}

std::string OsVersionString() {
  std::string windows = "unknown";
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    if (auto rtl_get_version =
            reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) {
      RTL_OSVERSIONINFOW vi{};
      vi.dwOSVersionInfoSize = sizeof(vi);
      if (rtl_get_version(&vi) == 0) {
        windows = std::to_string(vi.dwMajorVersion) + "." + std::to_string(vi.dwMinorVersion) +
                  " build " + std::to_string(vi.dwBuildNumber);
      }
    }
  }
  return "Windows " + windows;
}

std::string CpuNameString() {
  std::string cpu = "unknown";
  wchar_t name[128] = {};
  DWORD size = sizeof(name);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                   L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS) {
    char utf8[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    cpu = utf8;
    while (!cpu.empty() && cpu.back() == ' ') cpu.pop_back();
  }
  return cpu;
}

void* NativeWindowHandle() { return GameHwnd(); }

bool WindowDragActive() {
  HWND hwnd = GameHwnd();
  return hwnd && WindowDragActive(hwnd);
}

bool GamepadConnected() {
  typedef DWORD(WINAPI * GetStateFn)(DWORD, void*);
  static GetStateFn get_state = [] {
    HMODULE m = LoadLibraryW(L"xinput1_4.dll");
    if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
    return m ? reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState")) : nullptr;
  }();
  if (!get_state) return true;  // cannot tell: keep the button icons
  bool connected = false;
  alignas(8) unsigned char state[16] = {};
  for (DWORD i = 0; i < 4 && !connected; ++i) connected = get_state(i, state) == ERROR_SUCCESS;
  return connected || FakePadPresent();
}

bool GameHasFocus() {
  HWND fg = GetForegroundWindow();
  if (!fg) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}

bool ClientSize(int& w, int& h) {
  HWND hwnd = GameHwnd();
  RECT rc;
  if (!hwnd || !GetClientRect(hwnd, &rc)) return false;
  w = rc.right;
  h = rc.bottom;
  return true;
}

void ResizeClient(int w, int h) {
  HWND hwnd = GameHwnd();
  if (!hwnd || w <= 0 || h <= 0) return;
  const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
  const DWORD exstyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
  if (!(style & WS_CAPTION)) return;  // fullscreen: the display size applies
  if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
  RECT want{0, 0, w, h};
  AdjustWindowRectEx(&want, style, FALSE, exstyle);
  int ww = want.right - want.left, wh = want.bottom - want.top;
  RECT cur;
  GetWindowRect(hwnd, &cur);
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
  const RECT wa = mi.rcWork;
  ww = std::min<int>(ww, wa.right - wa.left);
  wh = std::min<int>(wh, wa.bottom - wa.top);
  const int cx = (cur.left + cur.right) / 2, cy = (cur.top + cur.bottom) / 2;
  const int x = std::clamp<int>(cx - ww / 2, wa.left, std::max<int>(wa.left, wa.right - ww));
  const int y = std::clamp<int>(cy - wh / 2, wa.top, std::max<int>(wa.top, wa.bottom - wh));
  SetWindowPos(hwnd, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool CursorPosInClient(int& x, int& y) {
  HWND hwnd = GameHwnd();
  POINT pt;
  if (!hwnd || !GetCursorPos(&pt)) return false;
  ScreenToClient(hwnd, &pt);
  x = pt.x;
  y = pt.y;
  return true;
}

void WarpCursorInClient(int x, int y) {
  HWND hwnd = GameHwnd();
  if (!hwnd) return;
  POINT screen{x, y};
  ClientToScreen(hwnd, &screen);
  SetCursorPos(screen.x, screen.y);
}

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void StartMouse(const MouseSink& sink) {
  static std::atomic<bool> started{false};
  bool expected = false;
  if (started.compare_exchange_strong(expected, true)) {
    g_sink = sink;
    std::thread(MouseThread).detach();
  }
}

void UpdateMouseCapture() { UpdateMouseCaptureNow(); }

}  // namespace ge::platform

#else  // POSIX + SDL3

#include <time.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cmath>

#include <rex/ui/sdl_virtual_key.h>

namespace ge::platform {
namespace {

std::atomic<uint8_t> g_keys[256];
std::atomic<uint8_t> g_mods{0};  // bit per SDL modifier scancode, see OnKey
MouseSink g_sink{};
std::atomic<bool> g_sink_ready{false};
std::atomic<bool> g_captured{false};
// Sub-pixel remainders of relative motion (touched by the event-pumping thread only).
float g_acc_x = 0.f, g_acc_y = 0.f;

void ClearKeys() {
  for (auto& k : g_keys) k.store(0, std::memory_order_relaxed);
  g_mods.store(0, std::memory_order_relaxed);
}

void OnKey(SDL_Scancode sc, bool down) {
  int mod_bit = -1, vk_lr = 0;
  switch (sc) {
    case SDL_SCANCODE_LSHIFT: mod_bit = 0; vk_lr = 0xA0; break;
    case SDL_SCANCODE_RSHIFT: mod_bit = 1; vk_lr = 0xA1; break;
    case SDL_SCANCODE_LCTRL: mod_bit = 2; vk_lr = 0xA2; break;
    case SDL_SCANCODE_RCTRL: mod_bit = 3; vk_lr = 0xA3; break;
    case SDL_SCANCODE_LALT: mod_bit = 4; vk_lr = 0xA4; break;
    case SDL_SCANCODE_RALT: mod_bit = 5; vk_lr = 0xA5; break;
    default: break;
  }
  if (mod_bit >= 0) {
    // Left and right share the generic VK (0x10 / 0x11 / 0x12): down while either is.
    const uint8_t bit = static_cast<uint8_t>(1u << mod_bit);
    const uint8_t mods = down ? static_cast<uint8_t>(g_mods.fetch_or(bit) | bit)
                              : static_cast<uint8_t>(g_mods.fetch_and(static_cast<uint8_t>(~bit)) &
                                                     static_cast<uint8_t>(~bit));
    g_keys[vk_lr].store(down, std::memory_order_relaxed);
    g_keys[0x10].store((mods & 0x03) != 0, std::memory_order_relaxed);
    g_keys[0x11].store((mods & 0x0C) != 0, std::memory_order_relaxed);
    g_keys[0x12].store((mods & 0x30) != 0, std::memory_order_relaxed);
    return;
  }
  const uint32_t vk = static_cast<uint32_t>(rex::ui::TranslateSDLScancode(sc));
  if (vk > 0 && vk < 256) g_keys[vk].store(down, std::memory_order_relaxed);
}

void OnMouseButton(uint8_t button, bool down) {
  int vk = 0;
  switch (button) {
    case SDL_BUTTON_LEFT: vk = 0x01; break;
    case SDL_BUTTON_RIGHT: vk = 0x02; break;
    case SDL_BUTTON_MIDDLE: vk = 0x04; break;
    case SDL_BUTTON_X1: vk = 0x05; break;
    case SDL_BUTTON_X2: vk = 0x06; break;
    default: return;
  }
  g_keys[vk].store(down, std::memory_order_relaxed);
}

// Runs on the thread that pumps SDL events (the UI thread) for keyboard / mouse
// events; keeps the async key state and feeds the mouse sink.
bool SDLCALL EventWatch(void*, SDL_Event* ev) {
  switch (ev->type) {
    case SDL_EVENT_KEY_DOWN: OnKey(ev->key.scancode, true); break;
    case SDL_EVENT_KEY_UP: OnKey(ev->key.scancode, false); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: OnMouseButton(ev->button.button, true); break;
    case SDL_EVENT_MOUSE_BUTTON_UP: OnMouseButton(ev->button.button, false); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_KEYMAP_CHANGED: ClearKeys(); break;
    case SDL_EVENT_MOUSE_MOTION:
      if (g_sink_ready.load(std::memory_order_acquire) && ev->motion.which != SDL_TOUCH_MOUSEID) {
        // Only collect while active, so deltas don't queue while alt-tabbed/paused
        // and snap the view on return.
        if (g_sink.active && g_sink.active() && g_sink.delta) {
          g_acc_x += ev->motion.xrel;
          g_acc_y += ev->motion.yrel;
          const int dx = static_cast<int>(g_acc_x), dy = static_cast<int>(g_acc_y);
          g_acc_x -= static_cast<float>(dx);
          g_acc_y -= static_cast<float>(dy);
          if (dx || dy) g_sink.delta(dx, dy);
        } else {
          g_acc_x = g_acc_y = 0.f;
        }
      }
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      if (g_sink_ready.load(std::memory_order_acquire) && g_sink.wheel && g_sink.wheel_wanted &&
          g_sink.wheel_wanted()) {
        float y = ev->wheel.y;
        if (ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) y = -y;  // natural scrolling: undo
        if (y > 0.f) g_sink.wheel(1);
        else if (y < 0.f) g_sink.wheel(-1);
      }
      break;
    default: break;
  }
  return true;
}

void EnsureEventWatch() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!SDL_AddEventWatch(EventWatch, nullptr))
      REXKRNL_WARN("GEMOUSE SDL_AddEventWatch failed: {}", SDL_GetError());
  });
}

void SetRelativeMouseOnMainThread(void* userdata) {
  const bool want = userdata != nullptr;
  if (SDL_Window* w = GameWindow()) {
    if (!SDL_SetWindowRelativeMouseMode(w, want))
      REXKRNL_WARN("GEMOUSE SDL_SetWindowRelativeMouseMode({}) failed: {}", want, SDL_GetError());
  }
}

// ~60 Hz on an SDL timer thread: capture (hide + confine) the pointer during play,
// release it in menus, when unfocused, or when mouse-look is off. Relative mode
// hides and confines the pointer; the SDK's WindowSDL still owns cursor visibility
// while not captured.
void UpdateMouseCaptureNow() {
  SDL_Window* w = GameWindow();
  const bool want = g_sink.active && g_sink.active() && w != nullptr;
  if (want != g_captured.load(std::memory_order_relaxed)) {
    g_captured.store(want, std::memory_order_relaxed);
    REXKRNL_INFO("GEMOUSE capture {}", want ? "ON" : "OFF");
    SDL_RunOnMainThread(SetRelativeMouseOnMainThread, want ? reinterpret_cast<void*>(1) : nullptr,
                        false);
  }
}

Uint32 SDLCALL CaptureTick(void*, SDL_TimerID, Uint32 interval) {
  UpdateMouseCaptureNow();
  return interval;
}

struct ResizeRequest {
  int w, h;
};

void ResizeOnMainThread(void* userdata) {
  std::unique_ptr<ResizeRequest> req(static_cast<ResizeRequest*>(userdata));
  SDL_Window* win = GameWindow();
  if (!win) return;
  const SDL_WindowFlags flags = SDL_GetWindowFlags(win);
  if (flags & SDL_WINDOW_FULLSCREEN) return;  // fullscreen: the display size applies
  if (flags & SDL_WINDOW_MAXIMIZED) {
    SDL_RestoreWindow(win);
    SDL_SyncWindow(win);
  }
  int px = 0, py = 0, cw = 0, ch = 0;
  SDL_GetWindowPosition(win, &px, &py);
  SDL_GetWindowSize(win, &cw, &ch);
  SDL_Rect usable{0, 0, 0, 0};
  const bool have_usable = SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(win), &usable) &&
                           usable.w > 0 && usable.h > 0;
  // The requested size is in pixels (as the Windows build sizes the client
  // area); SDL sizes windows in desktop coordinates, so scale by the density.
  const float density = SDL_GetWindowPixelDensity(win);
  const float scale = density > 0.f ? density : 1.f;
  int ww = int(std::lround(req->w / scale)), wh = int(std::lround(req->h / scale));
  if (have_usable) {
    ww = std::min(ww, usable.w);
    wh = std::min(wh, usable.h);
  }
  SDL_SetWindowSize(win, ww, wh);
  int x = px + (cw - ww) / 2, y = py + (ch - wh) / 2;  // centre where it was
  if (have_usable) {
    x = std::clamp(x, usable.x, std::max(usable.x, usable.x + usable.w - ww));
    y = std::clamp(y, usable.y, std::max(usable.y, usable.y + usable.h - wh));
  }
  SDL_SetWindowPosition(win, x, y);  // may fail (Wayland): ignored
}

void WarpOnMainThread(void* userdata) {
  std::unique_ptr<std::pair<float, float>> pos(static_cast<std::pair<float, float>*>(userdata));
  if (SDL_Window* w = GameWindow()) SDL_WarpMouseInWindow(w, pos->first, pos->second);
}

std::string TrimSpaces(std::string s) {
  const auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r'; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

}  // namespace

void SleepMs(uint32_t ms) { SleepPreciseUs(ms * 1000u); }

void SleepPreciseUs(uint32_t us) {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const int64_t ns = static_cast<int64_t>(ts.tv_nsec) + static_cast<int64_t>(us) * 1000;
  ts.tv_sec += static_cast<time_t>(ns / 1000000000);
  ts.tv_nsec = static_cast<long>(ns % 1000000000);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
  }
}

bool RelaunchDetached() {
  std::error_code ec;
  const std::string exe = std::filesystem::read_symlink("/proc/self/exe", ec).string();
  if (ec || exe.empty()) return false;
  // Start it from the exe's own directory so a normal boot's relative paths hold.
  const std::string dir = ExeDir().string();
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    setsid();
    if (!dir.empty() && chdir(dir.c_str()) != 0) {
    }
    char* const argv[] = {const_cast<char*>(exe.c_str()), nullptr};
    execv(exe.c_str(), argv);
    _exit(127);
  }
  return true;
}

void HardExit() { _exit(0); }

std::string OsVersionString() {
  std::string name = "Linux";
  std::ifstream f("/etc/os-release");
  for (std::string line; std::getline(f, line);) {
    if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
    std::string v = line.substr(12);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
      v = v.substr(1, v.size() - 2);
    if (!v.empty()) name = v;
    break;
  }
  utsname un{};
  if (uname(&un) == 0) name += std::string(", kernel ") + un.release;
  return name;
}

std::string CpuNameString() {
  std::ifstream f("/proc/cpuinfo");
  for (std::string line; std::getline(f, line);) {
    if (line.rfind("model name", 0) != 0) continue;
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string v = TrimSpaces(line.substr(colon + 1));
    if (!v.empty()) return v;
  }
  return "unknown";
}

void* NativeWindowHandle() { return nullptr; }

bool GamepadConnected() {
  int n = 0;
  SDL_JoystickID* pads = SDL_GetGamepads(&n);
  if (pads) SDL_free(pads);
  return n > 0 || FakePadPresent();
}

bool GameHasFocus() {
  SDL_Window* w = GameWindow();
  return w && SDL_GetKeyboardFocus() == w;
}

bool ClientSize(int& w, int& h) {
  SDL_Window* win = GameWindow();
  return win && SDL_GetWindowSizeInPixels(win, &w, &h);
}

void ResizeClient(int w, int h) {
  if (w <= 0 || h <= 0 || !GameWindow()) return;
  auto* req = new ResizeRequest{w, h};
  if (!SDL_RunOnMainThread(ResizeOnMainThread, req, false)) delete req;
}

bool CursorPosInClient(int& x, int& y) {
  SDL_Window* win = GameWindow();
  if (!win || SDL_GetMouseFocus() != win) return false;
  float fx = 0.f, fy = 0.f;
  SDL_GetMouseState(&fx, &fy);
  const float density = SDL_GetWindowPixelDensity(win);  // window coordinates -> pixels
  const float scale = density > 0.f ? density : 1.f;
  x = static_cast<int>(std::floor(fx * scale));
  y = static_cast<int>(std::floor(fy * scale));
  return true;
}

void WarpCursorInClient(int x, int y) {
  SDL_Window* win = GameWindow();
  if (!win) return;
  const float density = SDL_GetWindowPixelDensity(win);
  const float scale = density > 0.f ? density : 1.f;
  auto* pos = new std::pair<float, float>(static_cast<float>(x) / scale,
                                          static_cast<float>(y) / scale);
  if (!SDL_RunOnMainThread(WarpOnMainThread, pos, false)) delete pos;
}

bool KeyDown(int vk) {
  if (vk <= 0 || vk >= 256) return false;
  EnsureEventWatch();
  return g_keys[vk].load(std::memory_order_relaxed) != 0;
}

bool WindowDragActive() { return false; }

void StartMouse(const MouseSink& sink) {
  static std::atomic<bool> started{false};
  bool expected = false;
  if (!started.compare_exchange_strong(expected, true)) return;
  g_sink = sink;
  g_sink_ready.store(true, std::memory_order_release);
  EnsureEventWatch();
  if (SDL_AddTimer(15, CaptureTick, nullptr) == 0)  // ~60Hz capture-state poll
    REXKRNL_WARN("GEMOUSE SDL_AddTimer failed: {}", SDL_GetError());
  else
    REXKRNL_INFO("GEMOUSE SDL event watch + capture timer up");
}

void UpdateMouseCapture() {
  if (g_sink_ready.load(std::memory_order_acquire)) UpdateMouseCaptureNow();
}

}  // namespace ge::platform

#endif
