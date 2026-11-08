#include "core.h"
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#undef Status
#include <chrono>
#include <thread>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <algorithm>

using namespace ac;

struct LinuxClock : Clock {
    std::mutex mutex;
    std::condition_variable wake;
    void Interrupt() { std::lock_guard<std::mutex> lock(mutex); wake.notify_all(); }
    unsigned long long NowUs() override {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    bool WaitUntilUs(unsigned long long target, const std::atomic<bool>* cancel) override {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_until(lock, std::chrono::steady_clock::time_point(std::chrono::microseconds(target)),
                        [&] { return cancel && cancel->load(); });
        return !(cancel && cancel->load());
    }
};

struct XInput : InputSink {
    Display* display;
    explicit XInput(Display* d) : display(d) {}
    void MoveCursor(int x, int y) override {
        XTestFakeMotionEvent(display, -1, x, y, CurrentTime);
        XFlush(display);
    }
    void Send(const INPUT* inputs, int count) override {
        for (int i = 0; i < count; ++i) {
            const auto& in = inputs[i];
            if (in.type == INPUT_KEYBOARD) {
                // ponytail: use the active X keyboard map; arbitrary Unicode needs a clipboard/IME backend.
                KeySym symbol = in.ki.wScan < 256 ? in.ki.wScan : 0x01000000 | in.ki.wScan;
                KeyCode code = XKeysymToKeycode(display, symbol);
                if (code) XTestFakeKeyEvent(display, code, !(in.ki.dwFlags & KEYEVENTF_KEYUP), CurrentTime);
                continue;
            }
            const DWORD flags[] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP,
                MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP};
            const unsigned buttons[] = {1, 1, 3, 3, 2, 2};
            for (int j = 0; j < 6; ++j)
                if (in.mi.dwFlags & flags[j]) XTestFakeButtonEvent(display, buttons[j], j % 2 == 0, CurrentTime);
            if (in.mi.dwFlags & MOUSEEVENTF_WHEEL) {
                unsigned button = static_cast<int32_t>(in.mi.mouseData) > 0 ? 4 : 5;
                XTestFakeButtonEvent(display, button, True, CurrentTime);
                XTestFakeButtonEvent(display, button, False, CurrentTime);
            }
        }
        XFlush(display);
    }
};

struct App {
    LinuxClock clock;
    XInput sink;
    Engine engine;
    GtkWidget *window, *interval, *button, *kind, *mode, *action, *limit, *delay,
        *fixed, *x, *y, *random, *minimum, *maximum, *hold, *top, *start, *status;
    GKeyFile* settings = g_key_file_new();
    gchar* path = nullptr;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    bool quit = false;
    bool hotkeyDown = false;
    explicit App(Display* display) : sink(display), engine(clock, sink) {}
    Config Read() {
        Config c;
        c.intervalUs = gtk_spin_button_get_value(GTK_SPIN_BUTTON(interval));
        c.mouseBtn = gtk_combo_box_get_active(GTK_COMBO_BOX(button));
        c.clickType = gtk_combo_box_get_active(GTK_COMBO_BOX(kind));
        c.clickMode = gtk_combo_box_get_active(GTK_COMBO_BOX(mode));
        c.action = gtk_combo_box_get_active(GTK_COMBO_BOX(action));
        c.maxClicks = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(limit));
        c.startDelaySec = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(delay));
        c.posMode = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fixed)) ? POS_FIXED : POS_CURRENT;
        c.targetX = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(x));
        c.targetY = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(y));
        c.randomize = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(random));
        c.randMinUs = gtk_spin_button_get_value(GTK_SPIN_BUTTON(minimum));
        c.randMaxUs = gtk_spin_button_get_value(GTK_SPIN_BUTTON(maximum));
        c.clickHoldUs = gtk_spin_button_get_value(GTK_SPIN_BUTTON(hold));
        c.alwaysOnTop = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(top));
        return c;
    }
    void Toggle() {
        if (engine.IsRunning()) { engine.Stop(); clock.Interrupt(); }
        else {
            std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
            if (!lock.owns_lock()) return;
            engine.ApplyConfig(Read()); engine.Start(); wake.notify_one();
        }
    }
    void Save() {
        Config c = Read();
        std::wstring encoded = EncodeConfig(c);
        gchar* utf8 = g_ucs4_to_utf8(reinterpret_cast<const gunichar*>(encoded.c_str()), -1, nullptr, nullptr, nullptr);
        g_key_file_set_string(settings, "Settings", "config", utf8);
        g_free(utf8);
        gchar* directory = g_path_get_dirname(path);
        if (g_mkdir_with_parents(directory, 0700) != 0) g_warning("Cannot create settings directory: %s", directory);
        else {
            GError* error = nullptr;
            if (!g_key_file_save_to_file(settings, path, &error)) {
                g_warning("Cannot save settings: %s", error->message);
                g_error_free(error);
            }
        }
        g_free(directory);
    }
    void Build() {
        path = g_build_filename(g_get_user_config_dir(), "ultimate-autoclicker", "settings.ini", nullptr);
        Config c;
        if (g_key_file_load_from_file(settings, path, G_KEY_FILE_NONE, nullptr)) {
            gchar* text = g_key_file_get_string(settings, "Settings", "config", nullptr);
            if (text) {
                gunichar* wide = g_utf8_to_ucs4(text, -1, nullptr, nullptr, nullptr);
                if (wide) DecodeConfig(reinterpret_cast<wchar_t*>(wide), c);
                g_free(wide); g_free(text);
            }
        }
        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window), "Ultimate AutoClicker 1.2.2 — Linux X11");
        gtk_window_set_default_size(GTK_WINDOW(window), 460, 620);
        gtk_container_set_border_width(GTK_CONTAINER(window), 16);
        auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_container_add(GTK_CONTAINER(window), box);
        auto* notice = gtk_label_new("X11 desktop • F6 starts/stops globally\nNative Wayland applications are not supported.");
        gtk_box_pack_start(GTK_BOX(box), notice, FALSE, FALSE, 0);
        auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
        auto* grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
        gtk_container_add(GTK_CONTAINER(scroll), grid);
        int row = 0;
        auto field = [&](const char* label, GtkWidget* widget) {
            auto* text = gtk_label_new_with_mnemonic(label);
            gtk_label_set_mnemonic_widget(GTK_LABEL(text), widget);
            gtk_widget_set_halign(text, GTK_ALIGN_START);
            gtk_grid_attach(GTK_GRID(grid), text, 0, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), widget, 1, row++, 1, 1);
            gtk_widget_set_hexpand(widget, TRUE);
            return widget;
        };
        auto spin = [&](const char* label, double value, double lo, double hi) {
            auto* widget = gtk_spin_button_new_with_range(lo, hi, 1);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), value);
            return field(label, widget);
        };
        auto combo = [&](const char* label, const char* const* choices, int value, int count) {
            auto* widget = gtk_combo_box_text_new();
            for (int i = 0; i < count; ++i) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), choices[i]);
            gtk_combo_box_set_active(GTK_COMBO_BOX(widget), std::max(0, std::min(value, count - 1)));
            return field(label, widget);
        };
        auto check = [&](const char* label, bool value) {
            auto* widget = gtk_check_button_new();
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), value);
            return field(label, widget);
        };
        const char* buttons[] = {"Left", "Right", "Middle"};
        const char* kinds[] = {"Single", "Double"};
        const char* modes[] = {"Repeat", "Hold", "One click"};
        const char* actions[] = {"Click", "Scroll up", "Scroll down"};
        interval = spin("_Interval (µs)", c.intervalUs, kMinIntervalUs, kMaxIntervalUs);
        button = combo("Mouse _button", buttons, c.mouseBtn, 3);
        kind = combo("Click _type", kinds, c.clickType, 2);
        mode = combo("_Mode", modes, c.clickMode, 3);
        action = combo("_Action", actions, c.action, 3);
        limit = spin("Click limit (0 = infinite)", c.maxClicks, 0, 2147483647);
        delay = spin("Start delay (seconds)", c.startDelaySec, 0, 86400);
        hold = spin("Hold duration (µs, 0 = auto)", c.clickHoldUs, 0, kMaxIntervalUs);
        fixed = check("Fixed position", c.posMode == POS_FIXED);
        x = spin("Target X", c.targetX, -100000, 100000);
        y = spin("Target Y", c.targetY, -100000, 100000);
        random = check("Random interval", c.randomize);
        minimum = spin("Minimum interval (µs)", c.randMinUs, kMinIntervalUs, kMaxIntervalUs);
        maximum = spin("Maximum interval (µs)", c.randMaxUs, kMinIntervalUs, kMaxIntervalUs);
        top = check("Always on top", c.alwaysOnTop);
        gtk_window_set_keep_above(GTK_WINDOW(window), c.alwaysOnTop);
        g_signal_connect(top, "toggled", G_CALLBACK(+[](GtkToggleButton* toggle, gpointer data) {
            auto& a = *static_cast<App*>(data);
            gtk_window_set_keep_above(GTK_WINDOW(a.window), gtk_toggle_button_get_active(toggle));
        }), this);
        start = gtk_button_new_with_mnemonic("_Start / Stop (F6)");
        gtk_box_pack_start(GTK_BOX(box), start, FALSE, FALSE, 0);
        status = gtk_label_new("Idle • Clicks: 0");
        gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);
        g_signal_connect(start, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<App*>(data)->Toggle(); }), this);
        g_signal_connect(window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
            static_cast<App*>(data)->Save(); return FALSE;
        }), this);
        g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) { gtk_main_quit(); }), nullptr);
        worker = std::thread([this] {
            std::unique_lock<std::mutex> lock(mutex);
            while (!quit) {
                wake.wait(lock, [this] { return quit || engine.IsRunning(); });
                if (quit) break;
                engine.Tick();
            }
        });
        gtk_widget_show_all(window);
    }
    void Shutdown() {
        engine.Stop(); clock.Interrupt();
        { std::lock_guard<std::mutex> lock(mutex); quit = true; }
        wake.notify_one();
        if (worker.joinable()) worker.join();
        // Finish releases any button held when the window was closed.
        engine.Tick();
        g_key_file_unref(settings); g_free(path);
    }
};

static int SelfTest(App& app) {
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); } };
    check(gtk_widget_get_visible(app.window), "GUI visible");
    check(app.Read().intervalUs >= kMinIntervalUs, "valid GUI defaults");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.interval), 1234);
    check(app.Read().intervalUs == 1234, "GUI interval binding");
    app.Save();
    check(g_file_test(app.path, G_FILE_TEST_IS_REGULAR), "settings saved");
    GKeyFile* saved = g_key_file_new();
    check(g_key_file_load_from_file(saved, app.path, G_KEY_FILE_NONE, nullptr), "settings reload");
    gchar* encoded = g_key_file_get_string(saved, "Settings", "config", nullptr);
    gunichar* wide = encoded ? g_utf8_to_ucs4(encoded, -1, nullptr, nullptr, nullptr) : nullptr;
    Config restored;
    check(wide && DecodeConfig(reinterpret_cast<wchar_t*>(wide), restored) && restored.intervalUs == 1234, "settings round trip");
    g_free(wide); g_free(encoded); g_key_file_unref(saved);
    Display* d = app.sink.display;
    Window target = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 200, 200, 0, 0, 0);
    XSelectInput(d, target, ButtonPressMask | ButtonReleaseMask);
    XMapRaised(d, target); XSync(d, False);
    Config c; c.startDelaySec = 0; c.intervalUs = 100; c.maxClicks = 3;
    c.posMode = POS_FIXED; c.targetX = 50; c.targetY = 50;
    LinuxClock clock; XInput sink(d); Engine engine(clock, sink);
    auto run = [&](Config config) {
        engine.ApplyConfig(config); engine.Start();
        for (int i = 0; i < 100 && engine.IsRunning(); ++i) engine.Tick();
        XSync(d, False);
    };
    auto events = [&](unsigned button, int expected) {
        int downs = 0, ups = 0;
        while (XPending(d)) {
            XEvent event; XNextEvent(d, &event);
            if (event.type == ButtonPress && event.xbutton.button == button) ++downs;
            if (event.type == ButtonRelease && event.xbutton.button == button) ++ups;
        }
        check(downs == expected && ups == expected, "real XTest press/release pairs");
    };
    for (int i = 0; i < 3; ++i) {
        c.mouseBtn = i; run(c); events(i == 0 ? 1 : i == 1 ? 3 : 2, 3);
        check(engine.Status().clicksDone == 3 && !engine.IsRunning(), "repeat limit");
    }
    c.mouseBtn = BTN_LEFT; c.clickType = CL_DOUBLE; run(c); events(1, 6);
    c.clickType = CL_SINGLE; c.action = ACT_SCROLL_UP; run(c); events(4, 3);
    c.action = ACT_SCROLL_DOWN; run(c); events(5, 3);
    c.action = ACT_CLICK; c.clickMode = MODE_HOLD; c.clickHoldUs = 1000; c.maxClicks = 1;
    run(c); events(1, 1);
    c.clickHoldUs = 0; engine.ApplyConfig(c); engine.Start(); engine.Tick(); engine.Stop(); engine.Tick();
    XSync(d, False); events(1, 1);
    check(!engine.IsRunning(), "stop releases held button");
    XDestroyWindow(d, target);
    std::atomic<bool> cancel{false};
    auto start = clock.NowUs();
    std::thread stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); cancel = true; clock.Interrupt(); });
    check(!clock.WaitUntilUs(start + 5000000, &cancel), "clock cancellation");
    stopper.join();
    check(clock.NowUs() - start < 500000, "cancellation latency below 500ms");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.delay), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.limit), 2);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.fixed), FALSE);
    app.Toggle();
    for (int i = 0; i < 100 && app.engine.IsRunning(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(!app.engine.IsRunning() && app.engine.Status().clicksDone == 2, "GUI starts worker and repeat limit stops it");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.delay), 30);
    app.Toggle();
    check(app.engine.IsRunning(), "GUI countdown starts");
    app.Toggle();
    for (int i = 0; i < 100 && app.engine.IsRunning(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(!app.engine.IsRunning(), "GUI stop cancels worker countdown");
    std::printf("Linux integration: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    bool selftest = argc == 2 && std::strcmp(argv[1], "--selftest") == 0;
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) { std::puts("Ultimate AutoClicker 1.2.2 (Linux X11)"); return 0; }
    if (!selftest && g_strcmp0(g_getenv("XDG_SESSION_TYPE"), "wayland") == 0) {
        std::fputs("Native Wayland automation is unsupported. Log into an X11/Xorg session.\n", stderr); return 1;
    }
    XInitThreads();
    g_setenv("GDK_BACKEND", "x11", TRUE);
    if (!gtk_init_check(&argc, &argv)) { std::fputs("Cannot open X11 display. Select an X11 desktop session.\n", stderr); return 1; }
    Display* display = XOpenDisplay(nullptr);
    int event, error, major, minor;
    if (!display || !XTestQueryExtension(display, &event, &error, &major, &minor)) {
        std::fputs("X11 XTEST extension is required.\n", stderr);
        if (display) XCloseDisplay(display);
        return 1;
    }
    App app(display); app.Build();
    int result = 0;
    guint timer = 0;
    if (selftest) result = SelfTest(app);
    else {
        timer = g_timeout_add(50, +[](gpointer data) -> gboolean {
            auto& a = *static_cast<App*>(data);
            char keys[32]; XQueryKeymap(a.sink.display, keys);
            KeyCode f6 = XKeysymToKeycode(a.sink.display, XK_F6);
            bool down = f6 && (keys[f6 / 8] & (1 << (f6 % 8)));
            if (down && !a.hotkeyDown) a.Toggle();
            a.hotkeyDown = down;
            auto s = a.engine.Status();
            gchar* text = g_strdup_printf("%s • Clicks: %d", s.state == ST_IDLE ? "Idle" : s.state == ST_COUNTDOWN ? "Countdown" : "Running", s.clicksDone);
            gtk_label_set_text(GTK_LABEL(a.status), text); g_free(text);
            return G_SOURCE_CONTINUE;
        }, &app);
        gtk_main();
    }
    if (timer) g_source_remove(timer);
    app.Shutdown(); XCloseDisplay(display);
    return result;
}
