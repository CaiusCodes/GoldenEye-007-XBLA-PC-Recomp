// ge - this PC's network adapters, for Online Settings' LAN Address row.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ge {

struct LanAdapter {
  uint32_t address;  // IPv4, host byte order
  std::string name;  // Windows' name for the adapter ("Radmin VPN", "Wi-Fi", ...)
};

// Adapters that are up, with an IPv4 address that is not loopback or
// link-local (169.254), in Windows' order. The same set the LAN code uses.
std::vector<LanAdapter> ListLanAdapters();

std::string FormatIPv4(uint32_t host_order);

}  // namespace ge
