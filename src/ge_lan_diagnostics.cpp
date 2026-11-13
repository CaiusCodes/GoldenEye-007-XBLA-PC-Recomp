// ge - LAN troubleshooting lines in every log, written once at start on a
// background thread: this PC's networks (name, address, gateway, Public or
// Private) and the Windows Firewall's state and rules for this exe. Together
// with the SDK's [lan] lines (address chosen, first packet from/to each other
// PC, how each match search ended) a player's log shows why two PCs don't see
// each other without asking for packet logging.

#include "ge_lan_diagnostics.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <netfw.h>
#include <netlistmgr.h>

#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>

namespace ge {
namespace {

std::string Utf8(const wchar_t* text) {
  if (!text) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
  return out;
}

std::string Ipv4(const SOCKADDR* sa) {
  if (!sa || sa->sa_family != AF_INET) return {};
  const uint32_t a = ntohl(reinterpret_cast<const sockaddr_in*>(sa)->sin_addr.s_addr);
  return std::to_string((a >> 24) & 0xFF) + "." + std::to_string((a >> 16) & 0xFF) + "." +
         std::to_string((a >> 8) & 0xFF) + "." + std::to_string(a & 0xFF);
}

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string ProfileNames(long profiles) {
  if (profiles == NET_FW_PROFILE2_ALL || profiles == 0x7FFFFFFF) return "all networks";
  std::string out;
  auto add = [&](long bit, const char* name) {
    if (profiles & bit) out += out.empty() ? name : std::string("+") + name;
  };
  add(NET_FW_PROFILE2_PRIVATE, "Private");
  add(NET_FW_PROFILE2_PUBLIC, "Public");
  add(NET_FW_PROFILE2_DOMAIN, "Domain");
  return out.empty() ? "none" : out;
}

// Adapter GUID ("{...}", lower case) -> "Public" / "Private" / "Domain".
std::vector<std::pair<std::string, std::string>> NetworkCategories() {
  std::vector<std::pair<std::string, std::string>> out;
  INetworkListManager* nlm = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL,
                              __uuidof(INetworkListManager), reinterpret_cast<void**>(&nlm))))
    return out;
  IEnumNetworkConnections* connections = nullptr;
  if (SUCCEEDED(nlm->GetNetworkConnections(&connections)) && connections) {
    INetworkConnection* connection = nullptr;
    ULONG fetched = 0;
    while (connections->Next(1, &connection, &fetched) == S_OK && connection) {
      GUID id{};
      INetwork* network = nullptr;
      NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PUBLIC;
      if (SUCCEEDED(connection->GetAdapterId(&id)) &&
          SUCCEEDED(connection->GetNetwork(&network)) && network &&
          SUCCEEDED(network->GetCategory(&category))) {
        wchar_t text[64] = {};
        StringFromGUID2(id, text, 64);
        out.emplace_back(Lower(Utf8(text)),
                         category == NLM_NETWORK_CATEGORY_PRIVATE ? "Private"
                         : category == NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED ? "Domain"
                                                                                 : "Public");
      }
      if (network) network->Release();
      connection->Release();
      connection = nullptr;
    }
    connections->Release();
  }
  nlm->Release();
  return out;
}

void LogNetworks() {
  const auto categories = NetworkCategories();
  ULONG size = 32 * 1024;
  std::vector<unsigned char> buffer;
  ULONG result = ERROR_BUFFER_OVERFLOW;
  for (int tries = 0; tries < 3 && result == ERROR_BUFFER_OVERFLOW; ++tries) {
    buffer.resize(size);
    result = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                                      GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                                  &size);
  }
  if (result != NO_ERROR) {
    REXLOG_WARN("[lan-check] could not list this PC's networks (error {})", result);
    return;
  }
  int count = 0;
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    std::string addresses;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      const std::string ip = Ipv4(u->Address.lpSockaddr);
      if (ip.empty()) continue;
      if (!addresses.empty()) addresses += ", ";
      addresses += ip + "/" + std::to_string(u->OnLinkPrefixLength);
    }
    if (addresses.empty()) continue;
    std::string gateway = "none";
    for (auto* g = a->FirstGatewayAddress; g; g = g->Next) {
      const std::string ip = Ipv4(g->Address.lpSockaddr);
      if (!ip.empty()) {
        gateway = ip;
        break;
      }
    }
    std::string category = "no network profile";
    const std::string guid = Lower(a->AdapterName ? a->AdapterName : "");
    for (const auto& [id, name] : categories) {
      if (id == guid) category = name;
    }
    REXLOG_INFO("[lan-check] network '{}' ({}): {}, gateway {}, {}", Utf8(a->FriendlyName),
                Utf8(a->Description), addresses, gateway, category);
    ++count;
  }
  if (!count) REXLOG_WARN("[lan-check] no connected network with an IPv4 address");
}

void LogFirewall() {
  INetFwPolicy2* policy = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                              __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy)))) {
    REXLOG_INFO("[lan-check] Windows Firewall settings not readable (another firewall?)");
    return;
  }
  long current = 0;
  policy->get_CurrentProfileTypes(&current);
  std::string enabled;
  for (NET_FW_PROFILE_TYPE2 profile : {NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC,
                                       NET_FW_PROFILE2_DOMAIN}) {
    VARIANT_BOOL on = VARIANT_FALSE;
    policy->get_FirewallEnabled(profile, &on);
    if (!enabled.empty()) enabled += ", ";
    enabled += ProfileNames(profile) + (on ? " on" : " off");
  }
  REXLOG_INFO("[lan-check] Windows Firewall: {}; networks in use count as {}", enabled,
              ProfileNames(current));

  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::string exe_path = Lower(Utf8(exe));

  INetFwRules* rules = nullptr;
  IUnknown* enum_unknown = nullptr;
  IEnumVARIANT* enumerator = nullptr;
  int matched = 0;
  long allowed = 0, blocked = 0;  // inbound UDP profiles
  if (SUCCEEDED(policy->get_Rules(&rules)) && rules &&
      SUCCEEDED(rules->get__NewEnum(&enum_unknown)) && enum_unknown &&
      SUCCEEDED(enum_unknown->QueryInterface(__uuidof(IEnumVARIANT),
                                             reinterpret_cast<void**>(&enumerator)))) {
    VARIANT item;
    VariantInit(&item);
    ULONG fetched = 0;
    while (enumerator->Next(1, &item, &fetched) == S_OK) {
      INetFwRule* rule = nullptr;
      if (item.vt == VT_DISPATCH && item.pdispVal &&
          SUCCEEDED(item.pdispVal->QueryInterface(__uuidof(INetFwRule),
                                                  reinterpret_cast<void**>(&rule)))) {
        BSTR app = nullptr;
        if (SUCCEEDED(rule->get_ApplicationName(&app)) && app) {
          wchar_t expanded[MAX_PATH] = {};
          ExpandEnvironmentStringsW(app, expanded, MAX_PATH);
          if (Lower(Utf8(expanded)) == exe_path) {
            BSTR name = nullptr;
            NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_IN;
            NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
            VARIANT_BOOL on = VARIANT_FALSE;
            long protocol = 0, profiles = 0;
            rule->get_Name(&name);
            rule->get_Direction(&direction);
            rule->get_Action(&action);
            rule->get_Enabled(&on);
            rule->get_Protocol(&protocol);
            rule->get_Profiles(&profiles);
            const bool inbound = direction == NET_FW_RULE_DIR_IN;
            const bool udp = protocol == NET_FW_IP_PROTOCOL_UDP || protocol == NET_FW_IP_PROTOCOL_ANY;
            REXLOG_INFO("[lan-check] firewall rule '{}': {} {} {} on {}{}", Utf8(name),
                        inbound ? "inbound" : "outbound",
                        protocol == NET_FW_IP_PROTOCOL_UDP   ? "UDP"
                        : protocol == NET_FW_IP_PROTOCOL_TCP ? "TCP"
                                                             : "any protocol",
                        action == NET_FW_ACTION_ALLOW ? "allowed" : "BLOCKED",
                        ProfileNames(profiles), on ? "" : " (rule switched off)");
            if (inbound && udp && on) {
              (action == NET_FW_ACTION_ALLOW ? allowed : blocked) |= profiles;
            }
            if (name) SysFreeString(name);
            ++matched;
          }
          SysFreeString(app);
        }
        rule->Release();
      }
      VariantClear(&item);
    }
  }
  if (enumerator) enumerator->Release();
  if (enum_unknown) enum_unknown->Release();
  if (rules) rules->Release();
  policy->Release();

  if (!matched) {
    REXLOG_INFO("[lan-check] no Windows Firewall rule for this exe yet (Windows asks the first "
                "time LAN play is used)");
  } else if (blocked & current) {
    REXLOG_WARN("[lan-check] Windows Firewall BLOCKS incoming LAN packets for this exe on {} "
                "networks: other PCs can't reach this one. Allow GoldenEye 007 in Windows "
                "Security > Firewall & network protection > Allow an app through firewall",
                ProfileNames(blocked & current));
  } else if ((allowed & current) != current) {
    REXLOG_WARN("[lan-check] Windows Firewall has no allow rule for this exe on {} networks: "
                "other PCs may not reach this one",
                ProfileNames(current & ~allowed));
  } else {
    REXLOG_INFO("[lan-check] Windows Firewall allows incoming LAN packets for this exe on the "
                "networks in use");
  }
}

}  // namespace

void StartLanDiagnostics() {
  std::thread([] {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    LogNetworks();
    LogFirewall();
    if (SUCCEEDED(com)) CoUninitialize();
  }).detach();
}

}  // namespace ge
