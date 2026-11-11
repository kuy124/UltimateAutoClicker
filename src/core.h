// core.h - engine + pure logic for Ultimate AutoClicker.
// No GUI code here on purpose: everything in this header is unit-testable.

// MinGW does not define UNICODE by default. Without it DefWindowProc() resolves
// to the ANSI variant, which mangles wide strings (window titles become "U").
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include "linux_types.h"
#endif
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
namespace ac {

constexpr int kEmergencyEdgePx = 5;
bool IsScreenEdge(int x, int y, int left, int top, int right, int bottom);

// ---------------------------------------------------------------- limits
constexpr long long   kMinIntervalUs = 100;                            // 0.1 ms floor (anti spin-lock)
constexpr long long   kMaxIntervalUs = 24LL * 3600LL * 1000000LL;      // 24 h
constexpr unsigned long long kInjectTag = 0xAC10CULL;                  // dwExtraInfo marker for injected input

// ---------------------------------------------------------------- modes
enum { MODE_REPEAT = 0, MODE_HOLD = 1, MODE_SINGLE = 2 };
enum { ACT_CLICK = 0, ACT_SCROLL_UP = 1, ACT_SCROLL_DOWN = 2, ACT_TYPE = 3 };
enum { POS_CURRENT = 0, POS_FIXED = 1, POS_SEQ = 2 };
enum { BTN_LEFT = 0, BTN_RIGHT = 1, BTN_MIDDLE = 2 };
enum { CL_SINGLE = 0, CL_DOUBLE = 1 };

// ---------------------------------------------------------------- hotkey
// A combo is any set of up to 3 non-modifier keys plus a modifier mask.
struct Combo {
    BYTE mods = 0;                 // MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN
    std::vector<int> keys;         // non-modifier virtual key codes

    bool empty() const { return keys.empty(); }
    bool operator==(const Combo& o) const { return mods == o.mods && keys == o.keys; }
    std::wstring Name() const;                 // "Ctrl + Shift + Q"
    std::wstring Encode() const;               // "MOD:88,80"
    bool Decode(const std::wstring& s);
};

// True when the currently pressed keys (non-modifier) and held modifiers
// exactly match the combo.
bool ComboMatches(const Combo& c, const std::vector<int>& pressed, BYTE heldMods);

// ---------------------------------------------------------------- config
struct SeqPoint {
    int x = 0, y = 0;
    long long delayUs = -1;        // -1 = use the global interval; >=0 = explicit
};

struct Config {
    long long intervalUs = 100000;
    bool      randomize = false;
    long long randMinUs = 50000, randMaxUs = 150000;
    int       mouseBtn = BTN_LEFT;
    int       clickType = CL_SINGLE;
    int       clickMode = MODE_REPEAT;
    int       action = ACT_CLICK;
    int       posMode = POS_CURRENT;
    int       targetX = 0, targetY = 0;
    int       maxClicks = 0;               // 0 = infinite
    long long clickHoldUs = 0;             // 0 = auto (derived from the interval)
    int       startDelaySec = 3;
    std::wstring typeText = L"Hello";
    std::vector<SeqPoint> seq;
    Combo     hotkey;
    bool      alwaysOnTop = false;
};

// Interval helpers (pure).
long long FieldsToUs(double h, double m, double s, double ms, double us);
void     UsToFields(long long totalUs, double& h, double& m, double& s, double& ms, double& us);
long long ClampIntervalUs(long long us);

// Config <-> string, used for profiles and for the on-disk settings blob.
std::wstring EncodeConfig(const Config& c);
bool        DecodeConfig(const std::wstring& s, Config& out);

// ---------------------------------------------------------------- engine
struct Clock {
    virtual ~Clock() {}
    virtual unsigned long long NowUs() = 0;
    // Waits until targetUs. Returns false when interrupted via `cancel`.
    virtual bool WaitUntilUs(unsigned long long targetUs, const std::atomic<bool>* cancel) = 0;
};

struct InputSink {
    virtual ~InputSink() {}
    virtual void Send(const INPUT* in, int count) = 0;
    virtual void MoveCursor(int x, int y) = 0;
    virtual bool EmergencyStopRequested() { return false; }
};

enum { ST_IDLE = 0, ST_COUNTDOWN = 1, ST_RUNNING = 2 };

struct EngineStatus {
    int state = ST_IDLE;
    int clicksDone = 0;
    unsigned long long elapsedUs = 0;
    long long intervalUs = 0;      // effective interval used by the last click
    int countdownSec = 0;          // whole seconds left in the start countdown
    bool emergencyStopped = false; // stays visible until explicitly restarted
};

class Engine {
public:
    Engine(Clock& clock, InputSink& sink) : clock_(clock), sink_(sink) {}

    void ApplyConfig(const Config& c);
    Config GetConfig() const;

    void Start();                   // begin the start countdown (no-op if busy)
    void Stop();                    // request an immediate stop (thread safe)
    bool EmergencyStop();           // stop and latch the reason (thread safe)
    void Tick();                    // performs one step of work
    EngineStatus Status() const;
    unsigned long long ElapsedUs() const;
    bool IsRunning() const;
    void SetSeed(unsigned int s);

private:
    void TickCountdown();
    bool ShouldStop();
    void TickRunning();
    void TickHold();
    void DoClick(const Config& c, int x, int y, bool usePos);
    void DoScroll(const Config& c);
    void DoType(const Config& c);
    void Finish();
    long long EffectiveInterval(const Config& c);
    long long DelayForPoint(const Config& c);

    Clock& clock_;
    InputSink& sink_;

    mutable std::mutex mtx_;
    Config cfg_;
    EngineStatus status_;
    std::atomic<bool> stopRequested_{false};

    unsigned long long countdownEndUs_ = 0;
    unsigned long long startedAtUs_ = 0;
    unsigned long long nextDueUs_ = 0;
    bool firstClick_ = true;
    bool holdDown_ = false;
    int  seqIndex_ = 0;
    int  typeIndex_ = 0;
    unsigned int rng_ = 0x1234567u;
};

// Random helpers (seeded, deterministic for tests).
unsigned int NextRand(unsigned int& state);
long long RandomRange(long long lo, long long hi, unsigned int& state);

} // namespace ac
