// core.cpp - engine + pure logic for Ultimate AutoClicker.
#include "core.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace ac {

bool IsScreenEdge(int x, int y, int left, int top, int right, int bottom) {
    return x >= left && x < right && y >= top && y < bottom &&
        (x - left < kEmergencyEdgePx || right - x <= kEmergencyEdgePx ||
         y - top < kEmergencyEdgePx || bottom - y <= kEmergencyEdgePx);
}

// ---------------------------------------------------------------- hotkey
std::wstring Combo::Name() const {
    std::wstring out;
    if (mods & MOD_CONTROL) out += L"Ctrl + ";
    if (mods & MOD_SHIFT)   out += L"Shift + ";
    if (mods & MOD_ALT)     out += L"Alt + ";
    if (mods & MOD_WIN)     out += L"Win + ";
    for (size_t i = 0; i < keys.size(); i++) {
        if (i) out += L" + ";
        wchar_t buf[64] = {0};
#ifdef _WIN32
        LONG scan = MapVirtualKeyW(keys[i], MAPVK_VK_TO_VSC) << 16;
        switch (keys[i]) {
            case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
            case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
            case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
                scan |= 0x01000000; break;
        }
        GetKeyNameTextW(scan, buf, 64);
#else
        if (keys[i] >= 32 && keys[i] <= 126) { buf[0] = (wchar_t)keys[i]; }
        else if (keys[i] >= 112 && keys[i] <= 135)
            swprintf_s(buf, 64, L"F%d", keys[i] - 111);
#endif
        if (buf[0]) out += buf; else { wchar_t t[16]; swprintf_s(t, 16, L"VK_%d", keys[i]); out += t; }
    }
    return out;
}

std::wstring Combo::Encode() const {
    wchar_t buf[64];
    swprintf_s(buf, 64, L"%d:", (int)mods);
    std::wstring out = buf;
    for (size_t i = 0; i < keys.size(); i++) {
        if (i) out += L",";
        swprintf_s(buf, 64, L"%d", keys[i]);
        out += buf;
    }
    return out;
}

bool Combo::Decode(const std::wstring& s) {
    mods = 0; keys.clear();
    if (s.empty()) return false;
    size_t colon = s.find(L':');
    if (colon == std::wstring::npos) return false;
    mods = (BYTE)wcstol(s.substr(0, colon).c_str(), nullptr, 10);
    std::wstring rest = s.substr(colon + 1);
    size_t pos = 0;
    while (pos <= rest.size() && keys.size() < 3) {
        size_t comma = rest.find(L',', pos);
        std::wstring tok = rest.substr(pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
        if (!tok.empty()) keys.push_back((int)wcstol(tok.c_str(), nullptr, 10));
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    return !keys.empty();
}

bool ComboMatches(const Combo& c, const std::vector<int>& pressed, BYTE heldMods) {
    if (c.keys.empty()) return false;
    if (heldMods != c.mods) return false;
    if (pressed.size() != c.keys.size()) return false;
    for (size_t i = 0; i < c.keys.size(); i++) {
        bool found = false;
        for (size_t j = 0; j < pressed.size(); j++) {
            if (pressed[j] == c.keys[i]) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

// ---------------------------------------------------------------- intervals
long long FieldsToUs(double h, double m, double s, double ms, double us) {
    double total = us + ms * 1000.0 + s * 1000000.0 + m * 60000000.0 + h * 3600000000.0;
    if (total < 0) total = 0;
    return (long long)(total + 0.5);
}

void UsToFields(long long totalUs, double& h, double& m, double& s, double& ms, double& us) {
    if (totalUs < 0) totalUs = 0;
    long long rem = totalUs;
    h = (double)(rem / 3600000000LL); rem %= 3600000000LL;
    m = (double)(rem / 60000000LL);  rem %= 60000000LL;
    s = (double)(rem / 1000000LL);   rem %= 1000000LL;
    ms = (double)(rem / 1000LL);     rem %= 1000LL;
    us = (double)rem;
}

long long ClampIntervalUs(long long us) {
    if (us < kMinIntervalUs) return kMinIntervalUs;
    if (us > kMaxIntervalUs) return kMaxIntervalUs;
    return us;
}

// ---------------------------------------------------------------- config codec
static std::wstring Esc(const std::wstring& v) {
    std::wstring o;
    for (wchar_t ch : v) {
        if (ch == L'\\')      o += L"\\\\";
        else if (ch == L';')  o += L"\\;";
        else if (ch == L'=')  o += L"\\=";
        else if (ch == L'\n') o += L"\\n";
        else if (ch == L'\r') o += L"\\r";
        else o += ch;
    }
    return o;
}

static std::wstring Unesc(const std::wstring& v) {
    std::wstring o;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] == L'\\' && i + 1 < v.size()) {
            wchar_t n = v[++i];
            if (n == L'n') o += L'\n';
            else if (n == L'r') o += L'\r';
            else o += n;
        } else o += v[i];
    }
    return o;
}

std::wstring EncodeConfig(const Config& c) {
    std::wstring s;
    wchar_t buf[256];
    auto add = [&](const wchar_t* k, long long v) {
        swprintf_s(buf, 256, L"%ls=%lld;", k, v);
        s += buf;
    };
    add(L"interval_us", c.intervalUs);
    add(L"randomize", c.randomize ? 1 : 0);
    add(L"rand_min_us", c.randMinUs);
    add(L"rand_max_us", c.randMaxUs);
    add(L"mouse_btn", c.mouseBtn);
    add(L"click_type", c.clickType);
    add(L"click_mode", c.clickMode);
    add(L"action", c.action);
    add(L"pos_mode", c.posMode);
    add(L"target_x", c.targetX);
    add(L"target_y", c.targetY);
    add(L"max_clicks", c.maxClicks);
    add(L"hold_us", c.clickHoldUs);
    add(L"start_delay", c.startDelaySec);
    s += L"type_text=" + Esc(c.typeText) + L";";
    s += L"hotkey=" + Esc(c.hotkey.Encode()) + L";";
    s += L"top=" + std::wstring(c.alwaysOnTop ? L"1" : L"0") + L";";
    s += L"seq=";
    for (size_t i = 0; i < c.seq.size(); i++) {
        if (i) s += L"|";
        swprintf_s(buf, 256, L"%d,%d,%lld", c.seq[i].x, c.seq[i].y, c.seq[i].delayUs);
        s += buf;
    }
    return s;
}

bool DecodeConfig(const std::wstring& s, Config& out) {
    std::map<std::wstring, std::wstring> kv;
    std::wstring cur;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L';') { kv[cur.substr(0, cur.find(L'='))] = cur.substr(cur.find(L'=') + 1); cur.clear(); }
        else if (s[i] == L'\\' && i + 1 < s.size()) { cur += s[i]; cur += s[++i]; }
        else cur += s[i];
    }
    if (!cur.empty()) { size_t e = cur.find(L'='); if (e != std::wstring::npos) kv[cur.substr(0, e)] = cur.substr(e + 1); }
    if (kv.empty()) return false;

    auto num = [&](const wchar_t* k, long long def) -> long long {
        auto it = kv.find(k); if (it == kv.end()) return def;
        return wcstoll(Unesc(it->second).c_str(), nullptr, 10);
    };
    Config c;
    c.intervalUs = ClampIntervalUs(num(L"interval_us", 100000));
    c.randomize  = num(L"randomize", 0) != 0;
    c.randMinUs  = num(L"rand_min_us", 50000);
    c.randMaxUs  = num(L"rand_max_us", 150000);
    c.mouseBtn   = (int)num(L"mouse_btn", 0);
    c.clickType  = (int)num(L"click_type", 0);
    c.clickMode  = (int)num(L"click_mode", 0);
    c.action     = (int)num(L"action", 0);
    c.posMode    = (int)num(L"pos_mode", 0);
    c.targetX    = (int)num(L"target_x", 0);
    c.targetY    = (int)num(L"target_y", 0);
    c.maxClicks  = (int)num(L"max_clicks", 0);
    c.clickHoldUs = num(L"hold_us", 0);
    c.startDelaySec = (int)num(L"start_delay", 3);
    auto it = kv.find(L"type_text"); if (it != kv.end()) c.typeText = Unesc(it->second);
    it = kv.find(L"hotkey");       if (it != kv.end()) c.hotkey.Decode(Unesc(it->second));
    it = kv.find(L"top");          if (it != kv.end()) c.alwaysOnTop = (Unesc(it->second) == L"1");
    it = kv.find(L"seq");
    if (it != kv.end()) {
        std::wstring list = Unesc(it->second);
        size_t pos = 0;
        while (pos <= list.size()) {
            size_t bar = list.find(L'|', pos);
            std::wstring item = list.substr(pos, bar == std::wstring::npos ? std::wstring::npos : bar - pos);
            if (!item.empty()) {
                int x = 0, y = 0; long long d = -1;
                if (swscanf_s(item.c_str(), L"%d,%d,%lld", &x, &y, &d) >= 2) {
                    SeqPoint p; p.x = x; p.y = y; p.delayUs = d;
                    c.seq.push_back(p);
                }
            }
            if (bar == std::wstring::npos) break;
            pos = bar + 1;
        }
    }
    if (c.randMaxUs < c.randMinUs) std::swap(c.randMinUs, c.randMaxUs);
    out = c;
    return true;
}

// ---------------------------------------------------------------- random
unsigned int NextRand(unsigned int& state) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}

long long RandomRange(long long lo, long long hi, unsigned int& state) {
    if (hi <= lo) return lo;
    unsigned int span = (unsigned int)(hi - lo + 1);
    return lo + (long long)(NextRand(state) % span);
}

// ---------------------------------------------------------------- engine
void Engine::ApplyConfig(const Config& c) {
    std::lock_guard<std::mutex> lk(mtx_);
    Config n = c;
    n.intervalUs = ClampIntervalUs(n.intervalUs);
    n.randMinUs = ClampIntervalUs(n.randMinUs);
    n.randMaxUs = ClampIntervalUs(n.randMaxUs);
    if (n.randMaxUs < n.randMinUs) std::swap(n.randMinUs, n.randMaxUs);
    cfg_ = n;
}

Config Engine::GetConfig() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return cfg_;
}

void Engine::SetSeed(unsigned int s) {
    std::lock_guard<std::mutex> lk(mtx_);
    rng_ = s ? s : 1u;
}

EngineStatus Engine::Status() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return status_;
}

unsigned long long Engine::ElapsedUs() const {
    if (startedAtUs_ == 0) return 0;
    unsigned long long now = clock_.NowUs();
    return now > startedAtUs_ ? now - startedAtUs_ : 0;
}

bool Engine::IsRunning() const {    std::lock_guard<std::mutex> lk(mtx_);
    return status_.state != ST_IDLE;
}

void Engine::Start() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (status_.state != ST_IDLE) return;
    stopRequested_ = false;
    firstClick_ = true;
    holdDown_ = false;
    seqIndex_ = 0;
    typeIndex_ = 0;
    status_.clicksDone = 0;
    status_.elapsedUs = 0;
    status_.intervalUs = 0;
    status_.countdownSec = 0;
    status_.emergencyStopped = false;
    if (cfg_.startDelaySec > 0) {
        status_.state = ST_COUNTDOWN;
        status_.countdownSec = cfg_.startDelaySec;
        countdownEndUs_ = clock_.NowUs() + (unsigned long long)cfg_.startDelaySec * 1000000ULL;
    } else {
        status_.state = ST_RUNNING;
        countdownEndUs_ = 0;
        startedAtUs_ = clock_.NowUs();
        nextDueUs_ = startedAtUs_;
    }
}

void Engine::Stop() {
    stopRequested_ = true;
}

bool Engine::EmergencyStop() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (status_.state == ST_IDLE || status_.emergencyStopped || stopRequested_.load()) return false;
    status_.emergencyStopped = true;
    stopRequested_ = true;
    return true;
}

bool Engine::ShouldStop() {
    if (!stopRequested_.load() && sink_.EmergencyStopRequested()) EmergencyStop();
    return stopRequested_.load();
}

void Engine::Finish() {
    if (holdDown_) {
        // release the button before going idle
        Config c = GetConfig();
        DWORD up = MOUSEEVENTF_LEFTUP;
        if (c.mouseBtn == BTN_RIGHT) up = MOUSEEVENTF_RIGHTUP;
        else if (c.mouseBtn == BTN_MIDDLE) up = MOUSEEVENTF_MIDDLEUP;
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = up;
        in.mi.dwExtraInfo = kInjectTag;
        sink_.Send(&in, 1);
        holdDown_ = false;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    status_.state = ST_IDLE;
    status_.countdownSec = 0;
    status_.elapsedUs = clock_.NowUs() - startedAtUs_;
}

long long Engine::EffectiveInterval(const Config& c) {
    if (!c.randomize) return ClampIntervalUs(c.intervalUs);
    long long lo = ClampIntervalUs(c.randMinUs), hi = ClampIntervalUs(c.randMaxUs);
    if (hi < lo) std::swap(lo, hi);
    unsigned int s;
    { std::lock_guard<std::mutex> lk(mtx_); s = rng_; }
    long long v = RandomRange(lo, hi, s);
    { std::lock_guard<std::mutex> lk(mtx_); rng_ = s; }
    return v;
}

long long Engine::DelayForPoint(const Config& c) {
    if (c.posMode == POS_SEQ && !c.seq.empty()) {
        const SeqPoint& p = c.seq[seqIndex_ % c.seq.size()];
        return p.delayUs >= 0 ? p.delayUs : ClampIntervalUs(c.intervalUs);
    }
    return EffectiveInterval(c);
}

void Engine::Tick() {
    int st;
    { std::lock_guard<std::mutex> lk(mtx_); st = status_.state; }
    if (st != ST_IDLE && ShouldStop()) { Finish(); return; }
    switch (st) {
        case ST_COUNTDOWN: TickCountdown(); break;
        case ST_RUNNING:   TickRunning();   break;
        default: break;
    }
}

void Engine::TickCountdown() {
    unsigned long long now = clock_.NowUs();
    if (now >= countdownEndUs_) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            status_.state = ST_RUNNING;
            status_.countdownSec = 0;
        }
        startedAtUs_ = clock_.NowUs();
        nextDueUs_ = startedAtUs_;
        firstClick_ = true;
        return;
    }
    unsigned long long target = countdownEndUs_;
    unsigned long long slice = now + 50000ULL;
    if (slice < target) target = slice;
    clock_.WaitUntilUs(target, &stopRequested_);
    unsigned long long cur = clock_.NowUs();
    unsigned long long rem = countdownEndUs_ > cur ? countdownEndUs_ - cur : 0;
    std::lock_guard<std::mutex> lk(mtx_);
    status_.countdownSec = (int)((rem + 999999ULL) / 1000000ULL);
}

void Engine::TickRunning() {
    Config c = GetConfig();
    if (c.clickMode == MODE_HOLD) { TickHold(); return; }

    bool useSeq = (c.posMode == POS_SEQ && !c.seq.empty());
    long long waitUs = DelayForPoint(c);

    // resolve the target for this click
    int x = 0, y = 0;
    bool usePos = false;
    if (c.posMode == POS_FIXED) {
        x = c.targetX; y = c.targetY; usePos = true;
    } else if (useSeq) {
        const SeqPoint& p = c.seq[seqIndex_ % c.seq.size()];
        x = p.x; y = p.y; usePos = true;
        seqIndex_ = (seqIndex_ + 1) % (int)c.seq.size();
    }

    // schedule
    unsigned long long now = clock_.NowUs();
    if (firstClick_) {
        if (useSeq && waitUs > 0) {
            nextDueUs_ = now + (unsigned long long)waitUs;
            clock_.WaitUntilUs(nextDueUs_, &stopRequested_);
        } else {
            nextDueUs_ = now;
        }
        firstClick_ = false;
    } else {
        nextDueUs_ += (unsigned long long)waitUs;
        now = clock_.NowUs();
        if (nextDueUs_ > now) clock_.WaitUntilUs(nextDueUs_, &stopRequested_);
        else nextDueUs_ = now;
    }

    if (ShouldStop()) { Finish(); return; }

    // act
    if (c.action == ACT_TYPE)        DoType(c);
    else if (c.action == ACT_SCROLL_UP)   DoScroll(c);
    else if (c.action == ACT_SCROLL_DOWN) DoScroll(c);
    else                              DoClick(c, x, y, usePos);

    {
        std::lock_guard<std::mutex> lk(mtx_);
        status_.clicksDone++;
        status_.intervalUs = waitUs;
        status_.elapsedUs = clock_.NowUs() - startedAtUs_;
    }

    if (c.maxClicks > 0) {
        int done;
        { std::lock_guard<std::mutex> lk(mtx_); done = status_.clicksDone; }
        if (done >= c.maxClicks) { Finish(); return; }
    }
    if (c.clickMode == MODE_SINGLE) { Finish(); return; }
    if (ShouldStop()) { Finish(); return; }
}

void Engine::TickHold() {
    Config c = GetConfig();
    if (!holdDown_) {
        int x = 0, y = 0;
        bool usePos = false;
        if (c.posMode == POS_FIXED) { x = c.targetX; y = c.targetY; usePos = true; }
        else if (c.posMode == POS_SEQ && !c.seq.empty()) {
            const SeqPoint& p = c.seq[seqIndex_ % c.seq.size()];
            x = p.x; y = p.y; usePos = true;
            seqIndex_ = (seqIndex_ + 1) % (int)c.seq.size();
        }
        DWORD down = MOUSEEVENTF_LEFTDOWN;
        if (c.mouseBtn == BTN_RIGHT) down = MOUSEEVENTF_RIGHTDOWN;
        else if (c.mouseBtn == BTN_MIDDLE) down = MOUSEEVENTF_MIDDLEDOWN;
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = down;
        in.mi.dwExtraInfo = kInjectTag;
        if (usePos) {
            sink_.MoveCursor(x, y);
#ifdef _WIN32
            int vLeft = GetSystemMetrics(SM_XVIRTUALSCREEN), vTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vW = GetSystemMetrics(SM_CXVIRTUALSCREEN), vH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            LONG nx = (LONG)(((x - vLeft) * 65535LL) / (vW > 1 ? vW - 1 : 1));
            LONG ny = (LONG)(((y - vTop) * 65535LL) / (vH > 1 ? vH - 1 : 1));
            in.mi.dx = nx; in.mi.dy = ny;
            in.mi.dwFlags = down | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
#endif
        }
        sink_.Send(&in, 1);
        holdDown_ = true;
        startedAtUs_ = clock_.NowUs();
        return;
    }

    bool release = false;
    if (c.clickHoldUs > 0 && clock_.NowUs() - startedAtUs_ >= (unsigned long long)c.clickHoldUs) release = true;
    if (ShouldStop()) release = true;

    if (!release) {
        clock_.WaitUntilUs(clock_.NowUs() + 10000ULL, &stopRequested_);
        return;
    }

    {
        DWORD up = MOUSEEVENTF_LEFTUP;
        if (c.mouseBtn == BTN_RIGHT) up = MOUSEEVENTF_RIGHTUP;
        else if (c.mouseBtn == BTN_MIDDLE) up = MOUSEEVENTF_MIDDLEUP;
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = up;
        in.mi.dwExtraInfo = kInjectTag;
        sink_.Send(&in, 1);
    }
    holdDown_ = false;
    int n;
    { std::lock_guard<std::mutex> lk(mtx_); status_.clicksDone++; n = status_.clicksDone; }
    if (c.maxClicks > 0 && n >= c.maxClicks) { Finish(); return; }
    if (c.clickMode == MODE_SINGLE) { Finish(); return; }
    if (ShouldStop()) { Finish(); return; }
    // hold for a fixed time and repeat: wait one interval before the next press
    long long waitUs = EffectiveInterval(c);
    nextDueUs_ = clock_.NowUs() + (unsigned long long)waitUs;
    clock_.WaitUntilUs(nextDueUs_, &stopRequested_);
}

void Engine::DoClick(const Config& c, int x, int y, bool usePos) {
    if (ShouldStop()) return;
    DWORD downFlag = MOUSEEVENTF_LEFTDOWN, upFlag = MOUSEEVENTF_LEFTUP;
    if (c.mouseBtn == BTN_RIGHT)      { downFlag = MOUSEEVENTF_RIGHTDOWN;  upFlag = MOUSEEVENTF_RIGHTUP; }
    else if (c.mouseBtn == BTN_MIDDLE){ downFlag = MOUSEEVENTF_MIDDLEDOWN; upFlag = MOUSEEVENTF_MIDDLEUP; }

    INPUT inputs[2] = {};
    inputs[0].type = INPUT_MOUSE; inputs[0].mi.dwExtraInfo = kInjectTag;
    inputs[1].type = INPUT_MOUSE; inputs[1].mi.dwExtraInfo = kInjectTag;

    if (usePos) {
        sink_.MoveCursor(x, y);
#ifdef _WIN32
        int vLeft = GetSystemMetrics(SM_XVIRTUALSCREEN), vTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vW = GetSystemMetrics(SM_CXVIRTUALSCREEN), vH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        LONG nx = (LONG)(((x - vLeft) * 65535LL) / (vW > 1 ? vW - 1 : 1));
        LONG ny = (LONG)(((y - vTop) * 65535LL) / (vH > 1 ? vH - 1 : 1));
        inputs[0].mi.dx = nx; inputs[0].mi.dy = ny;
        inputs[0].mi.dwFlags = downFlag | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
        inputs[1].mi.dx = nx; inputs[1].mi.dy = ny;
        inputs[1].mi.dwFlags = upFlag   | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
#else
        inputs[0].mi.dwFlags = downFlag;
        inputs[1].mi.dwFlags = upFlag;
#endif
    } else {
        inputs[0].mi.dwFlags = downFlag;
        inputs[1].mi.dwFlags = upFlag;
    }

    long long holdUs = c.clickHoldUs > 0 ? c.clickHoldUs
                    : (c.intervalUs <= 1000 ? 0 : std::min<long long>(10000, c.intervalUs / 4));

    if (holdUs <= 0) {
        sink_.Send(inputs, 2);
        if (c.clickType == CL_DOUBLE && !ShouldStop()) sink_.Send(inputs, 2);
        return;
    }
    sink_.Send(&inputs[0], 1);
    clock_.WaitUntilUs(clock_.NowUs() + (unsigned long long)holdUs, &stopRequested_);
    sink_.Send(&inputs[1], 1);
    if (c.clickType == CL_DOUBLE) {
        clock_.WaitUntilUs(clock_.NowUs() + (unsigned long long)holdUs, &stopRequested_);
        if (ShouldStop()) return;
        sink_.Send(&inputs[0], 1);
        clock_.WaitUntilUs(clock_.NowUs() + (unsigned long long)holdUs, &stopRequested_);
        sink_.Send(&inputs[1], 1);
    }
}

void Engine::DoScroll(const Config& c) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwExtraInfo = kInjectTag;
    in.mi.mouseData = (c.action == ACT_SCROLL_UP) ? (DWORD)120 : (DWORD)(short)-120;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    sink_.Send(&in, 1);
}

void Engine::DoType(const Config& c) {
    if (c.typeText.empty()) return;
    wchar_t ch = c.typeText[typeIndex_ % c.typeText.size()];
    typeIndex_++;
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wScan = (WORD)ch; in[0].ki.dwFlags = KEYEVENTF_UNICODE; in[0].ki.dwExtraInfo = kInjectTag;
    in[1] = in[0]; in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    sink_.Send(in, 2);
}

} // namespace ac
