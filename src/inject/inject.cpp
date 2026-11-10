// inject.exe: loads ewcoop.dll (next to this exe) into running EvilWithin.exe.
// Start the game from Steam first, then run this. Injecting into every instance (so a second sandboxed
// instance is hooked too). Optionally pass a pid to target just that one. Nothing in the game folder changes.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>

static std::vector<DWORD> findGames() {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (_wcsicmp(e.szExeFile, L"EvilWithin.exe") == 0) pids.push_back(e.th32ProcessID);
    CloseHandle(snap);
    return pids;
}

static bool inject(DWORD pid, const wchar_t* dll) {
    // Load a uniquely named copy so the build output never gets locked by the game and a rebuilt DLL can be
    // injected again (it takes over the hook from the old copy).
    wchar_t copy[MAX_PATH];
    wcscpy(copy, dll);
    swprintf(wcsrchr(copy, L'\\') + 1, 64, L"ewcoop_loaded_%lu_%lu.dll", pid, GetTickCount());  // pid: unique per copy
    if (!CopyFileW(dll, copy, FALSE)) { fprintf(stderr, "copy failed for pid %lu: %lu\n", pid, GetLastError()); return false; }

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { fprintf(stderr, "OpenProcess(%lu) failed: %lu\n", pid, GetLastError()); return false; }

    size_t bytes = (wcslen(copy) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WriteProcessMemory(proc, remote, copy, bytes, nullptr);
    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE t = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    bool ok = t != nullptr;
    DWORD mod = 0;
    if (t) { WaitForSingleObject(t, 10000); GetExitCodeThread(t, &mod); ok = mod != 0; CloseHandle(t); }
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    if (!ok) fprintf(stderr, "LoadLibrary failed inside pid %lu.\n", pid);
    return ok;
}

int main(int argc, char** argv) {
    wchar_t dll[MAX_PATH];
    GetModuleFileNameW(nullptr, dll, MAX_PATH);
    wcscpy(wcsrchr(dll, L'\\') + 1, L"ewcoop.dll");
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"missing %ls\n", dll);
        return 1;
    }
    auto pids = findGames();
    if (pids.empty()) { fputs("EvilWithin.exe is not running. Start it from Steam first.\n", stderr); return 1; }

    DWORD only = argc > 1 ? static_cast<DWORD>(strtoul(argv[1], nullptr, 0)) : 0;
    int done = 0;
    for (DWORD pid : pids) {
        if (only && pid != only) continue;
        if (inject(pid, dll)) { wprintf(L"Injected into pid %lu. Log: ewcoop_%lu.log next to the DLL.\n", pid, pid); ++done; }
    }
    if (!done) { fputs("No instance injected.\n", stderr); return 1; }
    return 0;
}
