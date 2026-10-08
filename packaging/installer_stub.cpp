// GoldenEye 007 PC - Setup ("Setup GoldenEye 007.exe")
//
// The release folder holds only this Setup, README.txt and licenses\. Setup
// carries everything else inside itself: a ZIP appended to the end of this
// executable (installer\ scripts and the game\ runtime, see tools/Make-Release.ps1)
// followed by a 16-byte trailer: "GE7SETUP" and the ZIP's offset (uint64,
// little endian). Setup copies that ZIP to a temporary folder, has a hidden
// Windows PowerShell unpack it and run installer\Install-GoldenEye.ps1 with the
// release folder (where this exe is) and the unpacked payload, forwards its own
// command line (used for automated testing), removes the temporary folder and
// returns the script's exit code. Built by packaging/Build-Stubs.ps1.
//
// A GUI-subsystem program, so starting Setup never flashes a console.

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static const char kMagic[8] = {'G', 'E', '7', 'S', 'E', 'T', 'U', 'P'};

static std::wstring ExePath() {
  std::vector<wchar_t> buf(32768);
  DWORD length = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  if (length == 0 || length >= buf.size()) return {};
  return std::wstring(buf.data(), length);
}

static std::wstring Parent(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

static void Fail(const std::wstring& message) {
  MessageBoxW(nullptr, message.c_str(), L"GoldenEye 007 Setup", MB_OK | MB_ICONERROR);
}

// Copies the appended payload ZIP to zip_path. False if this exe carries none.
static bool ExtractPayload(const std::wstring& exe, const std::wstring& zip_path) {
  HANDLE in = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
  if (in == INVALID_HANDLE_VALUE) return false;
  bool ok = false;
  LARGE_INTEGER size = {};
  char trailer[16] = {};
  DWORD got = 0;
  LARGE_INTEGER at = {};
  if (GetFileSizeEx(in, &size) && size.QuadPart > 16) {
    at.QuadPart = size.QuadPart - 16;
    if (SetFilePointerEx(in, at, nullptr, FILE_BEGIN) && ReadFile(in, trailer, 16, &got, nullptr) &&
        got == 16 && std::memcmp(trailer, kMagic, 8) == 0) {
      uint64_t offset = 0;
      std::memcpy(&offset, trailer + 8, 8);
      const uint64_t end = static_cast<uint64_t>(size.QuadPart) - 16;
      HANDLE out = CreateFileW(zip_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
      if (offset < end && out != INVALID_HANDLE_VALUE) {
        at.QuadPart = static_cast<LONGLONG>(offset);
        SetFilePointerEx(in, at, nullptr, FILE_BEGIN);
        std::vector<char> chunk(1 << 20);
        uint64_t left = end - offset;
        ok = true;
        while (left > 0 && ok) {
          const DWORD want = static_cast<DWORD>(left < chunk.size() ? left : chunk.size());
          DWORD read = 0, written = 0;
          ok = ReadFile(in, chunk.data(), want, &read, nullptr) && read == want &&
               WriteFile(out, chunk.data(), read, &written, nullptr) && written == read;
          left -= read;
        }
      }
      if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    }
  }
  CloseHandle(in);
  return ok;
}

// A path for a single-quoted PowerShell string.
static std::wstring PsQuote(const std::wstring& s) {
  std::wstring out = L"'";
  for (wchar_t c : s) {
    if (c == L'\'') out += L"''";
    else out += c;
  }
  return out + L"'";
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR args, int) {
  const std::wstring exe = ExePath();
  if (exe.empty()) {
    Fail(L"Setup could not find its own location.");
    return 1;
  }
  const std::wstring release_root = Parent(exe);

  wchar_t temp[MAX_PATH + 1] = {};
  GetTempPathW(MAX_PATH, temp);
  const std::wstring work =
      std::wstring(temp) + L"GoldenEye007Setup-" + std::to_wstring(GetCurrentProcessId());
  CreateDirectoryW(work.c_str(), nullptr);
  const std::wstring zip = work + L"\\payload.zip";
  const std::wstring payload = work + L"\\payload";

  if (!ExtractPayload(exe, zip)) {
    RemoveDirectoryW(work.c_str());
    Fail(L"This copy of Setup is incomplete or damaged.\n\nDownload the release again and "
         L"extract the whole folder before running Setup.");
    return 1;
  }

  // Full path, so a stray powershell.exe elsewhere on PATH is never picked up.
  wchar_t system_dir[MAX_PATH];
  GetSystemDirectoryW(system_dir, MAX_PATH);
  const std::wstring powershell =
      std::wstring(system_dir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";

  // Unpack, run the installer with Setup's own arguments, always clean up.
  std::wstring script =
      L"$ErrorActionPreference='Stop'; $code=1; try { "
      L"Add-Type -AssemblyName System.IO.Compression.FileSystem; "
      L"[System.IO.Compression.ZipFile]::ExtractToDirectory(" + PsQuote(zip) + L", " +
      PsQuote(payload) + L"); "
      L"& " + PsQuote(payload + L"\\installer\\Install-GoldenEye.ps1") +
      L" -ReleaseRoot " + PsQuote(release_root) + L" -PayloadDir " + PsQuote(payload);
  if (args && *args) {
    // Inside the -Command "..." argument a quote must be written \" (testing
    // passes paths in quotes: -PackagePath "C:\some folder\package").
    script += L" ";
    for (const wchar_t* c = args; *c; ++c) {
      if (*c == L'"') script += L"\\\"";
      else script += *c;
    }
  }
  script += L"; $code=$LASTEXITCODE } catch { [Console]::Error.WriteLine($_.Exception.Message) } "
            L"finally { Remove-Item -LiteralPath " + PsQuote(work) +
            L" -Recurse -Force -ErrorAction SilentlyContinue }; exit $code";

  std::wstring command = L"\"" + powershell +
                         L"\" -NoProfile -STA -ExecutionPolicy Bypass -Command \"" + script + L"\"";

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};

  // CREATE_NO_WINDOW hides PowerShell's console; the script draws its own window.
  if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, release_root.c_str(), &startup, &process)) {
    Fail(L"Setup could not start Windows PowerShell.");
    return 1;
  }

  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  return static_cast<int>(exit_code);
}
