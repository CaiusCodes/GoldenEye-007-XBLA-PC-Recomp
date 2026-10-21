// ge - ReXGlue Recompiled Project
//
// Inputs for Render Scale "Auto" (see ge_gpu_detect.cpp).

#pragma once

#include <cstdint>
#include <string>

struct HWND__;

namespace ge {

struct GpuForScale {
  bool known = false;          // a hardware adapter was found
  bool integrated = false;     // shares system memory (handhelds, laptops)
  uint64_t video_memory_mb = 0;
  std::string name;
  std::string driver;          // user-mode driver version, a.b.c.d ("" if unknown)
};

GpuForScale DetectGpuForScale();
// Real display mode height (not affected by Windows display scaling).
int StartupDisplayHeight(int monitor_option);
int WindowDisplayHeight(HWND__* window);
// "2560x1440 @ 165 Hz" for the display the game opens on (log header).
std::string StartupDisplayMode(int monitor_option);

}  // namespace ge
