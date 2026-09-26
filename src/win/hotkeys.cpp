#include "hotkeys.h"

#include <atomic>

#include "winutil.h"

namespace hotkeys {

namespace {

std::atomic<int> g_stop{0};
std::atomic<int> g_pause{0};
std::atomic<bool> g_active{false};
HANDLE g_thread = nullptr;
DWORD g_threadId = 0;
HANDLE g_ready = nullptr;
// Only touched by the hook thread (and reset before it starts).
bool g_escDown = false;
bool g_pauseDown = false;

LRESULT CALLBACK hookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        const bool up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
        if (k->vkCode == VK_ESCAPE) {
            if (down && !g_escDown) ++g_stop;
            if (down) g_escDown = true;
            else if (up) g_escDown = false;
            return 1;
        }
        if (k->vkCode == VK_F8 || k->vkCode == VK_PAUSE) {
            if (down && !g_pauseDown) ++g_pause;
            if (down) g_pauseDown = true;
            else if (up) g_pauseDown = false;
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

DWORD WINAPI hookThread(LPVOID) {
    MSG m;
    PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // make sure the thread has a queue
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, hookProc, GetModuleHandleW(nullptr), 0);
    g_active = hook != nullptr;
    SetEvent(g_ready);
    if (hook) {
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        }
        UnhookWindowsHookEx(hook);
    }
    g_active = false;
    return 0;
}

}  // namespace

void enable() {
    if (g_thread) return;
    g_stop = 0;
    g_pause = 0;
    g_escDown = g_pauseDown = false;
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, hookThread, nullptr, 0, &g_threadId);
    if (g_thread && g_ready) WaitForSingleObject(g_ready, 2000);
    if (g_ready) CloseHandle(g_ready);
    g_ready = nullptr;
}

void disable() {
    if (!g_thread) return;
    PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_threadId = 0;
    g_active = false;
}

bool active() { return g_active; }

bool takeStop() { return g_stop.exchange(0) > 0; }

bool takePause() {
    int n = g_pause.load();
    while (n > 0) {
        if (g_pause.compare_exchange_weak(n, n - 1)) return true;
    }
    return false;
}

}  // namespace hotkeys
