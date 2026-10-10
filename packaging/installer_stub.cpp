// GoldenEye 007 PC - Setup ("Setup GoldenEye 007.exe")
//
// The release folder holds only this Setup, README.txt and licenses\. Setup
// carries everything else inside itself: a ZIP appended to the end of this
// executable (installer\ scripts and the game\ runtime, see tools/Make-Release.ps1)
// followed by a 16-byte trailer: "GE7SETUP" and the ZIP's offset (uint64,
// little endian). Setup copies that ZIP to a temporary folder, unpacks it with
// Windows' own tar.exe, runs installer\Install-GoldenEye.ps1 from it (-File, so
// no script text is built on the command line) with the release folder (where
// this exe is) and the unpacked payload, forwards its own command line (used for
// automated testing), removes the temporary folder and returns the script's exit
// code. Built by packaging/Build-Stubs.ps1.
//
// A GUI-subsystem program, so starting Setup never flashes a console.

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
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

// One command-line argument, quoted the way CommandLineToArgvW reads it back.
static std::wstring Quote(const std::wstring& s) {
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (wchar_t c : s) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    out += c;
  }
  out.append(slashes * 2, L'\\');
  return out + L"\"";
}

// Runs program with command (its full command line) in dir, without a
// console window. Returns its exit code, or -1 if it could not start.
static long Run(const std::wstring& program, std::wstring command, const std::wstring& dir) {
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, dir.c_str(), &startup, &process)) {
    return -1;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  return static_cast<long>(exit_code);
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

  // Full paths, so a stray tar.exe or powershell.exe on PATH is never picked up.
  wchar_t system_dir[MAX_PATH];
  GetSystemDirectoryW(system_dir, MAX_PATH);
  const std::wstring tar = std::wstring(system_dir) + L"\\tar.exe";
  const std::wstring powershell =
      std::wstring(system_dir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";

  long code = 1;
  CreateDirectoryW(payload.c_str(), nullptr);
  const long unpacked =
      Run(tar, Quote(tar) + L" -xf " + Quote(zip) + L" -C " + Quote(payload), work);
  if (unpacked != 0) {
    Fail(unpacked < 0 ? L"Setup could not start Windows' tar.exe to unpack itself.\n\n"
                        L"Setup needs Windows 10 (version 1803) or later."
                      : L"Setup could not unpack itself. Check there is free space on the "
                        L"system drive, then run Setup again.");
  } else {
    // Setup's own arguments go straight to the script (testing passes
    // -PackagePath "C:\some folder\package" -Unattended -Extras a,b).
    std::wstring command = Quote(powershell) +
                           L" -NoProfile -STA -ExecutionPolicy RemoteSigned -File " +
                           Quote(payload + L"\\installer\\Install-GoldenEye.ps1") +
                           L" -ReleaseRoot " + Quote(release_root) + L" -PayloadDir " +
                           Quote(payload);
    if (args && *args) command += L" " + std::wstring(args);
    // CREATE_NO_WINDOW hides PowerShell's console; the script draws its own window.
    code = Run(powershell, command, release_root);
    if (code < 0) {
      Fail(L"Setup could not start Windows PowerShell.");
      code = 1;
    }
  }

  std::error_code ignored;
  std::filesystem::remove_all(work, ignored);
  return static_cast<int>(code);
}
