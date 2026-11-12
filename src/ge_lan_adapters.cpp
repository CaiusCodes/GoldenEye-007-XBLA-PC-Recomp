// ge - this PC's network adapters, for Online Settings' LAN Address row.

#include "ge_lan_adapters.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

namespace ge {

std::vector<LanAdapter> ListLanAdapters() {
  std::vector<LanAdapter> out;
  ULONG size = 16 * 1024;
  std::vector<unsigned char> buffer;
  ULONG result = ERROR_BUFFER_OVERFLOW;
  for (int tries = 0; tries < 3 && result == ERROR_BUFFER_OVERFLOW; ++tries) {
    buffer.resize(size);
    result = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
  }
  if (result != NO_ERROR) return out;
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    std::string name;
    if (a->FriendlyName) {
      const int n = WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, nullptr, 0, nullptr, nullptr);
      if (n > 1) {
        name.resize(static_cast<size_t>(n - 1));
        WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name.data(), n, nullptr, nullptr);
      }
    }
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
      const uint32_t addr =
          ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
      if (addr == 0 || (addr >> 24) == 127 || (addr >> 16) == 0xA9FE) continue;
      out.push_back({addr, name});
    }
  }
  return out;
}

std::string FormatIPv4(uint32_t a) {
  return std::to_string((a >> 24) & 0xFF) + "." + std::to_string((a >> 16) & 0xFF) + "." +
         std::to_string((a >> 8) & 0xFF) + "." + std::to_string(a & 0xFF);
}

}  // namespace ge
