#include "mouse.h"

#include "hotkeys.h"

#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

namespace {

std::atomic<bool> g_running{false};
std::atomic<bool> g_stop{false};

double nowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / freq;
}

// Precise wait: sleeps for the bulk, spins for the last fraction of a millisecond.
void waitMs(double ms) {
    const double end = nowMs() + ms;
    for (;;) {
        double left = end - nowMs();
        if (left <= 0) return;
        if (left > 2.0) Sleep(DWORD(left - 1.0));
        else if (left > 0.25) Sleep(0);
        else YieldProcessor();
    }
}

class Injector {
public:
    explicit Injector(bool relative) : relative_(relative) {
        vx_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
        vy_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
        vw_ = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
        vh_ = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
        swapped_ = GetSystemMetrics(SM_SWAPBUTTON) != 0;
        GetCursorPos(&last_);
        prev_ = last_;
    }

    POINT clampToScreen(POINT p) const {
        p.x = std::clamp(p.x, LONG(vx_), LONG(vx_ + vw_ - 1));
        p.y = std::clamp(p.y, LONG(vy_), LONG(vy_ + vh_ - 1));
        return p;
    }

    void move(POINT p) {
        p = clampToScreen(p);
        prev_ = last_;
        last_ = p;
        if (relative_) {
            moveRelative(p);
            return;
        }
        INPUT in{};
        in.type = INPUT_MOUSE;
        // Windows maps back with floor(d * size / 65536); round up so we land on the exact pixel.
        in.mi.dx = LONG((int64_t(p.x - vx_) * 65536 + vw_ - 1) / vw_);
        in.mi.dy = LONG((int64_t(p.y - vy_) * 65536 + vh_ - 1) / vh_);
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        SendInput(1, &in, sizeof in);
    }

    void press(bool down) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        // With swapped buttons the physical right button acts as the primary one.
        if (swapped_) in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        else in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        SendInput(1, &in, sizeof in);
        down_ = down;
    }

    bool isDown() const { return down_; }
    POINT last() const { return last_; }

    // The cursor is far from both of the last two positions we sent: somebody else moved it.
    bool farFromUs(int tol) const {
        POINT c;
        if (!GetCursorPos(&c)) return false;
        auto isFar = [&](POINT q) { return std::abs(c.x - q.x) > tol || std::abs(c.y - q.y) > tol; };
        return isFar(last_) && isFar(prev_);
    }

private:
    void moveRelative(POINT p) {
        // Relative moves are scaled by pointer speed/acceleration: correct until we arrive.
        for (int i = 0; i < 4; ++i) {
            POINT c;
            if (!GetCursorPos(&c)) return;
            int dx = p.x - c.x, dy = p.y - c.y;
            if (!dx && !dy) return;
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dx = dx;
            in.mi.dy = dy;
            in.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &in, sizeof in);
            waitMs(1.0);
        }
    }

    bool relative_;
    bool swapped_ = false;
    bool down_ = false;
    int vx_ = 0, vy_ = 0, vw_ = 1, vh_ = 1;
    POINT last_{}, prev_{};
};

bool keyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Waits until the thread of `w` has taken the input sent so far. A game answers messages
// only between two frames, so a game that freezes for a moment is waited for instead of
// missing a press or a release (which would join two strokes with a line).
void syncWith(HWND w) {
    DWORD_PTR res = 0;
    SendMessageTimeoutW(w, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &res);
}

DWORD WINAPI worker(LPVOID arg) {
    std::unique_ptr<DrawJob> job(static_cast<DrawJob*>(arg));
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    const dz::Timing& t = job->timing;
    const HWND notify = job->notify;
    const int total = int(job->strokes.size());
    const int tol = std::max(30, int(t.stepPx * 4));
    Injector inj(job->relative);
    DrawResult result = DrawResult::Completed;
    int done = 0;
    double lastProgress = 0;
    bool pauseKeyWasDown = keyDown(VK_F8) || keyDown(VK_PAUSE);

    // The keyboard hook sees every key tap; polling is only the fallback when it is unavailable.
    auto pausePressed = [&] {
        if (hotkeys::active()) return hotkeys::takePause();
        bool d = keyDown(VK_F8) || keyDown(VK_PAUSE);
        bool edge = d && !pauseKeyWasDown;
        pauseKeyWasDown = d;
        return edge;
    };
    auto stopPressed = [&] { return hotkeys::takeStop() || (!hotkeys::active() && keyDown(VK_ESCAPE)); };
    auto toScreen = [&](float x, float y) {
        // Stroke points sit at pixel centres (x.5): the pixel is the one that contains them.
        return POINT{job->origin.x + LONG(std::floor(x)), job->origin.y + LONG(std::floor(y))};
    };

    // Checks stop/pause keys and the safety stop. Returns false when drawing must end.
    auto poll = [&]() -> bool {
        if (g_stop || stopPressed()) {
            result = DrawResult::Stopped;
            return false;
        }
        if (job->failsafe && inj.farFromUs(tol)) {
            waitMs(20);  // let queued moves land before deciding
            if (inj.farFromUs(tol)) {
                result = DrawResult::UserMoved;
                return false;
            }
        }
        if (pausePressed()) {
            const bool wasDown = inj.isDown();
            const POINT at = inj.last();
            if (wasDown) inj.press(false);
            PostMessageW(notify, WM_APP_DRAW_PAUSED, 1, 0);
            for (;;) {
                Sleep(20);
                if (g_stop || stopPressed()) {
                    result = DrawResult::Stopped;
                    PostMessageW(notify, WM_APP_DRAW_PAUSED, 0, 0);
                    return false;
                }
                if (pausePressed()) break;
            }
            PostMessageW(notify, WM_APP_DRAW_PAUSED, 0, 0);
            inj.move(at);
            waitMs(std::max(40.f, t.downDelayMs));
            inj.move(at);
            if (wasDown) {
                inj.press(true);
                waitMs(t.downDelayMs);
            }
        }
        return true;
    };

    std::vector<dz::Step> steps;
    for (int si = 0; si < total; ++si) {
        steps.clear();
        dz::planStroke(job->strokes[size_t(si)], t, steps);
        bool ok = true;
        for (const dz::Step& st : steps) {
            const POINT at = toScreen(st.at.x, st.at.y);
            const POINT last = inj.last();
            if (at.x != last.x || at.y != last.y) inj.move(at);
            if (st.down != inj.isDown()) inj.press(st.down);
            if (st.sync && job->syncWindow) syncWith(job->syncWindow);
            waitMs(st.ms);
            if (!poll()) {
                ok = false;
                break;
            }
        }
        if (!ok) break;
        ++done;
        const double now = nowMs();
        if (now - lastProgress > 100 || done == total) {
            PostMessageW(notify, WM_APP_DRAW_PROGRESS, WPARAM(done), LPARAM(total));
            lastProgress = now;
        }
    }
    if (inj.isDown()) inj.press(false);
    timeEndPeriod(1);
    g_running = false;
    PostMessageW(notify, WM_APP_DRAW_DONE, WPARAM(result), LPARAM(done));
    return 0;
}

}  // namespace

namespace mouse {

bool start(DrawJob* job) {
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) {
        delete job;
        return false;
    }
    g_stop = false;
    HANDLE th = CreateThread(nullptr, 0, worker, job, 0, nullptr);
    if (!th) {
        g_running = false;
        delete job;
        return false;
    }
    CloseHandle(th);
    return true;
}

bool running() { return g_running; }

void requestStop() { g_stop = true; }

}  // namespace mouse
