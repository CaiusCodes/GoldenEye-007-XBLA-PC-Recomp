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
};

GpuForScale DetectGpuForScale();
// Real display mode height (not affected by Windows display scaling).
int StartupDisplayHeight(int monitor_option);
int WindowDisplayHeight(HWND__* window);

}  // namespace ge
