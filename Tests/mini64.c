// Minimal 64-bit PE: no CRT, imports only GetTickCount + ExitProcess from kernel32.
// Boots on WineGlass to validate the box64 CPU + Win32 thunk path end-to-end.
__declspec(dllimport) unsigned int __stdcall GetTickCount(void);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int);
void mainCRTStartup(void) {
    unsigned int t = GetTickCount();     // thunk #1
    ExitProcess((t & 0xFF) ^ 0x2A);      // thunk #2 -> stops the engine
}
