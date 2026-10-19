// ge - the small part of Win32 the game code uses, for macOS and Linux.
//
// On Windows this just includes <windows.h>. Elsewhere it maps the handful of
// calls the hooks use (key state, cursor position, window rect, focus, timers)
// onto SDL3, which the SDK already uses for its window on every platform. HWND
// stands for the game's SDL_Window. Code that needs more than this (raw mouse
// input, cursor capture, relaunching) keeps its own #if _WIN32 branches.
#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#else  // !_WIN32

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <thread>

#include <unistd.h>

#include <rex/ui/sdl_virtual_key.h>
#include <rex/ui/virtual_key.h>

using HWND = SDL_Window*;
using DWORD = uint32_t;
using LONG = int32_t;
using BOOL = int;
struct RECT { LONG left, top, right, bottom; };
struct POINT { LONG x, y; };
union LARGE_INTEGER { int64_t QuadPart; };

// Win32 virtual-key codes, as rex::ui::VirtualKey numbers them.
#define GE_VK(name, key) constexpr int name = static_cast<int>(rex::ui::VirtualKey::key)
GE_VK(VK_LBUTTON, kLButton);
GE_VK(VK_RBUTTON, kRButton);
GE_VK(VK_MBUTTON, kMButton);
GE_VK(VK_XBUTTON1, kXButton1);
GE_VK(VK_XBUTTON2, kXButton2);
GE_VK(VK_BACK, kBack);
GE_VK(VK_TAB, kTab);
GE_VK(VK_RETURN, kReturn);
GE_VK(VK_SHIFT, kShift);
GE_VK(VK_CONTROL, kControl);
GE_VK(VK_MENU, kMenu);
GE_VK(VK_ESCAPE, kEscape);
GE_VK(VK_SPACE, kSpace);
GE_VK(VK_LEFT, kLeft);
GE_VK(VK_UP, kUp);
GE_VK(VK_RIGHT, kRight);
GE_VK(VK_DOWN, kDown);
GE_VK(VK_NUMPAD0, kNumpad0);
GE_VK(VK_NUMPAD9, kNumpad9);
GE_VK(VK_SUBTRACT, kSubtract);
GE_VK(VK_DECIMAL, kDecimal);
GE_VK(VK_OEM_MINUS, kOemMinus);
GE_VK(VK_OEM_PERIOD, kOemPeriod);
#undef GE_VK

namespace ge::compat {

// Virtual key -> the SDL scancodes that produce it (built once from the SDK's
// own scancode table, so both directions agree). Generic Shift/Control/Alt
// also answer for their left and right keys, as on Windows.
struct KeyTable {
  SDL_Scancode codes[256][4];
  int count[256] = {};
  KeyTable() {
    for (int sc = 0; sc < SDL_SCANCODE_COUNT; ++sc) {
      const int vk = static_cast<int>(rex::ui::TranslateSDLScancode(static_cast<SDL_Scancode>(sc)));
      Add(vk, sc);
    }
    Add(VK_SHIFT, SDL_SCANCODE_LSHIFT);
    Add(VK_SHIFT, SDL_SCANCODE_RSHIFT);
    Add(VK_CONTROL, SDL_SCANCODE_LCTRL);
    Add(VK_CONTROL, SDL_SCANCODE_RCTRL);
    Add(VK_MENU, SDL_SCANCODE_LALT);
    Add(VK_MENU, SDL_SCANCODE_RALT);
  }
  void Add(int vk, int sc) {
    if (vk <= 0 || vk > 255 || count[vk] >= 4) return;
    for (int i = 0; i < count[vk]; ++i)
      if (codes[vk][i] == sc) return;
    codes[vk][count[vk]++] = static_cast<SDL_Scancode>(sc);
  }
};

inline const KeyTable& Keys() {
  static const KeyTable table;
  return table;
}

}  // namespace ge::compat

// High bit set while the key or mouse button is held (the only bit used here).
inline short GetAsyncKeyState(int vk) {
  if (vk <= 0 || vk > 255) return 0;
  SDL_MouseButtonFlags buttons = 0;
  switch (vk) {
    case VK_LBUTTON: buttons = SDL_BUTTON_LMASK; break;
    case VK_RBUTTON: buttons = SDL_BUTTON_RMASK; break;
    case VK_MBUTTON: buttons = SDL_BUTTON_MMASK; break;
    case VK_XBUTTON1: buttons = SDL_BUTTON_X1MASK; break;
    case VK_XBUTTON2: buttons = SDL_BUTTON_X2MASK; break;
    default: break;
  }
  if (buttons) return (SDL_GetGlobalMouseState(nullptr, nullptr) & buttons) ? short(0x8000) : 0;
  int numkeys = 0;
  const bool* state = SDL_GetKeyboardState(&numkeys);
  if (!state) return 0;
  const auto& keys = ge::compat::Keys();
  for (int i = 0; i < keys.count[vk]; ++i) {
    const int sc = keys.codes[vk][i];
    if (sc < numkeys && state[sc]) return short(0x8000);
  }
  return 0;
}

// The game window while it has keyboard focus, else none.
inline HWND GetForegroundWindow() { return SDL_GetKeyboardFocus(); }
inline DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }
inline DWORD GetWindowThreadProcessId(HWND hwnd, DWORD* pid) {
  if (pid) *pid = hwnd ? GetCurrentProcessId() : 0;
  return 0;
}

inline BOOL GetClientRect(HWND hwnd, RECT* rc) {
  int w = 0, h = 0;
  if (!hwnd || !SDL_GetWindowSize(hwnd, &w, &h)) return 0;
  *rc = {0, 0, w, h};
  return 1;
}
inline BOOL GetCursorPos(POINT* pt) {
  float x = 0.f, y = 0.f;
  SDL_GetGlobalMouseState(&x, &y);
  *pt = {static_cast<LONG>(x), static_cast<LONG>(y)};
  return 1;
}
inline BOOL ScreenToClient(HWND hwnd, POINT* pt) {
  int x = 0, y = 0;
  if (!hwnd || !SDL_GetWindowPosition(hwnd, &x, &y)) return 0;
  pt->x -= x;
  pt->y -= y;
  return 1;
}
inline BOOL ClientToScreen(HWND hwnd, POINT* pt) {
  int x = 0, y = 0;
  if (!hwnd || !SDL_GetWindowPosition(hwnd, &x, &y)) return 0;
  pt->x += x;
  pt->y += y;
  return 1;
}
inline BOOL SetCursorPos(int x, int y) {
  return SDL_WarpMouseGlobal(static_cast<float>(x), static_cast<float>(y)) ? 1 : 0;
}

inline uint64_t GetTickCount64() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) {
  f->QuadPart = 1000000000;  // steady_clock ticks: nanoseconds
  return 1;
}
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* c) {
  using namespace std::chrono;
  c->QuadPart = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
  return 1;
}
// The watchdog's guest stack dumps check the pages with VirtualQuery first;
// without it they are simply left out.
constexpr DWORD MEM_COMMIT = 0x1000;
constexpr DWORD PAGE_READONLY = 0x02, PAGE_READWRITE = 0x04, PAGE_WRITECOPY = 0x08,
                PAGE_EXECUTE_READ = 0x20, PAGE_EXECUTE_READWRITE = 0x40;
struct MEMORY_BASIC_INFORMATION {
  void* BaseAddress;
  size_t RegionSize;
  DWORD State;
  DWORD Protect;
};
inline size_t VirtualQuery(const void*, MEMORY_BASIC_INFORMATION*, size_t) { return 0; }

inline void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

#endif  // _WIN32
