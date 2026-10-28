// ge - platform facade: the few OS / windowing services the game hooks and the
// native menu need. Windows keeps its Win32 implementation (raw input, ClipCursor,
// ...); every other platform goes through SDL3 + POSIX. Implemented in ge_platform.cpp.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct SDL_Window;

namespace ge::platform {

// Windows virtual-key codes (the SDK's rex::ui::VirtualKey values) for the keys the
// hooks and the menu poll through KeyDown().
namespace vk {
constexpr int kLButton = 0x01;
constexpr int kRButton = 0x02;
constexpr int kBack = 0x08;
constexpr int kReturn = 0x0D;
constexpr int kShift = 0x10;
constexpr int kEscape = 0x1B;
constexpr int kSpace = 0x20;
constexpr int kUp = 0x26;
constexpr int kDown = 0x28;
constexpr int kNumpad0 = 0x60;
constexpr int kNumpad9 = 0x69;
constexpr int kSubtract = 0x6D;
constexpr int kDecimal = 0x6E;
constexpr int kOemMinus = 0xBD;
constexpr int kOemPeriod = 0xBE;
}  // namespace vk

// ---- time ----
uint64_t TicksMs();               // monotonic milliseconds (SDL_GetTicks)
int64_t PerfCounter();            // SDL_GetPerformanceCounter
int64_t PerfFrequency();          // SDL_GetPerformanceFrequency
void SleepMs(uint32_t ms);
// Sleep about `us` microseconds with sub-millisecond granularity (Win32 high-resolution
// waitable timer; POSIX clock_nanosleep).
void SleepPreciseUs(uint32_t us);

// ---- process ----
std::filesystem::path ExeDir();   // folder of the running executable
// Start a new instance of this executable, detached, with ExeDir() as its working
// directory. True if it was started.
bool RelaunchDetached();
[[noreturn]] void HardExit();     // end the process at once (no shutdown)
std::string OsVersionString();    // for the log header
std::string CpuNameString();      // for the log header
bool GamepadConnected();          // a controller is plugged in (or fakepad.txt beside the exe)

// ---- window ----
SDL_Window* GameWindow();         // the game's (only) SDL window, nullptr before it exists
void* NativeWindowHandle();       // HWND on Windows, nullptr elsewhere
bool GameHasFocus();              // this process owns the keyboard focus
bool ClientSize(int& w, int& h);  // game window client area in pixels
// Windowed mode: make the client area w x h, keeping the window centred where it was
// and inside the screen. Does nothing in fullscreen. May complete asynchronously.
void ResizeClient(int w, int h);

// ---- pointer / keyboard ----
bool CursorPosInClient(int& x, int& y);  // pointer relative to the client area
void WarpCursorInClient(int x, int y);
// Windows VK code (also the mouse buttons 0x01 left, 0x02 right, 0x04 middle, 0x05/0x06
// X1/X2) is currently held, whichever window has focus on Windows / while focused on SDL.
bool KeyDown(int vk);
// Windows: the game window is being moved/sized (title bar or border drag), or a
// button is held with the pointer outside the client area. SDL: always false (the
// compositor owns moves, and the pointer is not warped during them).
bool WindowDragActive();

// Raw mouse input, delivered to the game's hooks.
struct MouseSink {
  void (*delta)(int dx, int dy);  // relative motion; only called while active() is true
  void (*wheel)(int notches);     // +1 per notch up, -1 per notch down; only while wheel_wanted()
  bool (*active)();               // mouse-look is live: collect deltas and capture the pointer
  bool (*wheel_wanted)();         // wheel notches are wanted
};
// Once: start collecting deltas/wheel and capture (hide + confine) the pointer while
// sink.active() holds, release it otherwise. Windows: raw-input thread with a 15 ms
// timer. Elsewhere: an SDL event watch + 15 ms timer using relative mouse mode.
void StartMouse(const MouseSink& sink);
// Re-evaluate the capture state now (the timer does it every 15 ms); hooks call
// it when the pause menu opens so the pointer is released at once.
void UpdateMouseCapture();

}  // namespace ge::platform
