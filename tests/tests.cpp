// tests.cpp - unit + engine tests for Ultimate AutoClicker.
// Run:  test.bat
#include "core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

using namespace ac;

// ---------------------------------------------------------------- harness
static int g_checks = 0, g_fails = 0;
static const char* g_case = "";

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_fails++; \
    std::printf("  FAIL  %s:%d  [%s]  %s\n", __FILE__, __LINE__, g_case, #cond); } } while (0)

#define CHECK_EQ(a, b) do { g_checks++; long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { g_fails++; std::printf("  FAIL  %s:%d  [%s]  %s == %s  (%lld != %lld)\n", \
    __FILE__, __LINE__, g_case, #a, #b, _a, _b); } } while (0)

#define CHECK_STR(a, b) do { g_checks++; std::wstring _a = (a), _b = (b); \
    if (_a != _b) { g_fails++; std::printf("  FAIL  %s:%d  [%s]  \"%ls\" != \"%ls\"\n", \
    __FILE__, __LINE__, g_case, _a.c_str(), _b.c_str()); } } while (0)

static void Case(const char* name) { g_case = name; std::printf("- %s\n", name); }

// ---------------------------------------------------------------- fakes
struct FakeClock : Clock {
    unsigned long long now = 0;
    Engine* stopOnWait = nullptr;
    int waitsBeforeStop = 0;
    unsigned long long NowUs() override { return now; }
    bool WaitUntilUs(unsigned long long target, const std::atomic<bool>* cancel) override {
        if (target > now) now = target;
        if (stopOnWait && --waitsBeforeStop == 0) stopOnWait->EmergencyStop();
        return !(cancel && cancel->load());
    }
};

struct FakeEvent {
    unsigned long long t = 0;
    WORD type = 0;
    DWORD flags = 0;
    int dx = 0, dy = 0;
    DWORD data = 0;
    WORD scan = 0;
};

struct FakeSink : InputSink {
    std::vector<FakeEvent> events;
    std::vector<std::pair<int, int>> moves;
    unsigned long long* clock = nullptr;
    bool emergency = false;
    bool EmergencyStopRequested() override { return emergency; }

    void Send(const INPUT* in, int count) override {
        for (int i = 0; i < count; i++) {
            FakeEvent e;
            e.t = clock ? *clock : 0;
            e.type = in[i].type;
            if (in[i].type == INPUT_MOUSE) {
                e.flags = in[i].mi.dwFlags;
                e.dx = in[i].mi.dx;
                e.dy = in[i].mi.dy;
                e.data = in[i].mi.mouseData;
            } else {
                e.flags = in[i].ki.dwFlags;
                e.scan = in[i].ki.wScan;
            }
            events.push_back(e);
        }
    }
    void MoveCursor(int x, int y) override { moves.push_back({ x, y }); }
};

// ---------------------------------------------------------------- helpers
static void RunUntilIdle(Engine& e, int maxIters = 200000) {
    for (int i = 0; i < maxIters; i++) {
        if (e.Status().state == ST_IDLE) return;
        e.Tick();
    }
}

static Config BaseConfig() {
    Config c;
    c.intervalUs = 10000;          // 10 ms
    c.startDelaySec = 0;
    c.randomize = false;
    c.clickMode = MODE_REPEAT;
    c.action = ACT_CLICK;
    c.posMode = POS_CURRENT;
    c.mouseBtn = BTN_LEFT;
    c.clickType = CL_SINGLE;
    return c;
}

static int CountDowns(const FakeSink& s) {
    int n = 0;
    for (size_t i = 0; i < s.events.size(); i++)
        if (s.events[i].type == INPUT_MOUSE && (s.events[i].flags & (MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_RIGHTDOWN | MOUSEEVENTF_MIDDLEDOWN)))
            n++;
    return n;
}

static int CountUps(const FakeSink& s) {
    int n = 0;
    for (size_t i = 0; i < s.events.size(); i++)
        if (s.events[i].type == INPUT_MOUSE && (s.events[i].flags & (MOUSEEVENTF_LEFTUP | MOUSEEVENTF_RIGHTUP | MOUSEEVENTF_MIDDLEUP)))
            n++;
    return n;
}

// ---------------------------------------------------------------- tests
static void TestIntervalMath() {
    Case("interval math");
    CHECK_EQ(FieldsToUs(0, 0, 0, 0, 0), 0);
    CHECK_EQ(FieldsToUs(0, 0, 0, 0, 500), 500);
    CHECK_EQ(FieldsToUs(0, 0, 0, 100, 0), 100000);
    CHECK_EQ(FieldsToUs(0, 0, 1, 0, 0), 1000000);
    CHECK_EQ(FieldsToUs(0, 1, 0, 0, 0), 60000000);
    CHECK_EQ(FieldsToUs(1, 0, 0, 0, 0), 3600000000LL);
    CHECK_EQ(FieldsToUs(0, 0, 0.5, 0, 0), 500000);
    CHECK_EQ(FieldsToUs(-1, 0, 0, 0, 0), 0);          // negative -> 0

    double h, m, s, ms, us;
    UsToFields(3600000000LL + 60000000LL + 1000000LL + 1000LL + 5LL, h, m, s, ms, us);
    CHECK_EQ((long long)h, 1);
    CHECK_EQ((long long)m, 1);
    CHECK_EQ((long long)s, 1);
    CHECK_EQ((long long)ms, 1);
    CHECK_EQ((long long)us, 5);
    CHECK_EQ(FieldsToUs(h, m, s, ms, us), 3600000000LL + 60000000LL + 1000000LL + 1000LL + 5LL);

    CHECK_EQ(ClampIntervalUs(0), kMinIntervalUs);
    CHECK_EQ(ClampIntervalUs(-5000), kMinIntervalUs);
    CHECK_EQ(ClampIntervalUs(1234), 1234);
    CHECK_EQ(ClampIntervalUs(kMaxIntervalUs + 1), kMaxIntervalUs);
}

static void TestCombo() {
    Case("hotkey combo");
    Combo c;
    c.mods = MOD_CONTROL | MOD_SHIFT;
    c.keys = { 'Q' };
    CHECK_STR(c.Name(), L"Ctrl + Shift + Q");
    CHECK_STR(c.Encode(), L"6:81");
    Combo d;
    CHECK(d.Decode(c.Encode()));
    CHECK(d == c);

    Combo xz;
    xz.keys = { 'X', 'Z' };
    CHECK_STR(xz.Name(), L"X + Z");
    Combo dz;
    CHECK(dz.Decode(xz.Encode()));
    CHECK(dz == xz);

    // matching
    std::vector<int> pressed = { 'X', 'Z' };
    CHECK(ComboMatches(xz, pressed, 0));
    CHECK(!ComboMatches(xz, { 'X' }, 0));                 // missing a key
    CHECK(!ComboMatches(xz, { 'X', 'Z', 'Y' }, 0));         // extra key
    CHECK(!ComboMatches(xz, pressed, MOD_CONTROL));         // wrong modifiers
    Combo cq;
    cq.mods = MOD_CONTROL; cq.keys = { 'C' };
    CHECK(ComboMatches(cq, { 'C' }, MOD_CONTROL));
    CHECK(!ComboMatches(cq, { 'C' }, 0));
    CHECK(!ComboMatches(cq, { 'C' }, MOD_CONTROL | MOD_SHIFT));
    CHECK(!ComboMatches(Combo(), {}, 0));                   // empty combo never matches
}

static void TestConfigCodec() {
    Case("config encode/decode");
    Config c = BaseConfig();
    c.intervalUs = 123456;
    c.randomize = true;
    c.randMinUs = 1000;
    c.randMaxUs = 2000;
    c.mouseBtn = BTN_MIDDLE;
    c.clickType = CL_DOUBLE;
    c.clickMode = MODE_SINGLE;
    c.action = ACT_TYPE;
    c.posMode = POS_SEQ;
    c.targetX = 11; c.targetY = 22;
    c.maxClicks = 99;
    c.clickHoldUs = 777;
    c.startDelaySec = 5;
    c.typeText = L"a=b;c\\d\ne";
    c.alwaysOnTop = true;
    c.hotkey.mods = MOD_ALT; c.hotkey.keys = { 'F', '6' };
    SeqPoint p1; p1.x = 5;  p1.y = 6;  p1.delayUs = 7000;
    SeqPoint p2; p2.x = -3; p2.y = 900; p2.delayUs = -1;
    c.seq = { p1, p2 };

    std::wstring blob = EncodeConfig(c);
    Config o;
    CHECK(DecodeConfig(blob, o));
    CHECK_EQ(o.intervalUs, c.intervalUs);
    CHECK_EQ(o.randomize, 1);
    CHECK_EQ(o.randMinUs, c.randMinUs);
    CHECK_EQ(o.randMaxUs, c.randMaxUs);
    CHECK_EQ(o.mouseBtn, c.mouseBtn);
    CHECK_EQ(o.clickType, c.clickType);
    CHECK_EQ(o.clickMode, c.clickMode);
    CHECK_EQ(o.action, c.action);
    CHECK_EQ(o.posMode, c.posMode);
    CHECK_EQ(o.targetX, c.targetX);
    CHECK_EQ(o.targetY, c.targetY);
    CHECK_EQ(o.maxClicks, c.maxClicks);
    CHECK_EQ(o.clickHoldUs, c.clickHoldUs);
    CHECK_EQ(o.startDelaySec, c.startDelaySec);
    CHECK_STR(o.typeText, c.typeText);
    CHECK_EQ(o.alwaysOnTop, 1);
    CHECK(o.hotkey == c.hotkey);
    CHECK_EQ((long long)o.seq.size(), 2);
    CHECK_EQ(o.seq[0].x, 5);  CHECK_EQ(o.seq[0].y, 6);  CHECK_EQ(o.seq[0].delayUs, 7000);
    CHECK_EQ(o.seq[1].x, -3); CHECK_EQ(o.seq[1].y, 900); CHECK_EQ(o.seq[1].delayUs, -1);

    // garbage in -> false, no crash
    Config junk;
    CHECK(!DecodeConfig(L"", junk));
    CHECK(!DecodeConfig(L"garbage-without-equals", junk));
}

static void TestStartCountdown() {
    Case("start countdown");
    FakeClock clock; FakeSink sink; sink.clock = &clock.now;
    Engine e(clock, sink);
    Config c = BaseConfig();
    c.startDelaySec = 3;
    e.ApplyConfig(c);

    e.Start();
    CHECK_EQ(e.Status().state, ST_COUNTDOWN);
    CHECK_EQ(e.Status().countdownSec, 3);
    CHECK(!e.IsRunning() ? false : true);   // countdown counts as busy

    // tick until the countdown is over
    int guard = 0;
    while (e.Status().state == ST_COUNTDOWN && guard++ < 10000) e.Tick();
    CHECK_EQ(e.Status().state, ST_RUNNING);
    CHECK(clock.now >= 3000000ULL);          // it really waited 3 seconds
    CHECK_EQ(CountDowns(sink), 0);           // and did not click during the countdown

    // stop during a countdown
    FakeClock c2; FakeSink s2; s2.clock = &c2.now;
    Engine e2(c2, s2);
    e2.ApplyConfig(c);
    e2.Start();
    e2.Tick();
    e2.Stop();
    e2.Tick();
    CHECK_EQ(e2.Status().state, ST_IDLE);
    CHECK_EQ(CountDowns(s2), 0);
}

static void TestRepeatLimitNoRestart() {
    Case("repeat N times stops (regression: endless restart)");
    FakeClock clock; FakeSink sink; sink.clock = &clock.now;
    Engine e(clock, sink);
    Config c = BaseConfig();
    c.maxClicks = 5;
    e.ApplyConfig(c);

    e.Start();
    RunUntilIdle(e);
    CHECK_EQ(e.Status().state, ST_IDLE);
    CHECK_EQ(e.Status().clicksDone, 5);
    CHECK_EQ(CountDowns(sink), 5);
    CHECK_EQ(CountUps(sink), 5);

    // keep ticking: it must stay stopped, it must not restart itself
    for (int i = 0; i < 50; i++) e.Tick();
    CHECK_EQ(e.Status().state, ST_IDLE);
    CHECK_EQ(e.Status().clicksDone, 5);
    CHECK_EQ(CountDowns(sink), 5);
}

static void TestSingleMode() {
    Case("single click mode");
    FakeClock clock; FakeSink sink; sink.clock = &clock.now;
    Engine e(clock, sink);
    Config c = BaseConfig();
    c.clickMode = MODE_SINGLE;
    e.ApplyConfig(c);
    e.Start();
    RunUntilIdle(e);
    CHECK_EQ(e.Status().state, ST_IDLE);
    CHECK_EQ(e.Status().clicksDone, 1);
    CHECK_EQ(CountDowns(sink), 1);
}

static void TestIntervalScheduling() {
    Case("interval scheduling (no drift)");
    FakeClock clock; FakeSink sink; sink.clock = &clock.now;
    Engine e(clock, sink);
    Config c = BaseConfig();
    c.intervalUs = 10000;
    c.maxClicks = 5;
    e.ApplyConfig(c);
    e.Start();
    RunUntilIdle(e);

    std::vector<unsigned long long> downs;
    for (size_t i = 0; i < sink.events.size(); i++)
        if (sink.events[i].flags & MOUSEEVENTF_LEFTDOWN) downs.push_back(sink.events[i].t);

    CHECK_EQ((long long)downs.size(), 5);
    if (downs.size() == 5) {
        CHECK_EQ((long long)downs[0], 0);
        CHECK_EQ((long long)downs[1], 10000);
        CHECK_EQ((long long)downs[2], 20000);
        CHECK_EQ((long long)downs[3], 30000);
        CHECK_EQ((long long)downs[4], 40000);
    }
}

static void TestZeroIntervalClamp() {
    Case("zero interval is clamped (regression: spin lock)");
    FakeClock clock; FakeSink sink; sink.clock = &clock.now;
    Engine e(clock, sink);
    Config c = BaseConfig();
    c.intervalUs = 0;
    c.maxClicks = 3;
    e.ApplyConfig(c);
    CHECK_EQ(e.GetConfig().intervalUs, kMinIntervalUs);

    e.Start();
    RunUntilIdle(e);
    CHECK_EQ(e.Status().clicksDone, 3);

    std::vector<unsigned long long> downs;
    for (size_t i = 0; i < sink.events.size(); i++)
        if (sink.events[i].flags & MOUSEEVENTF_LEFTDOWN) downs.push_back(sink.events[i].t);
    CHECK_EQ((long long)downs.size(), 3);
    if (downs.size() == 3) {
        CHECK_EQ((long long)downs[1], kMinIntervalUs);
        CHECK_EQ((long long)downs[2], 2 * kMinIntervalUs);
    }
}

static void TestRandomInterval() {
    Case("randomised interval stays in range");
    FakeClock clock; FakeSink sink; sink.clock = &clock.now;
    Engine e(clock, sink);
    Config c = BaseConfig();
    c.randomize = true;
    c.randMinUs = 5000;
    c.randMaxUs = 9000;
    c.maxClicks = 40;
    e.ApplyConfig(c);
    e.SetSeed(12345);
    e.Start();
    RunUntilIdle(e);
    CHECK_EQ(e.Status().clicksDone, 40);

    std::vector<unsigned long long> downs;
    for (size_t i = 0; i < sink.events.size(); i++)
        if (sink.events[i].flags & MOUSEEVENTF_LEFTDOWN) downs.push_back(sink.events[i].t);

    bool inRange = true, sawDifferent = false;
    for (size_t i = 1; i < downs.size(); i++) {
        long long gap = (long long)(downs[i] - downs[i - 1]);
        if (gap < 5000 || gap > 9000 + 2500) inRange = false;   // 2500 = hold time inside the click
        if (gap != (long long)(downs[1] - downs[0])) sawDifferent = true;
    }
    CHECK(inRange);
    CHECK(sawDifferent);
}

static void TestDoubleClick() {
    Case("double click emits down/up twice");
    FakeClock clock; FakeSink sink; sink.clock = &clock.now;
    Engine e(clock, sink);
    Config c = BaseConfig();
    c.clickType = CL_DOUBLE;
    c.maxClicks = 1;
    e.ApplyConfig(c);
    e.Start();
    RunUntilIdle(e);
    CHECK_EQ(CountDowns(sink), 2);
    CHECK_EQ(CountUps(sink), 2);
    CHECK_EQ((long long)sink.events.size(), 4);
}

static void TestHoldMode() {
    Case("hold mode presses and releases");
    FakeClock clock; FakeSink sink; sink.clock = &clock.now;
    Engine e(clock, sink);
    Config c = BaseConfig();
    c.clickMode = MODE_HOLD;
    c.clickHoldUs = 50000;      // 50 ms
    c.maxClicks = 1;
    e.ApplyConfig(c);
    e.Start();

    e.Tick();                   // press
    CHECK_EQ(CountDowns(sink), 1);
    CHECK_EQ(CountUps(sink), 0);
    CHECK_EQ(e.Status().state, ST_RUNNING);

    for (int i = 0; i < 3; i++) e.Tick();   // 30 ms, still held
    CHECK_EQ(CountUps(sink), 0);

    for (int i = 0; i < 10 && e.Status().state == ST_RUNNING; i++) e.Tick();
    CHECK_EQ(CountUps(sink), 1);
    CHECK_EQ(e.Status().state, ST_IDLE);
    CHECK_EQ(e.Status().clicksDone, 1);
}

static void TestHoldReleasedOnStop() {
    Case("hold mode releases the button on stop");
    FakeClock clock; FakeSink sink; sink.clock = &clock.now;
    Engine e(clock, sink);
    Config c = BaseConfig();
    c.clickMode = MODE_HOLD;
    c.clickHoldUs = 0;          // hold until stopped
    e.ApplyConfig(c);
    e.Start();
    e.Tick();
    CHECK_EQ(CountDowns(sink), 1);
    CHECK_EQ(CountUps(sink), 0);
    e.Stop();
    e.Tick();
    CHECK_EQ(CountUps(sink), 1);            // released
    CHECK_EQ(e.Status().state, ST_IDLE);
}

static void TestSequenceMode() {
    Case("sequence cycles through the points");
    FakeClock clock; FakeSink sink; sink.clock = &clock.now;
    Engine e(clock, sink);
    Config c = BaseConfig();
    c.posMode = POS_SEQ;
    SeqPoint a; a.x = 10; a.y = 20; a.delayUs = 10000;
    SeqPoint b; b.x = 30; b.y = 40; b.delayUs = 20000;
    SeqPoint d; d.x = 50; d.y = 60; d.delayUs = 30000;
    c.seq = { a, b, d };
    c.maxClicks = 4;
    e.ApplyConfig(c);
    e.Start();
    RunUntilIdle(e);
    CHECK_EQ(e.Status().clicksDone, 4);

    // click times honour the per point delays
    std::vector<unsigned long long> downs;
    for (size_t i = 0; i < sink.events.size(); i++)
        if (sink.events[i].flags & MOUSEEVENTF_LEFTDOWN) downs.push_back(sink.events[i].t);
    CHECK_EQ((long long)downs.size(), 4);
    if (downs.size() == 4) {
        CHECK_EQ((long long)downs[0], 10000);
        CHECK_EQ((long long)downs[1], 30000);
        CHECK_EQ((long long)downs[2], 60000);
        CHECK_EQ((long long)downs[3], 70000);
    }

    // and the coordinates rotate 10,20 -> 30,40 -> 50,60 -> 10,20
    CHECK_EQ((long long)sink.moves.size(), 4);
    if (sink.moves.size() == 4) {
        CHECK_EQ(sink.moves[0].first, 10); CHECK_EQ(sink.moves[0].second, 20);
        CHECK_EQ(sink.moves[1].first, 30); CHECK_EQ(sink.moves[1].second, 40);
        CHECK_EQ(sink.moves[2].first, 50); CHECK_EQ(sink.moves[2].second, 60);
        CHECK_EQ(sink.moves[3].first, 10); CHECK_EQ(sink.moves[3].second, 20);
    }
}

static void TestFixedPosition() {
    Case("fixed position moves and clicks there");
    FakeClock clock; FakeSink sink; sink.clock = &clock.now;
    Engine e(clock, sink);
    Config c = BaseConfig();
    c.posMode = POS_FIXED;
    c.targetX = 123; c.targetY = 456;
    c.maxClicks = 1;
    e.ApplyConfig(c);
    e.Start();
    RunUntilIdle(e);
    CHECK_EQ((long long)sink.moves.size(), 1);
    if (!sink.moves.empty()) {
        CHECK_EQ(sink.moves[0].first, 123);
        CHECK_EQ(sink.moves[0].second, 456);
    }
    CHECK_EQ((long long)sink.events.size(), 2);
    if (sink.events.size() == 2) {
#ifdef _WIN32
        CHECK(sink.events[0].flags & MOUSEEVENTF_ABSOLUTE);
        CHECK(sink.events[0].flags & MOUSEEVENTF_VIRTUALDESK);
        CHECK(sink.events[0].flags & MOUSEEVENTF_MOVE);
#else
        CHECK(sink.events[0].flags & MOUSEEVENTF_LEFTDOWN);
        CHECK(!(sink.events[0].flags & MOUSEEVENTF_MOVE));
#endif
        CHECK(sink.events[1].flags & MOUSEEVENTF_LEFTUP);
    }
}

static void TestScrollAction() {
    Case("scroll action");
    FakeClock clock; FakeSink sink; sink.clock = &clock.now;
    Engine e(clock, sink);
    Config c = BaseConfig();
    c.action = ACT_SCROLL_UP;
    c.maxClicks = 2;
    e.ApplyConfig(c);
    e.Start();
    RunUntilIdle(e);
    CHECK_EQ((long long)sink.events.size(), 2);
    CHECK_EQ((long long)sink.events[0].data, 120);
    CHECK(sink.events[0].flags & MOUSEEVENTF_WHEEL);

    FakeClock c2; FakeSink s2; s2.clock = &c2.now;
    Engine e2(c2, s2);
    Config c3 = BaseConfig();
    c3.action = ACT_SCROLL_DOWN;
    c3.maxClicks = 1;
    e2.ApplyConfig(c3);
    e2.Start();
    RunUntilIdle(e2);
    CHECK_EQ((long long)s2.events.size(), 1);
    CHECK_EQ((long long)(short)s2.events[0].data, -120);
}

static void TestTypeAction() {
    Case("auto type action");
    FakeClock clock; FakeSink sink; sink.clock = &clock.now;
    Engine e(clock, sink);
    Config c = BaseConfig();
    c.action = ACT_TYPE;
    c.typeText = L"ab";
    c.maxClicks = 4;
    e.ApplyConfig(c);
    e.Start();
    RunUntilIdle(e);
    CHECK_EQ((long long)sink.events.size(), 8);       // 2 events (down+up) per character
    CHECK_EQ((long long)sink.events[0].scan, (long long)L'a');
    CHECK_EQ((long long)sink.events[2].scan, (long long)L'b');
    CHECK_EQ((long long)sink.events[4].scan, (long long)L'a');
    CHECK(sink.events[0].flags & KEYEVENTF_UNICODE);
    CHECK(sink.events[1].flags & KEYEVENTF_KEYUP);
}

static void TestMouseButtons() {
    Case("mouse button selection");
    for (int btn = 0; btn < 3; btn++) {
        FakeClock clock; FakeSink sink; sink.clock = &clock.now;
        Engine e(clock, sink);
        Config c = BaseConfig();
        c.mouseBtn = btn;
        c.maxClicks = 1;
        e.ApplyConfig(c);
        e.Start();
        RunUntilIdle(e);
        CHECK_EQ((long long)sink.events.size(), 2);
        if (sink.events.size() == 2) {
            DWORD want = btn == BTN_LEFT ? MOUSEEVENTF_LEFTDOWN
                        : btn == BTN_RIGHT ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_MIDDLEDOWN;
            CHECK(sink.events[0].flags & want);
        }
    }
}

static void TestStopIsImmediate() {
    Case("stop interrupts the wait");
    FakeClock clock; FakeSink sink; sink.clock = &clock.now;
    Engine e(clock, sink);
    Config c = BaseConfig();
    c.intervalUs = 10000000;      // 10 s
    c.maxClicks = 0;
    e.ApplyConfig(c);
    e.Start();
    e.Tick();                      // first click
    CHECK_EQ(e.Status().state, ST_RUNNING);
    e.Stop();
    e.Tick();                      // the long wait must abort
    CHECK_EQ(e.Status().state, ST_IDLE);
    CHECK_EQ(e.Status().clicksDone, 1);
}

static void TestRestartAfterStop() {
    Case("can restart after a stop");
    FakeClock clock; FakeSink sink; sink.clock = &clock.now;
    Engine e(clock, sink);
    Config c = BaseConfig();
    c.maxClicks = 2;
    e.ApplyConfig(c);

    e.Start(); RunUntilIdle(e);
    CHECK_EQ(e.Status().clicksDone, 2);

    e.Start();
    CHECK_EQ(e.Status().state, ST_RUNNING);
    RunUntilIdle(e);
    CHECK_EQ(e.Status().clicksDone, 2);
    CHECK_EQ(CountDowns(sink), 4);
}

static void TestEmergencyStop() {
    Case("screen-edge emergency stop");
    const int screens[][4] = {{0, 0, 1920, 1080}, {-1280, -100, 0, 924}};
    for (const auto& r : screens) {
        int x = (r[0] + r[2]) / 2, y = (r[1] + r[3]) / 2;
        CHECK(IsScreenEdge(r[0], y, r[0], r[1], r[2], r[3]));
        CHECK(IsScreenEdge(r[2] - 1, y, r[0], r[1], r[2], r[3]));
        CHECK(IsScreenEdge(x, r[1], r[0], r[1], r[2], r[3]));
        CHECK(IsScreenEdge(x, r[3] - 1, r[0], r[1], r[2], r[3]));
        CHECK(IsScreenEdge(r[0] + kEmergencyEdgePx - 1, y, r[0], r[1], r[2], r[3]));
        CHECK(!IsScreenEdge(r[0] + kEmergencyEdgePx, y, r[0], r[1], r[2], r[3]));
        CHECK(!IsScreenEdge(x, y, r[0], r[1], r[2], r[3]));
        CHECK(!IsScreenEdge(r[2], y, r[0], r[1], r[2], r[3]));
    }
    for (int mode : {MODE_REPEAT, MODE_HOLD, MODE_SINGLE}) {
        FakeClock clock; FakeSink sink;
        Engine e(clock, sink);
        Config c = BaseConfig(); c.clickMode = mode; c.clickHoldUs = 0;
        e.ApplyConfig(c); e.Start();
        if (mode != MODE_SINGLE) e.Tick();
        int downs = CountDowns(sink);
        sink.emergency = true; e.Tick();
        CHECK(!e.IsRunning());
        CHECK(e.Status().emergencyStopped);
        CHECK_EQ(CountDowns(sink), downs);
        CHECK_EQ(CountUps(sink), downs);
        e.Tick(); CHECK_EQ(CountDowns(sink), downs);
        sink.emergency = false; e.Start();
        CHECK(!e.Status().emergencyStopped);
        e.Stop(); e.Tick();
    }
    {
        FakeClock clock; FakeSink sink; Engine e(clock, sink);
        Config c = BaseConfig(); c.startDelaySec = 30;
        e.ApplyConfig(c); e.Start();
        CHECK(e.EmergencyStop()); CHECK(!e.EmergencyStop());
        e.Tick(); CHECK(!e.IsRunning()); CHECK(sink.events.empty());
        CHECK(e.Status().emergencyStopped);
    }
    for (int wait : {1, 2}) {
        FakeClock clock; FakeSink sink; Engine e(clock, sink);
        Config c = BaseConfig(); c.clickType = CL_DOUBLE; c.clickHoldUs = 10000;
        e.ApplyConfig(c); e.Start();
        clock.stopOnWait = &e; clock.waitsBeforeStop = wait;
        e.Tick(); e.Tick();
        CHECK(!e.IsRunning()); CHECK(e.Status().emergencyStopped);
        CHECK_EQ(CountDowns(sink), 1); CHECK_EQ(CountUps(sink), 1);
    }
}

int main() {
    std::printf("Ultimate AutoClicker - test suite\n");
    std::printf("===============================\n");

    TestIntervalMath();
    TestCombo();
    TestConfigCodec();
    TestStartCountdown();
    TestRepeatLimitNoRestart();
    TestSingleMode();
    TestIntervalScheduling();
    TestZeroIntervalClamp();
    TestRandomInterval();
    TestDoubleClick();
    TestHoldMode();
    TestHoldReleasedOnStop();
    TestSequenceMode();
    TestFixedPosition();
    TestScrollAction();
    TestTypeAction();
    TestMouseButtons();
    TestStopIsImmediate();
    TestRestartAfterStop();
    TestEmergencyStop();

    std::printf("===============================\n");
    std::printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
