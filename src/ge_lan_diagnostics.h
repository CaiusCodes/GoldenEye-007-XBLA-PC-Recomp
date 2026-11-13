// ge - LAN troubleshooting lines written to the log once at start.
#pragma once

namespace ge {

// Logs this PC's networks and the Windows Firewall rules for this exe
// ([lan-check] lines) on a background thread.
void StartLanDiagnostics();

}  // namespace ge
