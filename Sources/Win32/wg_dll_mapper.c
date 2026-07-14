#include "wg_dll_mapper.h"
#include "wg_log.h"
#include "wg_x86_state.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TAG "DLL"

// Default stub — returns 0 in RAX and logs the call
static void stub_default(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 0;
}

static void stub_return_1(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 1;
}

static void stub_return_neg1(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = (uint64_t)-1;
}

static void stub_ExitProcess(void *cpu, void *mem) {
    WGx86State *c = cpu;
    WG_LOGI(TAG, "ExitProcess(%llu)", (unsigned long long)c->gpr[WG_REG_RCX]);
    c->halted = true;
}

static void stub_GetModuleHandleA(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 0x00400000;
}

static void stub_GetModuleHandleW(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 0x00400000;
}

static void stub_GetCurrentProcess(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = (uint64_t)-1;
}

static void stub_GetVersion(void *cpu, void *mem) {
    // Windows 10 = 0x0A000000 (major 10, minor 0)
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 0x00000A00;
}

static void stub_GetTickCount(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 10000;
}

static void stub_GetDeviceCaps(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 96; // default DPI
}

static void stub_GetSysColor(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 0x00FFFFFF;
}

static void stub_RegisterClassW(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 0xC001;
}

static void stub_CreateWindowExW(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 0x00010001; // fake HWND
}

static void stub_MessageBoxIndirectW(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 1; // IDOK
}

static void stub_ShellExecuteW(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 33; // success > 32
}

static void stub_RegOpenKeyExW(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 2; // ERROR_FILE_NOT_FOUND
}

static void stub_RegQueryValueExW(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 2;
}

static void stub_RegEnumValueW(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 259; // ERROR_NO_MORE_ITEMS
}

static void stub_RegEnumKeyW(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 259;
}

static void stub_CoCreateInstance(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 0x80004002; // E_NOINTERFACE
}

static void stub_SHGetSpecialFolderLocation(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 0x80004005; // E_FAIL
}

static void stub_CreateFileW(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = (uint64_t)-1; // INVALID_HANDLE_VALUE
}

static void stub_FindFirstFileW(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = (uint64_t)-1; // INVALID_HANDLE_VALUE
}

static void stub_GetFileAttributesW(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 0xFFFFFFFF; // INVALID_FILE_ATTRIBUTES
}

static void stub_GlobalAlloc(void *cpu, void *mem) {
    // NSIS uses GlobalAlloc for temporary buffers
    void *p = calloc(1, 65536);
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = (uint64_t)(uintptr_t)p;
}

static void stub_GlobalLock(void *cpu, void *mem) {
    // GlobalLock on GMEM_FIXED returns the pointer itself
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = ((WGx86State*)cpu)->gpr[WG_REG_RCX];
}

static void stub_GlobalFree(void *cpu, void *mem) {
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 0;
}

static void stub_MulDiv(void *cpu, void *mem) {
    WGx86State *c = cpu;
    int32_t a = (int32_t)c->gpr[WG_REG_RCX];
    int32_t b = (int32_t)c->gpr[WG_REG_RDX];
    int32_t d = (int32_t)c->gpr[WG_REG_R8];
    if (d == 0) { c->gpr[WG_REG_RAX] = (uint64_t)-1; return; }
    c->gpr[WG_REG_RAX] = (uint64_t)(int64_t)((int64_t)a * b / d);
}

static void stub_GetDiskFreeSpaceW(void *cpu, void *mem) {
    // Return true with large fake disk space
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 1;
}

static void stub_PeekMessageW(void *cpu, void *mem) {
    // No messages available
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = 0;
}

static void stub_GetCommandLineW(void *cpu, void *mem) {
    // Return a pointer to a static empty command line
    static uint16_t empty_cmdline[] = { '"', 'a', '.', 'e', 'x', 'e', '"', 0 };
    ((WGx86State*)cpu)->gpr[WG_REG_RAX] = (uint64_t)(uintptr_t)empty_cmdline;
}

// --- Mapper implementation ---

WGDllMapper *wg_dll_mapper_create(void) {
    WGDllMapper *m = calloc(1, sizeof(WGDllMapper));
    if (!m) return NULL;
    m->capacity = 4096;
    m->entries = calloc(m->capacity, sizeof(WGDllEntry));
    if (!m->entries) { free(m); return NULL; }
    m->next_thunk = WG_THUNK_BASE;
    return m;
}

void wg_dll_mapper_destroy(WGDllMapper *mapper) {
    if (!mapper) return;
    free(mapper->entries);
    free(mapper);
}

uint64_t wg_dll_mapper_register(WGDllMapper *mapper, const char *dll,
                                 const char *func, WGWin32StubFunc handler,
                                 int num_args) {
    if (mapper->count >= mapper->capacity) return 0;

    WGDllEntry *e = &mapper->entries[mapper->count++];
    strncpy(e->dll_name, dll, sizeof(e->dll_name) - 1);
    strncpy(e->func_name, func, sizeof(e->func_name) - 1);
    e->thunk_addr = mapper->next_thunk;
    e->handler = handler ? handler : stub_default;
    e->num_args = num_args;
    // The legacy stub fns write the (unused) builtin-interpreter CPU, so under
    // blink a bare registration "returns" via default_ret. Derive it from the
    // registered stub so R1S/RnegS/etc. actually take effect without needing an
    // explicit engine handler for every function.
    if (handler == stub_return_1)            e->default_ret = 1;
    else if (handler == stub_return_neg1)    e->default_ret = -1;
    else if (handler == stub_GetCurrentProcess) e->default_ret = -1; // pseudo-handle
    else                                     e->default_ret = 0;
    mapper->next_thunk += 8;
    return e->thunk_addr;
}

uint64_t wg_dll_mapper_resolve(WGDllMapper *mapper, const char *dll, const char *func) {
    // Try exact match first
    for (int i = 0; i < mapper->count; i++) {
        if (strcasecmp(mapper->entries[i].dll_name, dll) == 0 &&
            strcmp(mapper->entries[i].func_name, func) == 0) {
            return mapper->entries[i].thunk_addr;
        }
    }
    // Try without .dll suffix
    char dll_stripped[64], entry_stripped[64];
    strncpy(dll_stripped, dll, sizeof(dll_stripped) - 1);
    char *dot = strrchr(dll_stripped, '.');
    if (dot) *dot = '\0';

    for (int i = 0; i < mapper->count; i++) {
        strncpy(entry_stripped, mapper->entries[i].dll_name, sizeof(entry_stripped) - 1);
        char *edot = strrchr(entry_stripped, '.');
        if (edot) *edot = '\0';

        if (strcasecmp(entry_stripped, dll_stripped) == 0 &&
            strcmp(mapper->entries[i].func_name, func) == 0) {
            return mapper->entries[i].thunk_addr;
        }
    }

    WG_LOGI(TAG, "Auto-stub: %s!%s", dll, func);
    return wg_dll_mapper_register(mapper, dll, func, stub_default, 0);
}

uint64_t wg_dll_mapper_find_any(WGDllMapper *mapper, const char *func) {
    for (int i = 0; i < mapper->count; i++) {
        if (strcmp(mapper->entries[i].func_name, func) == 0)
            return mapper->entries[i].thunk_addr;
    }
    return 0;
}

WGWin32StubFunc wg_dll_mapper_get_handler(WGDllMapper *mapper, uint64_t thunk_addr) {
    for (int i = 0; i < mapper->count; i++) {
        if (mapper->entries[i].thunk_addr == thunk_addr)
            return mapper->entries[i].handler;
    }
    return NULL;
}

// R(dll, name, fn, nargs) — register with handler and arg count
// RS(dll, name, nargs) — register with stub_default and arg count
// R1S(dll, name, nargs) — register with stub_return_1 and arg count
#define R(dll, name, fn, n)  wg_dll_mapper_register(m, dll, #name, fn, n)
#define RS(dll, name, n)     wg_dll_mapper_register(m, dll, #name, stub_default, n)
#define R1S(dll, name, n)    wg_dll_mapper_register(m, dll, #name, stub_return_1, n)

void wg_dll_mapper_register_defaults(WGDllMapper *m) {
    // === KERNEL32.dll === (function, handler, num_stdcall_args)
    R1S("KERNEL32.dll", SetCurrentDirectoryW, 1);
    R1S("KERNEL32.dll", SetCurrentDirectoryA, 1);
    R("KERNEL32.dll", GetFileAttributesW, stub_GetFileAttributesW, 1);
    RS("KERNEL32.dll", GetFullPathNameW, 4);
    RS("KERNEL32.dll", GetFullPathNameA, 4);
    RS("KERNEL32.dll", Sleep, 1);
    R("KERNEL32.dll", GetTickCount, stub_GetTickCount, 0);
    R("KERNEL32.dll", CreateFileW, stub_CreateFileW, 7);
    RS("KERNEL32.dll", GetFileSize, 2);
    R1S("KERNEL32.dll", MoveFileW, 2);
    R1S("KERNEL32.dll", SetFileAttributesW, 2);
    RS("KERNEL32.dll", GetModuleFileNameW, 3);
    R1S("KERNEL32.dll", CopyFileW, 3);
    R("KERNEL32.dll", ExitProcess, stub_ExitProcess, 1);
    R1S("KERNEL32.dll", SetEnvironmentVariableW, 2);
    RS("KERNEL32.dll", GetWindowsDirectoryW, 2);
    RS("KERNEL32.dll", GetTempPathW, 2);
    R("KERNEL32.dll", GetCommandLineW, stub_GetCommandLineW, 0);
    R("KERNEL32.dll", GetVersion, stub_GetVersion, 0);
    RS("KERNEL32.dll", SetErrorMode, 1);
    RS("KERNEL32.dll", WaitForSingleObject, 2);
    R("KERNEL32.dll", GetCurrentProcess, stub_GetCurrentProcess, 0);
    RS("KERNEL32.dll", CompareFileTime, 2);
    R1S("KERNEL32.dll", GlobalUnlock, 1);
    R("KERNEL32.dll", GlobalLock, stub_GlobalLock, 1);
    R1S("KERNEL32.dll", CreateThread, 6);
    RS("KERNEL32.dll", GetExitCodeThread, 2);
    R1S("KERNEL32.dll", ResumeThread, 1);
    RS("KERNEL32.dll", GetLastError, 0);
    R1S("KERNEL32.dll", CreateDirectoryW, 2);
    RS("KERNEL32.dll", CreateProcessW, 10);
    RS("KERNEL32.dll", CreateProcessA, 10);
    // nsExec plug-in (ExecToLog) imports — register with correct stdcall arg
    // counts so each call cleans the stack (auto-stub num_args=0 desyncs it and
    // hangs nsExec, which froze the installer UI after the Steam-launch step).
    RS("KERNEL32.dll", GlobalReAlloc, 3);
    RS("KERNEL32.dll", PeekNamedPipe, 6);
    RS("KERNEL32.dll", GetStartupInfoW, 1);
    RS("KERNEL32.dll", CreatePipe, 4);
    RS("KERNEL32.dll", UnmapViewOfFile, 1);
    RS("KERNEL32.dll", MapViewOfFile, 5);
    RS("KERNEL32.dll", MapViewOfFileEx, 6);
    RS("KERNEL32.dll", CreateFileMappingW, 6);
    RS("KERNEL32.dll", CreateFileMappingA, 6);
    RS("KERNEL32.dll", IsWow64Process, 2);
    RS("ADVAPI32.dll", InitializeSecurityDescriptor, 2);
    RS("ADVAPI32.dll", SetSecurityDescriptorDacl, 4);
    R1S("KERNEL32.dll", RemoveDirectoryW, 1);
    RS("KERNEL32.dll", lstrcmpiA, 2);
    RS("KERNEL32.dll", GetTempFileNameW, 4);
    R1S("KERNEL32.dll", WriteFile, 5);
    RS("KERNEL32.dll", lstrcpyA, 2);
    RS("KERNEL32.dll", lstrcpyW, 2);
    R1S("KERNEL32.dll", MoveFileExW, 3);
    RS("KERNEL32.dll", lstrcatW, 2);
    RS("KERNEL32.dll", GetSystemDirectoryW, 2);
    RS("KERNEL32.dll", GetProcAddress, 2);
    R("KERNEL32.dll", GetModuleHandleA, stub_GetModuleHandleA, 1);
    R("KERNEL32.dll", GlobalFree, stub_GlobalFree, 1);
    R("KERNEL32.dll", GlobalAlloc, stub_GlobalAlloc, 2);
    RS("KERNEL32.dll", GetShortPathNameW, 3);
    RS("KERNEL32.dll", SearchPathW, 6);
    RS("KERNEL32.dll", lstrcmpiW, 2);
    R1S("KERNEL32.dll", SetFileTime, 4);
    R1S("KERNEL32.dll", CloseHandle, 1);
    RS("KERNEL32.dll", GetOverlappedResult, 4);
    // Toolhelp process enumeration (used by the nsProcess plug-in). Registered
    // with correct stdcall arg counts so the stack stays balanced when the real
    // plug-in code runs; the default return 0 makes Process32First report no
    // processes -> FindProcess("Steam.exe") resolves to "not running".
    RS("KERNEL32.dll", CreateToolhelp32Snapshot, 2);
    RS("KERNEL32.dll", Process32First, 2);
    RS("KERNEL32.dll", Process32Next, 2);
    RS("KERNEL32.dll", Process32FirstW, 2);
    RS("KERNEL32.dll", Process32NextW, 2);
    RS("KERNEL32.dll", OpenProcess, 3);
    R1S("KERNEL32.dll", TerminateProcess, 2);
    RS("KERNEL32.dll", ExpandEnvironmentStringsW, 3);
    RS("KERNEL32.dll", lstrcmpW, 2);
    R("KERNEL32.dll", GetDiskFreeSpaceW, stub_GetDiskFreeSpaceW, 5);
    RS("KERNEL32.dll", GetDiskFreeSpaceExW, 4);
    RS("KERNEL32.dll", GetDiskFreeSpaceExA, 4);
    RS("KERNEL32.dll", lstrlenW, 1);
    RS("KERNEL32.dll", lstrcpynW, 3);
    R1S("KERNEL32.dll", GetExitCodeProcess, 2);
    R("KERNEL32.dll", FindFirstFileW, stub_FindFirstFileW, 2);
    RS("KERNEL32.dll", FindNextFileW, 2);
    R1S("KERNEL32.dll", DeleteFileW, 1);
    RS("KERNEL32.dll", SetFilePointer, 4);
    R1S("KERNEL32.dll", ReadFile, 5);
    R1S("KERNEL32.dll", FindClose, 1);
    R("KERNEL32.dll", MulDiv, stub_MulDiv, 3);
    RS("KERNEL32.dll", MultiByteToWideChar, 6);
    RS("KERNEL32.dll", lstrlenA, 1);
    RS("KERNEL32.dll", WideCharToMultiByte, 8);
    RS("KERNEL32.dll", GetPrivateProfileStringW, 6);
    R1S("KERNEL32.dll", WritePrivateProfileStringW, 4);
    R1S("KERNEL32.dll", FreeLibrary, 1);
    RS("KERNEL32.dll", LoadLibraryExW, 3);
    R("KERNEL32.dll", GetModuleHandleW, stub_GetModuleHandleW, 1);
    RS("KERNEL32.dll", GetCurrentProcessId, 0);
    RS("KERNEL32.dll", GetCurrentThreadId, 0);
    RS("KERNEL32.dll", SetLastError, 1);
    RS("KERNEL32.dll", IsDebuggerPresent, 0);
    RS("KERNEL32.dll", OutputDebugStringA, 1);
    RS("KERNEL32.dll", VirtualAlloc, 4);     // real handler in wg_engine.c (maps guest heap)
    R1S("KERNEL32.dll", VirtualFree, 3);
    R1S("KERNEL32.dll", VirtualProtect, 4);  // TRUE=success; 4 args so stdcall cleanup is correct
    R1S("KERNEL32.dll", VirtualProtectEx, 5);
    RS("KERNEL32.dll", HeapCreate, 3);
    RS("KERNEL32.dll", HeapAlloc, 3);
    R1S("KERNEL32.dll", HeapFree, 3);
    RS("KERNEL32.dll", GetProcessHeap, 0);
    R1S("KERNEL32.dll", QueryPerformanceCounter, 1);
    R1S("KERNEL32.dll", QueryPerformanceFrequency, 1);
    RS("KERNEL32.dll", GetSystemInfo, 1);
    RS("KERNEL32.dll", GetNativeSystemInfo, 1);
    RS("KERNEL32.dll", GetLogicalProcessorInformation, 2);  // UE4 core-count probe
    // stdcall/3 args (RelationshipType, Buffer, ReturnedLength). MUST be registered
    // with the right arg count: as an auto-stub (num_args=0) it leaks 12 bytes of
    // caller stack -> corrupts saved regs -> SIGSEGV in Steam's CPU-topology init
    // (0x542370). Returns FALSE; Steam falls back to GetSystemInfo.
    RS("KERNEL32.dll", GetLogicalProcessorInformationEx, 3);
    R1S("KERNEL32.dll", GetVersionExA, 1);
    R1S("KERNEL32.dll", GetVersionExW, 1);

    // === MSVC CRT startup imports (steam.exe & other real EXEs) ===
    // Registered with CORRECT stdcall arg counts so each call cleans the stack;
    // auto-stub (num_args=0) desyncs it and crashes the CRT. R1S = return TRUE
    // for BOOL-success calls; RS = default 0. Engine handlers exist for
    // Tls*/Encode/DecodePointer/Heap*/GetCurrentThread and override returns.
    // -- critical sections / SRW / one-time init / SLIST (mostly void/BOOL) --
    RS ("KERNEL32.dll", InitializeCriticalSection, 1);
    R1S("KERNEL32.dll", InitializeCriticalSectionEx, 3);
    R1S("KERNEL32.dll", InitializeCriticalSectionAndSpinCount, 2);
    RS ("KERNEL32.dll", EnterCriticalSection, 1);
    RS ("KERNEL32.dll", LeaveCriticalSection, 1);
    RS ("KERNEL32.dll", DeleteCriticalSection, 1);
    R1S("KERNEL32.dll", TryEnterCriticalSection, 1);
    RS ("KERNEL32.dll", InitializeSRWLock, 1);
    RS ("KERNEL32.dll", AcquireSRWLockExclusive, 1);
    RS ("KERNEL32.dll", ReleaseSRWLockExclusive, 1);
    R1S("KERNEL32.dll", TryAcquireSRWLockExclusive, 1);
    R1S("KERNEL32.dll", InitOnceBeginInitialize, 4);
    R1S("KERNEL32.dll", InitOnceComplete, 3);
    // condition variables + address-wait (UCRT resolves these dynamically from
    // api-ms-win-core-synch; auto-stub num_args=0 would desync like FlsAlloc did)
    RS ("KERNEL32.dll", InitializeConditionVariable, 1);
    RS ("KERNEL32.dll", WakeConditionVariable, 1);
    RS ("KERNEL32.dll", WakeAllConditionVariable, 1);
    R1S("KERNEL32.dll", SleepConditionVariableCS, 3);
    R1S("KERNEL32.dll", SleepConditionVariableSRW, 4);
    RS ("KERNEL32.dll", AcquireSRWLockShared, 1);
    RS ("KERNEL32.dll", ReleaseSRWLockShared, 1);
    R1S("KERNEL32.dll", TryAcquireSRWLockShared, 1);
    R1S("KERNEL32.dll", WaitOnAddress, 4);
    RS ("KERNEL32.dll", WakeByAddressSingle, 1);
    RS ("KERNEL32.dll", WakeByAddressAll, 1);
    RS ("KERNEL32.dll", GetTickCount64, 0);
    // localization (UCRT resolves these dynamically from api-ms-win-core-localization)
    RS ("KERNEL32.dll", LCMapStringEx, 9);
    RS ("KERNEL32.dll", CompareStringEx, 9);
    RS ("KERNEL32.dll", GetLocaleInfoEx, 4);
    RS ("KERNEL32.dll", GetLocaleInfoW, 4);
    RS ("KERNEL32.dll", GetUserDefaultLocaleName, 2);
    RS ("KERNEL32.dll", GetSystemDefaultLocaleName, 2);
    R1S("KERNEL32.dll", GetStringTypeExW, 5);
    R1S("KERNEL32.dll", IsValidLocale, 2);
    RS ("KERNEL32.dll", EnumSystemLocalesW, 2);
    RS ("KERNEL32.dll", EnumSystemLocalesEx, 4);
    RS ("KERNEL32.dll", GetNLSVersionEx, 3);
    RS ("KERNEL32.dll", ResolveLocaleName, 3);
    RS ("KERNEL32.dll", GetUserDefaultLCID, 0);
    RS ("KERNEL32.dll", GetSystemDefaultLCID, 0);
    RS ("KERNEL32.dll", InitializeSListHead, 1);
    RS ("KERNEL32.dll", InterlockedPushEntrySList, 2);
    RS ("KERNEL32.dll", InterlockedPopEntrySList, 1);
    RS ("KERNEL32.dll", InterlockedFlushSList, 1);
    // -- TLS (engine handlers provide values) --
    RS ("KERNEL32.dll", TlsAlloc, 0);
    RS ("KERNEL32.dll", TlsGetValue, 1);
    R1S("KERNEL32.dll", TlsSetValue, 2);
    R1S("KERNEL32.dll", TlsFree, 1);
    RS ("KERNEL32.dll", FlsAlloc, 1);
    RS ("KERNEL32.dll", FlsGetValue, 1);
    R1S("KERNEL32.dll", FlsSetValue, 2);
    R1S("KERNEL32.dll", FlsFree, 1);
    // -- heap (engine handlers for HeapReAlloc) --
    RS ("KERNEL32.dll", HeapReAlloc, 4);
    RS ("KERNEL32.dll", HeapSize, 3);
    R1S("KERNEL32.dll", HeapValidate, 3);
    R1S("KERNEL32.dll", HeapSetInformation, 4);
    R1S("KERNEL32.dll", HeapLock, 1);
    R1S("KERNEL32.dll", HeapUnlock, 1);
    RS ("KERNEL32.dll", HeapWalk, 2);
    R1S("KERNEL32.dll", HeapQueryInformation, 5);
    RS ("KERNEL32.dll", GetProcessHeaps, 2);
    // -- console / std handles --
    RS ("KERNEL32.dll", GetStdHandle, 1);
    R1S("KERNEL32.dll", SetStdHandle, 2);
    RS ("KERNEL32.dll", GetFileType, 1);
    RS ("KERNEL32.dll", GetConsoleCP, 0);
    R1S("KERNEL32.dll", GetConsoleMode, 2);
    R1S("KERNEL32.dll", SetConsoleMode, 2);
    R1S("KERNEL32.dll", WriteConsoleW, 5);
    R1S("KERNEL32.dll", ReadConsoleW, 5);
    R1S("KERNEL32.dll", ReadConsoleA, 5);
    R1S("KERNEL32.dll", SetConsoleCtrlHandler, 2);
    // -- locale / codepage --
    RS ("KERNEL32.dll", GetACP, 0);
    RS ("KERNEL32.dll", GetOEMCP, 0);
    R1S("KERNEL32.dll", GetCPInfo, 2);
    R1S("KERNEL32.dll", IsValidCodePage, 1);
    RS ("KERNEL32.dll", LCMapStringW, 6);
    RS ("KERNEL32.dll", LCMapStringA, 6);
    RS ("KERNEL32.dll", CompareStringW, 6);
    R1S("KERNEL32.dll", GetStringTypeW, 4);
    RS ("KERNEL32.dll", GetDateFormatW, 6);
    RS ("KERNEL32.dll", GetTimeFormatW, 6);
    // -- exception / pointer encoding (engine handlers for Encode/DecodePointer) --
    RS ("KERNEL32.dll", RtlUnwind, 4);
    RS ("KERNEL32.dll", UnhandledExceptionFilter, 1);
    RS ("KERNEL32.dll", SetUnhandledExceptionFilter, 1);
    RS ("KERNEL32.dll", RaiseException, 4);
    RS ("KERNEL32.dll", IsProcessorFeaturePresent, 1);  // 0 = use safe CPU paths
    RS ("KERNEL32.dll", EncodePointer, 1);
    RS ("KERNEL32.dll", DecodePointer, 1);
    R1S("KERNEL32.dll", DuplicateHandle, 7);
    RS ("KERNEL32.dll", GetCurrentThread, 0);
    // -- threads / events / sync objects --
    RS ("KERNEL32.dll", OpenThread, 3);
    R1S("KERNEL32.dll", SetThreadPriority, 2);
    R1S("KERNEL32.dll", TerminateThread, 2);
    RS ("KERNEL32.dll", SuspendThread, 1);
    RS ("KERNEL32.dll", SwitchToThread, 0);
    RS ("KERNEL32.dll", SetThreadAffinityMask, 2);
    R1S("KERNEL32.dll", SetProcessAffinityMask, 2);
    R1S("KERNEL32.dll", GetProcessAffinityMask, 3);
    RS ("KERNEL32.dll", CreateEventW, 4);
    RS ("KERNEL32.dll", CreateEventA, 4);
    RS ("KERNEL32.dll", CreateEventExW, 4);   // UE4 FEventWin uses this, not CreateEventW
    RS ("KERNEL32.dll", CreateEventExA, 4);
    RS ("KERNEL32.dll", OpenEventA, 3);
    R1S("KERNEL32.dll", SetEvent, 1);
    R1S("KERNEL32.dll", ResetEvent, 1);
    RS ("KERNEL32.dll", WaitForSingleObjectEx, 3);
    RS ("KERNEL32.dll", WaitForMultipleObjects, 4);
    RS ("KERNEL32.dll", WaitForMultipleObjectsEx, 5);
    RS ("KERNEL32.dll", CreateMutexW, 3);
    RS ("KERNEL32.dll", CreateMutexA, 3);
    RS ("KERNEL32.dll", OpenMutexW, 3);
    RS ("KERNEL32.dll", OpenMutexA, 3);
    R1S("KERNEL32.dll", ReleaseMutex, 1);
    RS ("KERNEL32.dll", CreateSemaphoreW, 4);
    RS ("KERNEL32.dll", CreateSemaphoreA, 4);
    R1S("KERNEL32.dll", ReleaseSemaphore, 3);
    RS ("KERNEL32.dll", SleepEx, 2);
    RS ("KERNEL32.dll", Sleep, 1);
    RS ("KERNEL32.dll", ExitThread, 1);
    RS ("KERNEL32.dll", CreateIoCompletionPort, 4);
    RS ("KERNEL32.dll", GetQueuedCompletionStatus, 5);
    RS ("KERNEL32.dll", GetQueuedCompletionStatusEx, 6);
    RS ("KERNEL32.dll", PostQueuedCompletionStatus, 4);
    // -- thread pool --
    RS ("KERNEL32.dll", QueueUserWorkItem, 3);
    RS ("KERNEL32.dll", CreateThreadpoolWork, 3);
    RS ("KERNEL32.dll", SubmitThreadpoolWork, 1);
    RS ("KERNEL32.dll", CloseThreadpoolWork, 1);
    RS ("KERNEL32.dll", WaitForThreadpoolWorkCallbacks, 2);
    RS ("KERNEL32.dll", CreateThreadpoolTimer, 3);
    RS ("KERNEL32.dll", SetThreadpoolTimer, 4);
    RS ("KERNEL32.dll", CloseThreadpoolTimer, 1);
    // -- fibers --
    RS ("KERNEL32.dll", ConvertThreadToFiber, 1);
    R1S("KERNEL32.dll", ConvertFiberToThread, 0);
    RS ("KERNEL32.dll", CreateFiber, 3);
    RS ("KERNEL32.dll", DeleteFiber, 1);
    RS ("KERNEL32.dll", SwitchToFiber, 1);
    // -- module / resource --
    RS ("KERNEL32.dll", LoadLibraryA, 1);
    RS ("KERNEL32.dll", LoadLibraryExA, 3);
    R1S("KERNEL32.dll", GetModuleHandleExW, 3);
    R1S("KERNEL32.dll", GetModuleHandleExA, 3);
    RS ("KERNEL32.dll", GetModuleFileNameA, 3);
    RS ("KERNEL32.dll", FindResourceA, 3);
    RS ("KERNEL32.dll", FindResourceW, 3);
    RS ("KERNEL32.dll", LoadResource, 2);
    RS ("KERNEL32.dll", LockResource, 1);
    RS ("KERNEL32.dll", SizeofResource, 2);
    RS ("KERNEL32.dll", LocalAlloc, 2);
    RS ("KERNEL32.dll", LocalFree, 1);
    // -- env / time / file / misc --
    RS ("KERNEL32.dll", GetEnvironmentVariableW, 3);
    RS ("KERNEL32.dll", GetEnvironmentStringsW, 0);
    R1S("KERNEL32.dll", FreeEnvironmentStringsW, 1);
    RS ("KERNEL32.dll", GetCommandLineA, 0);
    RS ("KERNEL32.dll", SystemTimeToFileTime, 2);
    RS ("KERNEL32.dll", GetSystemTime, 1);
    RS ("KERNEL32.dll", GetLocalTime, 1);
    RS ("KERNEL32.dll", GetSystemTimeAsFileTime, 1);
    RS ("KERNEL32.dll", GetSystemTimePreciseAsFileTime, 1);
    RS ("KERNEL32.dll", FileTimeToSystemTime, 2);
    RS ("KERNEL32.dll", SystemTimeToTzSpecificLocalTime, 3);
    RS ("KERNEL32.dll", GetTimeZoneInformation, 1);
    RS ("KERNEL32.dll", GetFileAttributesA, 1);
    RS ("KERNEL32.dll", GetCurrentDirectoryA, 2);
    RS ("KERNEL32.dll", GetCurrentDirectoryW, 2);
    R1S("KERNEL32.dll", FlushFileBuffers, 1);
    RS ("KERNEL32.dll", GetDriveTypeW, 1);
    R1S("KERNEL32.dll", GetFileAttributesExW, 3);
    R1S("KERNEL32.dll", GetFileInformationByHandle, 2);
    R1S("KERNEL32.dll", GetFileSizeEx, 2);
    R1S("KERNEL32.dll", SetEndOfFile, 1);
    R1S("KERNEL32.dll", SetFilePointerEx, 5);
    RS ("KERNEL32.dll", FindFirstFileExW, 6);
    RS ("KERNEL32.dll", VirtualQuery, 3);
    RS ("KERNEL32.dll", CreateFileA, 7);
    R1S("KERNEL32.dll", DeviceIoControl, 8);
    RS ("KERNEL32.dll", VerSetConditionMask, 4);
    R1S("KERNEL32.dll", VerifyVersionInfoW, 3);
    R1S("KERNEL32.dll", GlobalMemoryStatusEx, 1);
    R1S("KERNEL32.dll", ProcessIdToSessionId, 2);
    R1S("KERNEL32.dll", SetHandleInformation, 3);
    RS ("KERNEL32.dll", GetFileTime, 4);
    R1S("KERNEL32.dll", RemoveDirectoryA, 1);
    RS ("KERNEL32.dll", OutputDebugStringW, 1);
    RS ("KERNEL32.dll", DebugBreak, 0);
    RS ("KERNEL32.dll", IsBadWritePtr, 2);  // 0 = "writable" (FALSE)
    // Dynamically resolved via GetProcAddress — must have correct arg counts
    RS ("KERNEL32.dll", AppPolicyGetProcessTerminationMethod, 2);
    RS ("KERNEL32.dll", GetCurrentPackageId, 2);
    RS ("KERNEL32.dll", GetTempPath2W, 2);
    RS ("KERNEL32.dll", CorExitProcess, 1);
    RS ("KERNEL32.dll", SetCurrentProcessExplicitAppUserModelID, 1);
    // -- USER32 / GDI / ADVAPI auto-stubbed by steam --
    RS ("USER32.dll", RegisterClassExW, 1);
    RS ("USER32.dll", UnregisterClassW, 2);
    R1S("USER32.dll", GetClassInfoExW, 3);
    RS ("USER32.dll", MessageBoxW, 4);
    RS ("USER32.dll", MessageBoxA, 4);
    RS ("USER32.dll", GetDesktopWindow, 0);
    RS ("USER32.dll", MonitorFromWindow, 2);
    RS ("USER32.dll", MonitorFromPoint, 3);
    R1S("USER32.dll", GetMonitorInfoW, 2);
    R1S("USER32.dll", PostThreadMessageW, 4);
    R1S("USER32.dll", PostThreadMessageA, 4);
    RS ("USER32.dll", GetWindowThreadProcessId, 2);
    R1S("USER32.dll", EnumWindows, 2);
    R1S("USER32.dll", EnumChildWindows, 3);
    R1S("USER32.dll", UpdateWindow, 1);
    R1S("USER32.dll", MoveWindow, 6);
    R1S("USER32.dll", RedrawWindow, 4);
    R1S("USER32.dll", KillTimer, 2);
    RS ("USER32.dll", LoadIconW, 2);
    RS ("USER32.dll", MsgWaitForMultipleObjects, 5);
    RS ("USER32.dll", MsgWaitForMultipleObjectsEx, 6);
    R1S("USER32.dll", AllowSetForegroundWindow, 1);
    RS ("GDI32.dll", GetStockObject, 1);
    RS ("GDI32.dll", CreateFontW, 14);
    RS ("GDI32.dll", CreateFontA, 14);
    R1S("GDI32.dll", GetTextExtentPoint32W, 4);
    R1S("GDI32.dll", GetTextExtentPoint32A, 4);
    RS ("GDI32.dll", CreateDIBSection, 6);
    RS ("ADVAPI32.dll", RegOpenKeyExA, 5);
    RS ("ADVAPI32.dll", RegCreateKeyExA, 9);
    RS ("ADVAPI32.dll", RegQueryValueExA, 6);
    RS ("ADVAPI32.dll", RegSetValueExA, 6);
    RS ("ADVAPI32.dll", RegOpenKeyA, 3);
    RS ("bcrypt.dll", BCryptGenRandom, 4);
    R1S("KERNEL32.dll", SystemFunction036, 2); // RtlGenRandom
    R1S("ADVAPI32.dll", SystemFunction036, 2);
    R1S("ADVAPI32.dll", RtlGenRandom, 2);
    // ProcessPrng — BoringSSL's preferred Windows entropy source (modern Steam)
    R1S("bcryptprimitives.dll", ProcessPrng, 2);
    R1S("cryptbase.dll", ProcessPrng, 2);
    R1S("bcrypt.dll", ProcessPrng, 2);
    R1S("ADVAPI32.dll", ProcessPrng, 2);
    // Legacy CryptoAPI — OpenSSL-style TLS seeds RAND through these
    R1S("ADVAPI32.dll", CryptAcquireContextW, 5);
    R1S("ADVAPI32.dll", CryptAcquireContextA, 5);
    R1S("ADVAPI32.dll", CryptGenRandom, 3);
    R1S("ADVAPI32.dll", CryptReleaseContext, 2);

    // === vcruntime / ucrt ===
    // All standard CRT functions are __cdecl — caller cleans args (num_args=0).
    // Wrong num_args here causes double stack-cleanup → crash in CRT init.
    RS ("vcruntime140.dll", memset, 0);
    RS ("vcruntime140.dll", memcpy, 0);
    RS ("vcruntime140.dll", memmove, 0);
    RS ("vcruntime140.dll", memcmp, 0);
    RS ("ucrtbase.dll", malloc, 0);
    RS ("ucrtbase.dll", free, 0);
    RS ("ucrtbase.dll", _initterm, 0);
    RS ("ucrtbase.dll", _initterm_e, 0);
    RS ("ucrtbase.dll", exit, 0);
    RS ("ucrtbase.dll", _exit, 0);
    RS ("msvcp140.dll", _Xbad_alloc, 0);
    // CRT thread spawn — __cdecl (num_args=0). Engine handlers create real
    // cooperative scheduler threads so MSVC-CRT apps' worker threads actually run.
    RS ("ucrtbase.dll", _beginthreadex, 0);
    RS ("ucrtbase.dll", _beginthread, 0);
    RS ("ucrtbase.dll", _endthreadex, 0);
    RS ("ucrtbase.dll", _endthread, 0);

    // === WS2_32 extension functions (via WSAIoctl SIO_GET_EXTENSION_FUNCTION_POINTER) ===
    RS ("WS2_32.dll", ConnectEx, 7);
    RS ("WS2_32.dll", DisconnectEx, 4);
    RS ("WS2_32.dll", AcceptEx, 8);
    RS ("WS2_32.dll", TransmitFile, 7);

    // === iphlpapi.dll ===
    RS ("IPHLPAPI.DLL", GetAdaptersAddresses, 5);
    RS ("IPHLPAPI.DLL", GetAdaptersInfo, 2);
    RS ("IPHLPAPI.DLL", GetNetworkParams, 2);
    RS ("IPHLPAPI.DLL", GetBestInterface, 2);
    RS ("IPHLPAPI.DLL", GetBestRoute, 3);
    RS ("IPHLPAPI.DLL", GetIfTable, 3);

    // === winhttp.dll ===
    RS ("winhttp.dll", WinHttpOpen, 5);
    RS ("winhttp.dll", WinHttpConnect, 4);
    RS ("winhttp.dll", WinHttpOpenRequest, 7);
    RS ("winhttp.dll", WinHttpSendRequest, 7);
    RS ("winhttp.dll", WinHttpReceiveResponse, 2);
    RS ("winhttp.dll", WinHttpQueryHeaders, 6);
    RS ("winhttp.dll", WinHttpQueryDataAvailable, 2);
    RS ("winhttp.dll", WinHttpReadData, 4);
    RS ("winhttp.dll", WinHttpWriteData, 4);
    RS ("winhttp.dll", WinHttpCloseHandle, 1);
    RS ("winhttp.dll", WinHttpSetOption, 4);
    RS ("winhttp.dll", WinHttpAddRequestHeaders, 4);
    RS ("winhttp.dll", WinHttpCrackUrl, 4);
    RS ("winhttp.dll", WinHttpSetStatusCallback, 4);
    RS ("winhttp.dll", WinHttpSetTimeouts, 5);
    RS ("winhttp.dll", WinHttpGetProxyForUrl, 4);
    RS ("winhttp.dll", WinHttpGetDefaultProxyConfiguration, 1);
    RS ("winhttp.dll", WinHttpGetIEProxyConfigForCurrentUser, 1);

    // === wininet.dll ===
    R1S("wininet.dll", InternetGetConnectedState, 2);
    RS ("wininet.dll", InternetAttemptConnect, 1);
    RS ("wininet.dll", InternetOpenA, 5);
    RS ("wininet.dll", InternetOpenW, 5);
    RS ("wininet.dll", InternetConnectA, 8);
    RS ("wininet.dll", InternetConnectW, 8);
    RS ("wininet.dll", HttpOpenRequestA, 8);
    RS ("wininet.dll", HttpOpenRequestW, 8);
    RS ("wininet.dll", HttpSendRequestA, 5);
    RS ("wininet.dll", HttpSendRequestW, 5);
    RS ("wininet.dll", HttpSendRequestExA, 5);
    RS ("wininet.dll", HttpSendRequestExW, 5);
    RS ("wininet.dll", HttpQueryInfoA, 5);
    RS ("wininet.dll", HttpQueryInfoW, 5);
    RS ("wininet.dll", InternetReadFile, 4);
    RS ("wininet.dll", InternetCloseHandle, 1);
    RS ("wininet.dll", InternetSetOptionA, 4);
    RS ("wininet.dll", InternetSetOptionW, 4);
    RS ("wininet.dll", InternetQueryOptionA, 4);
    RS ("wininet.dll", InternetQueryOptionW, 4);
    R1S("wininet.dll", InternetCheckConnectionA, 3);
    R1S("wininet.dll", InternetCheckConnectionW, 3);
    RS ("wininet.dll", InternetQueryDataAvailable, 4);
    RS ("wininet.dll", InternetSetStatusCallbackA, 2);
    RS ("wininet.dll", InternetSetStatusCallbackW, 2);
    R1S("wininet.dll", InternetGetConnectedStateExW, 3);

    // === crypt32.dll / secur32.dll ===
    R1S("crypt32.dll", CertOpenSystemStoreW, 2);
    R1S("crypt32.dll", CertOpenSystemStoreA, 2);
    R1S("crypt32.dll", CertCloseStore, 2);
    RS ("crypt32.dll", CertFreeCertificateContext, 1);
    RS ("crypt32.dll", CertEnumCertificatesInStore, 2);
    R1S("crypt32.dll", CertGetCertificateChain, 8);
    RS ("crypt32.dll", CertFreeCertificateChain, 1);
    R1S("crypt32.dll", CertVerifyCertificateChainPolicy, 4);
    R1S("crypt32.dll", CryptProtectData, 7);
    R1S("crypt32.dll", CryptUnprotectData, 7);
    R1S("crypt32.dll", CryptStringToBinaryW, 7);
    R1S("crypt32.dll", CryptBinaryToStringW, 5);
    RS ("crypt32.dll", CertFindCertificateInStore, 6);
    RS ("crypt32.dll", CertGetNameStringW, 5);
    RS ("secur32.dll", InitSecurityInterfaceW, 0);
    RS ("secur32.dll", InitSecurityInterfaceA, 0);
    RS ("secur32.dll", AcquireCredentialsHandleW, 9);
    RS ("secur32.dll", AcquireCredentialsHandleA, 9);
    RS ("secur32.dll", FreeCredentialsHandle, 1);
    RS ("secur32.dll", InitializeSecurityContextW, 12);
    RS ("secur32.dll", InitializeSecurityContextA, 12);
    RS ("secur32.dll", DeleteSecurityContext, 1);
    RS ("secur32.dll", QueryContextAttributesW, 3);
    RS ("secur32.dll", QueryContextAttributesA, 3);
    RS ("secur32.dll", FreeContextBuffer, 1);
    RS ("secur32.dll", EncryptMessage, 4);
    RS ("secur32.dll", DecryptMessage, 4);
    RS ("secur32.dll", CompleteAuthToken, 2);
    RS ("secur32.dll", ApplyControlToken, 2);
    RS ("sspicli.dll", AcquireCredentialsHandleW, 9);
    RS ("sspicli.dll", InitializeSecurityContextW, 12);
    RS ("sspicli.dll", DeleteSecurityContext, 1);
    RS ("sspicli.dll", FreeCredentialsHandle, 1);
    RS ("sspicli.dll", QueryContextAttributesW, 3);
    RS ("sspicli.dll", FreeContextBuffer, 1);
    RS ("sspicli.dll", EncryptMessage, 4);
    RS ("sspicli.dll", DecryptMessage, 4);

    // === USER32.dll ===
    RS("USER32.dll", GetSystemMenu, 2);
    RS("USER32.dll", SetClassLongW, 3);
    R1S("USER32.dll", IsWindowEnabled, 1);
    RS("USER32.dll", EnableMenuItem, 3);
    R1S("USER32.dll", SetWindowPos, 7);
    R("USER32.dll", GetSysColor, stub_GetSysColor, 1);
    RS("USER32.dll", GetWindowLongW, 2);
    RS("USER32.dll", SetCursor, 1);
    R1S("USER32.dll", LoadCursorW, 2);
    R1S("USER32.dll", CheckDlgButton, 3);
    RS("USER32.dll", GetMessagePos, 0);
    RS("USER32.dll", LoadBitmapW, 2);
    RS("USER32.dll", CallWindowProcW, 5);
    RS("USER32.dll", IsWindowVisible, 1);
    R1S("USER32.dll", CloseClipboard, 0);
    RS("USER32.dll", SetClipboardData, 2);
    R1S("USER32.dll", EmptyClipboard, 0);
    R1S("USER32.dll", OpenClipboard, 1);
    RS("USER32.dll", wsprintfW, 0); // cdecl variadic — caller cleans
    R1S("USER32.dll", ScreenToClient, 2);
    R1S("USER32.dll", GetWindowRect, 2);
    RS("USER32.dll", GetSystemMetrics, 1);
    R1S("USER32.dll", SetDlgItemTextW, 3);
    RS("USER32.dll", GetDlgItemTextW, 4);
    R("USER32.dll", MessageBoxIndirectW, stub_MessageBoxIndirectW, 1);
    RS("USER32.dll", CharPrevW, 2);
    RS("USER32.dll", CharNextA, 1);
    RS("USER32.dll", wsprintfA, 0); // cdecl variadic — caller cleans
    RS("USER32.dll", DispatchMessageW, 1);
    R("USER32.dll", PeekMessageW, stub_PeekMessageW, 5);
    R1S("USER32.dll", GetDC, 1);
    R1S("USER32.dll", ReleaseDC, 2);
    RS("USER32.dll", EnableWindow, 2);
    R1S("USER32.dll", InvalidateRect, 3);
    RS("USER32.dll", SendMessageW, 4);
    RS("USER32.dll", SendDlgItemMessageW, 5);
    RS("USER32.dll", DefWindowProcW, 4);
    R1S("USER32.dll", BeginPaint, 2);
    R1S("USER32.dll", GetClientRect, 2);
    R1S("USER32.dll", FillRect, 3);
    R1S("USER32.dll", EndDialog, 2);
    R("USER32.dll", RegisterClassW, stub_RegisterClassW, 1);
    R1S("USER32.dll", SystemParametersInfoW, 4);
    R("USER32.dll", CreateWindowExW, stub_CreateWindowExW, 12);
    R1S("USER32.dll", GetClassInfoW, 3);
    RS("USER32.dll", DialogBoxParamW, 5);
    RS("USER32.dll", CharNextW, 1);
    R1S("USER32.dll", ExitWindowsEx, 2);
    R1S("USER32.dll", DestroyWindow, 1);
    RS("USER32.dll", LoadImageW, 6);
    R1S("USER32.dll", SetTimer, 4);
    R1S("USER32.dll", SetWindowTextW, 2);
    RS ("USER32.dll", GetWindowTextW, 3);
    RS ("USER32.dll", GetWindowTextLengthW, 1);
    RS("USER32.dll", PostQuitMessage, 1);
    R1S("USER32.dll", ShowWindow, 2);
    RS("USER32.dll", GetDlgItem, 2);
    RS("USER32.dll", IsWindow, 1);
    RS("USER32.dll", SetWindowLongW, 3);
    RS("USER32.dll", FindWindowExW, 4);
    RS("USER32.dll", TrackPopupMenu, 7);
    R1S("USER32.dll", AppendMenuW, 4);
    R1S("USER32.dll", CreatePopupMenu, 0);
    RS("USER32.dll", DrawTextW, 5);
    R1S("USER32.dll", EndPaint, 2);
    RS("USER32.dll", CreateDialogParamW, 5);
    RS("USER32.dll", SendMessageTimeoutW, 7);
    R1S("USER32.dll", SetForegroundWindow, 1);
    RS("USER32.dll", GetMessageA, 4);
    // Window message pump + GDI device contexts (handled in engine)
    RS("USER32.dll", GetMessageW, 4);
    RS("USER32.dll", TranslateMessage, 1);
    RS("USER32.dll", DispatchMessageW, 1);
    RS("USER32.dll", DefWindowProcW, 4);
    RS("USER32.dll", GetDC, 1);
    RS("USER32.dll", ReleaseDC, 2);
    RS("USER32.dll", BeginPaint, 2);
    RS("USER32.dll", FillRect, 3);

    // === GDI32.dll ===
    RS("GDI32.dll", SelectObject, 2);
    RS("GDI32.dll", SetBkMode, 2);
    R1S("GDI32.dll", CreateFontIndirectW, 1);
    R1S("GDI32.dll", CreateFontIndirectA, 1);

    // nsDialogs plugin exports. NSIS plugins are __cdecl (the caller cleans the
    // 5 args), so num_args MUST be 0 here — a non-zero count double-cleans the
    // stack and crashes NSIS right after the call returns. The engine dispatches
    // these natively (handle_nsdialogs) to build custom wizard pages.
    RS("nsDialogs.dll", Create, 0);
    RS("nsDialogs.dll", CreateControl, 0);
    RS("nsDialogs.dll", CreateItem, 0);
    RS("nsDialogs.dll", Show, 0);
    RS("nsDialogs.dll", SetImage, 0);
    RS("nsDialogs.dll", SetIcon, 0);
    RS("nsDialogs.dll", SetUserData, 0);
    RS("nsDialogs.dll", GetUserData, 0);
    RS("nsDialogs.dll", OnClick, 0);
    RS("nsDialogs.dll", OnChange, 0);
    RS("nsDialogs.dll", OnNotify, 0);
    RS("nsDialogs.dll", OnBack, 0);
    RS("nsDialogs.dll", SetRTL, 0);
    RS("nsDialogs.dll", CreateTimer, 0);
    RS("nsDialogs.dll", KillTimer, 0);
    RS("nsDialogs.dll", SelectFileDialog, 0);
    RS("nsDialogs.dll", SelectFolderDialog, 0);
    RS("nsDialogs.dll", SetButtonLong, 0);
    RS("GDI32.dll", SetTextColor, 2);
    R1S("GDI32.dll", DeleteObject, 1);
    R("GDI32.dll", GetDeviceCaps, stub_GetDeviceCaps, 2);
    R1S("GDI32.dll", CreateBrushIndirect, 1);
    RS("GDI32.dll", SetBkColor, 2);
    RS("GDI32.dll", CreateSolidBrush, 1);
    RS("GDI32.dll", TextOutW, 5);
    RS("GDI32.dll", MoveToEx, 4);
    RS("GDI32.dll", LineTo, 3);
    // Bitmaps / blitting (handled in engine; registered here for correct
    // stdcall stack cleanup — wrong arg counts corrupt the guest stack).
    RS("GDI32.dll", CreateCompatibleDC, 1);
    RS("GDI32.dll", DeleteDC, 1);
    RS("GDI32.dll", CreateCompatibleBitmap, 3);
    RS("GDI32.dll", GetObjectW, 3);
    RS("GDI32.dll", BitBlt, 9);
    RS("GDI32.dll", StretchBlt, 11);
    RS("GDI32.dll", SetStretchBltMode, 2);
    RS("GDI32.dll", StretchDIBits, 13);
    RS("GDI32.dll", SetDIBitsToDevice, 12);

    // === SHELL32.dll ===
    RS("SHELL32.dll", CommandLineToArgvW, 2);
    R1S("SHELL32.dll", IsUserAnAdmin, 0);
    // SHGetFolderPathW: 5-arg stdcall. Register under KERNEL32 too because our
    // GetProcAddress resolves with a hardcoded KERNEL32 dll name — without a
    // correct-arg registration it auto-stubs with num_args=0 and desyncs the
    // stack on every call. The engine has an explicit handler that fills a path.
    RS("SHELL32.dll", SHGetFolderPathW, 5);
    RS("KERNEL32.dll", SHGetFolderPathW, 5);
    R("SHELL32.dll", SHGetSpecialFolderLocation, stub_SHGetSpecialFolderLocation, 3);
    RS("SHELL32.dll", SHGetPathFromIDListW, 2);
    RS("SHELL32.dll", SHBrowseForFolderW, 1);
    RS("SHELL32.dll", SHGetFileInfoW, 5);
    R("SHELL32.dll", ShellExecuteW, stub_ShellExecuteW, 6);
    RS("SHELL32.dll", SHFileOperationW, 1);
    R1S("SHELL32.dll", ShellExecuteExW, 1);

    // === SHLWAPI.dll === (path helpers — the UE4/Visage launcher builds its
    // shipping-exe path with these; engine handlers write the real results)
    R1S("SHLWAPI.dll", PathRemoveFileSpecW, 1);
    RS("SHLWAPI.dll", PathCombineW, 3);
    R1S("SHLWAPI.dll", PathCanonicalizeW, 2);
    R1S("SHLWAPI.dll", PathFileExistsW, 1);
    RS("SHLWAPI.dll", PathFindFileNameW, 1);
    RS("SHLWAPI.dll", PathAppendW, 2);

    // === ADVAPI32.dll ===
    RS("ADVAPI32.dll", RegDeleteKeyW, 2);
    R1S("ADVAPI32.dll", SetFileSecurityW, 3);
    R1S("ADVAPI32.dll", OpenProcessToken, 3);
    R1S("ADVAPI32.dll", LookupPrivilegeValueW, 3);
    R1S("ADVAPI32.dll", AdjustTokenPrivileges, 6);
    R("ADVAPI32.dll", RegOpenKeyExW, stub_RegOpenKeyExW, 5);
    R("ADVAPI32.dll", RegEnumValueW, stub_RegEnumValueW, 8);
    RS("ADVAPI32.dll", RegDeleteValueW, 2);
    RS("ADVAPI32.dll", RegCloseKey, 1);
    RS("ADVAPI32.dll", RegCreateKeyExW, 9);
    RS("ADVAPI32.dll", RegSetValueExW, 6);
    R("ADVAPI32.dll", RegQueryValueExW, stub_RegQueryValueExW, 6);
    R("ADVAPI32.dll", RegEnumKeyW, stub_RegEnumKeyW, 4);

    // === COMCTL32.dll ===
    RS("COMCTL32.dll", ImageList_AddMasked, 3);
    RS("COMCTL32.dll", Ordinal_17, 1);
    R1S("COMCTL32.dll", ImageList_Destroy, 1);
    R1S("COMCTL32.dll", ImageList_Create, 5);

    // === OLEAUT32.dll ===
    RS("OLEAUT32.dll", Ordinal_9, 1);   // VariantClear(VARIANT*)

    // === ole32.dll ===
    RS("ole32.dll", OleUninitialize, 0);
    RS("ole32.dll", OleInitialize, 1);
    RS("ole32.dll", CoTaskMemFree, 1);
    R("ole32.dll", CoCreateInstance, stub_CoCreateInstance, 5);

    // === ntdll.dll ===
    RS("ntdll.dll", RtlInitializeCriticalSection, 1);

    // === WS2_32.dll (Winsock2) ===
    // WS2_32 exports by ordinal. The ordinal numbers below are from the
    // Windows 10 WS2_32.dll export table. Ordinals 1-23 are the Berkeley
    // socket functions; higher ordinals are WSA extensions.
    RS("WS2_32.dll", Ordinal_1,   3);  // accept(s, addr, addrlen)
    RS("WS2_32.dll", Ordinal_2,   3);  // bind
    RS("WS2_32.dll", Ordinal_3,   1);  // closesocket
    RS("WS2_32.dll", Ordinal_4,   3);  // connect
    RS("WS2_32.dll", Ordinal_5,   3);  // getpeername
    RS("WS2_32.dll", Ordinal_6,   3);  // getsockname
    RS("WS2_32.dll", Ordinal_7,   5);  // getsockopt
    RS("WS2_32.dll", Ordinal_8,   1);  // htonl
    RS("WS2_32.dll", Ordinal_9,   1);  // htons
    RS("WS2_32.dll", Ordinal_10,  3);  // ioctlsocket
    RS("WS2_32.dll", Ordinal_11,  1);  // inet_addr
    RS("WS2_32.dll", Ordinal_12,  1);  // inet_ntoa
    RS("WS2_32.dll", Ordinal_13,  2);  // listen
    RS("WS2_32.dll", Ordinal_14,  1);  // ntohl
    RS("WS2_32.dll", Ordinal_15,  1);  // ntohs
    RS("WS2_32.dll", Ordinal_16,  4);  // recv
    RS("WS2_32.dll", Ordinal_17,  6);  // recvfrom
    RS("WS2_32.dll", Ordinal_18,  5);  // select
    RS("WS2_32.dll", Ordinal_19,  4);  // send
    RS("WS2_32.dll", Ordinal_20,  6);  // sendto
    RS("WS2_32.dll", Ordinal_21,  5);  // setsockopt
    RS("WS2_32.dll", Ordinal_22,  2);  // shutdown
    RS("WS2_32.dll", Ordinal_23,  3);  // socket
    RS("WS2_32.dll", Ordinal_52,  2);  // gethostbyname (legacy)
    RS("WS2_32.dll", Ordinal_111, 0);  // WSAGetLastError (0 args)
    RS("WS2_32.dll", Ordinal_112, 1);  // WSASetLastError (1 arg)
    RS("WS2_32.dll", Ordinal_113, 0);  // WSACancelBlockingCall (legacy)
    RS("WS2_32.dll", Ordinal_115, 2);  // WSAStartup
    RS("WS2_32.dll", Ordinal_116, 0);  // WSACleanup
    // NOTE: Ordinal_151 is __WSAFDIsSet(s, fd_set*) = 2 args. It used to be
    // ALSO registered here as 4 args ("WSARecvMsg"); since registration appends
    // and resolve returns the FIRST match, that wrong 4-arg entry won and made
    // every FD_ISSET over-pop 8 bytes of stack — corrupting the caller's saved
    // registers (e.g. clobbering Steam's CTCPConnection 'this'/esi to 0, which
    // turned the manifest send into send(socket=0) and killed the download).
    // Registered once, correctly, below.
    // Named imports (some are by name, not ordinal)
    RS("WS2_32.dll", WSAStartup, 2);
    RS("WS2_32.dll", WSACleanup, 0);
    RS("WS2_32.dll", WSAGetLastError, 0);
    RS("WS2_32.dll", WSASetLastError, 1);
    RS("WS2_32.dll", WSASend, 7);
    RS("WS2_32.dll", WSASendTo, 9);
    RS("WS2_32.dll", WSARecv, 7);
    RS("WS2_32.dll", WSARecvFrom, 9);
    RS("WS2_32.dll", WSASocketW, 6);
    RS("WS2_32.dll", WSASocketA, 6);
    RS("WS2_32.dll", Ordinal_151, 2); // __WSAFDIsSet(s, fd_set*)
    RS("WS2_32.dll", WSAIoctl, 9);
    RS("WS2_32.dll", WSAEventSelect, 3);
    RS("WS2_32.dll", WSAEnumNetworkEvents, 3);
    R1S("WS2_32.dll", WSACreateEvent, 0);   // non-null handle (0 = WSA_INVALID_EVENT)
    R1S("WS2_32.dll", WSACloseEvent, 1);
    R1S("WS2_32.dll", WSAResetEvent, 1);
    R1S("WS2_32.dll", WSASetEvent, 1);
    // WSAWaitForMultipleEvents: default-return 0 = WSA_WAIT_EVENT_0 so the app
    // proceeds to WSAEnumNetworkEvents (which polls the real socket state).
    RS ("WS2_32.dll", WSAWaitForMultipleEvents, 5);
    RS("WS2_32.dll", getaddrinfo, 4);
    RS("WS2_32.dll", freeaddrinfo, 1);
    RS("WS2_32.dll", gethostname, 2);
    RS("WS2_32.dll", gethostbyname, 1);
    RS("WS2_32.dll", Ordinal_57, 2);   // gethostname (legacy ordinal)
    RS("WS2_32.dll", socket, 3);
    RS("WS2_32.dll", closesocket, 1);
    // __stdcall/4 — MUST be registered, else auto-stub (num_args=0) leaks 16
    // bytes of guest stack and corrupts the NEXT socket call's args.
    RS("WS2_32.dll", inet_ntop, 4);
    RS("WS2_32.dll", inet_pton, 4);
    RS("WS2_32.dll", InetNtopA, 4);
    RS("WS2_32.dll", InetPtonA, 4);
    RS("WS2_32.dll", connect, 3);
    RS("WS2_32.dll", send, 4);
    RS("WS2_32.dll", recv, 4);
    RS("WS2_32.dll", bind, 3);
    RS("WS2_32.dll", listen, 2);
    RS("WS2_32.dll", select, 5);
    RS("WS2_32.dll", shutdown, 2);
    RS("WS2_32.dll", setsockopt, 5);
    RS("WS2_32.dll", getsockopt, 5);
    RS("WS2_32.dll", ioctlsocket, 3);
    RS("WS2_32.dll", htons, 1);
    RS("WS2_32.dll", htonl, 1);
    RS("WS2_32.dll", ntohs, 1);
    RS("WS2_32.dll", ntohl, 1);
    RS("WS2_32.dll", inet_addr, 1);
    RS("WS2_32.dll", inet_ntoa, 1);
    RS("WS2_32.dll", getpeername, 3);
    RS("WS2_32.dll", getsockname, 3);

    // === WSOCK32.dll ===
    RS("WSOCK32.dll", Ordinal_1142, 2); // WSAStartup (legacy alias)

    WG_LOGI(TAG, "Registered %d Win32 API stubs", m->count);
}
