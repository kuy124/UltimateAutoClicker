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
    std::mutex cursorMutex;
    std::vector<GdkRectangle> screens;
    int cursorX = 0, cursorY = 0;
    bool movedCursor = false;
    explicit XInput(Display* d) : display(d) {}
    bool Cursor(int& x, int& y) {
        Window root, child; int localX, localY; unsigned mask;
        return XQueryPointer(display, DefaultRootWindow(display), &root, &child,
            &x, &y, &localX, &localY, &mask);
    }
    bool EmergencyStopRequested() override {
        std::lock_guard<std::mutex> lock(cursorMutex);
        int x, y;
        if (!Cursor(x, y)) return false;
        // ponytail: X11 position polling; raw motion if movement onto an injected target must be distinguished.
        if (movedCursor && x == cursorX && y == cursorY) return false;
        movedCursor = false;
        for (const auto& r : screens)
            if (IsScreenEdge(x, y, r.x, r.y, r.x + r.width, r.y + r.height)) return true;
        return false;
    }
    void MoveCursor(int x, int y) override {
        std::lock_guard<std::mutex> lock(cursorMutex);
        XTestFakeMotionEvent(display, -1, x, y, CurrentTime);
        movedCursor = Cursor(cursorX, cursorY);
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
        *fixed, *x, *y, *random, *minimum, *maximum, *hold, *top, *start, *status, *safety;
    GtkWidget* emergencyOverlay = nullptr;
    guint effectTimer = 0;
    gint64 effectStarted = 0;
    bool emergencyShown = false, fullScreenEffect = false;
    gboolean animateEffect = TRUE;
    GKeyFile* settings = g_key_file_new();
    gchar* path = nullptr;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    bool quit = false;
    bool hotkeyDown = false;
    explicit App(Display* display) : sink(display), engine(clock, sink) {}
    void RefreshScreens() {
        GdkDisplay* desktop = gdk_display_get_default();
        std::lock_guard<std::mutex> lock(sink.cursorMutex);
        sink.screens.clear();
        for (int i = 0; i < gdk_display_get_n_monitors(desktop); ++i) {
            GdkMonitor* monitor = gdk_display_get_monitor(desktop, i);
            GdkRectangle r; gdk_monitor_get_geometry(monitor, &r);
            int scale = gdk_monitor_get_scale_factor(monitor);
            r.x *= scale; r.y *= scale; r.width *= scale; r.height *= scale;
            sink.screens.push_back(r);
        }
    }
    void ShowEmergencyEffect() {
        GdkScreen* screen = gtk_widget_get_screen(window);
        if (!emergencyOverlay) {
            emergencyOverlay = gtk_window_new(GTK_WINDOW_POPUP);
            gtk_window_set_title(GTK_WINDOW(emergencyOverlay), "EMERGENCY STOP - all clicks stopped");
            gtk_window_set_keep_above(GTK_WINDOW(emergencyOverlay), TRUE);
            gtk_window_set_accept_focus(GTK_WINDOW(emergencyOverlay), FALSE);
            gtk_window_set_focus_on_map(GTK_WINDOW(emergencyOverlay), FALSE);
            gtk_window_set_skip_taskbar_hint(GTK_WINDOW(emergencyOverlay), TRUE);
            gtk_widget_set_app_paintable(emergencyOverlay, TRUE);
            GdkVisual* visual = gdk_screen_get_rgba_visual(screen);
            fullScreenEffect = visual && gdk_screen_is_composited(screen);
            if (visual) gtk_widget_set_visual(emergencyOverlay, visual);
            auto* toast = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(toast), "<b>EMERGENCY STOP</b>\nAll clicks stopped. Use Start or F6 to restart.");
            gtk_label_set_justify(GTK_LABEL(toast), GTK_JUSTIFY_CENTER);
            gtk_label_set_line_wrap(GTK_LABEL(toast), TRUE);
            gtk_widget_set_halign(toast, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(toast, GTK_ALIGN_START);
            gtk_widget_set_margin_top(toast, fullScreenEffect ? 24 : 0);
            GtkCssProvider* css = gtk_css_provider_new();
            gtk_css_provider_load_from_data(css,
                "label { background: #211219; color: white; border: 2px solid #ff4b64;"
                " border-radius: 12px; padding: 18px 24px; font-size: 16px; }", -1, nullptr);
            gtk_style_context_add_provider(gtk_widget_get_style_context(toast), GTK_STYLE_PROVIDER(css),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(css);
            gtk_container_add(GTK_CONTAINER(emergencyOverlay), toast);
            g_signal_connect(emergencyOverlay, "draw", G_CALLBACK(+[](GtkWidget* widget, cairo_t* cr, gpointer data) -> gboolean {
                auto& a = *static_cast<App*>(data);
                cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
                cairo_set_source_rgba(cr, 0.13, 0.07, 0.10, a.fullScreenEffect ? 0 : 1);
                cairo_paint(cr); cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
                if (a.fullScreenEffect) {
                    cairo_set_source_rgba(cr, 1, 0.29, 0.39, 1);
                    cairo_set_line_width(cr, 6);
                    cairo_rectangle(cr, 3, 3, gtk_widget_get_allocated_width(widget) - 6,
                        gtk_widget_get_allocated_height(widget) - 6);
                    cairo_stroke(cr);
                }
                return FALSE;
            }), this);
            gtk_widget_realize(emergencyOverlay);
            gdk_window_set_pass_through(gtk_widget_get_window(emergencyOverlay), TRUE);
        }
        int px, py;
        gdk_device_get_position(gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default())),
            nullptr, &px, &py);
        GdkRectangle r;
        gdk_monitor_get_geometry(gdk_display_get_monitor_at_point(gdk_display_get_default(), px, py), &r);
        int width = fullScreenEffect ? r.width : std::min(460, r.width);
        int height = fullScreenEffect ? r.height : 110;
        gtk_widget_set_size_request(emergencyOverlay, width, height);
        gtk_window_resize(GTK_WINDOW(emergencyOverlay), width, height);
        gtk_window_move(GTK_WINDOW(emergencyOverlay), r.x + (r.width - width) / 2, r.y + (fullScreenEffect ? 0 : 24));
        animateEffect = TRUE;
        g_object_get(gtk_settings_get_default(), "gtk-enable-animations", &animateEffect, nullptr);
        effectStarted = g_get_monotonic_time();
        gtk_widget_set_opacity(emergencyOverlay, 0.92);
        gtk_widget_show_all(emergencyOverlay);
        if (effectTimer) g_source_remove(effectTimer);
        effectTimer = g_timeout_add(16, +[](gpointer data) -> gboolean {
            auto& a = *static_cast<App*>(data);
            gint64 elapsed = (g_get_monotonic_time() - a.effectStarted) / 1000;
            if (elapsed >= 2500) {
                gtk_widget_hide(a.emergencyOverlay); a.effectTimer = 0; return G_SOURCE_REMOVE;
            }
            if (a.animateEffect) {
                int pulse = elapsed % 800;
                if (pulse > 400) pulse = 800 - pulse;
                double opacity = (170 + pulse * 65 / 400) / 255.0;
                if (elapsed > 2000) opacity *= (2500 - elapsed) / 500.0;
                gtk_widget_set_opacity(a.emergencyOverlay, opacity);
            }
            return G_SOURCE_CONTINUE;
        }, this);
    }
    void CheckEmergency() {
        if (engine.IsRunning() && sink.EmergencyStopRequested()) { engine.EmergencyStop(); clock.Interrupt(); }
        if (engine.Status().emergencyStopped && !emergencyShown) {
            emergencyShown = true;
            gtk_label_set_markup(GTK_LABEL(safety), "<b>EMERGENCY STOP</b> — all clicks stopped.\nUse Start or F6 to restart.");
            ShowEmergencyEffect();
        }
    }
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
            emergencyShown = false;
            if (effectTimer) { g_source_remove(effectTimer); effectTimer = 0; }
            if (emergencyOverlay) gtk_widget_hide(emergencyOverlay);
            gtk_label_set_text(GTK_LABEL(safety), "Emergency stop: move the cursor to any screen edge.");
            RefreshScreens();
            { std::lock_guard<std::mutex> lock(sink.cursorMutex); sink.movedCursor = false; }
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
        gtk_window_set_title(GTK_WINDOW(window), "Ultimate AutoClicker 1.2.3 — Linux X11");
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
        safety = gtk_label_new("Emergency stop: move the cursor to any screen edge.");
        gtk_label_set_line_wrap(GTK_LABEL(safety), TRUE);
        gtk_box_pack_start(GTK_BOX(box), safety, FALSE, FALSE, 0);
        RefreshScreens();
        auto monitorChanged = +[](GdkDisplay*, GdkMonitor*, gpointer data) { static_cast<App*>(data)->RefreshScreens(); };
        g_signal_connect(gdk_display_get_default(), "monitor-added", G_CALLBACK(monitorChanged), this);
        g_signal_connect(gdk_display_get_default(), "monitor-removed", G_CALLBACK(monitorChanged), this);
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
        g_signal_handlers_disconnect_by_data(gdk_display_get_default(), this);
        if (effectTimer) { g_source_remove(effectTimer); effectTimer = 0; }
        if (emergencyOverlay) { gtk_widget_destroy(emergencyOverlay); emergencyOverlay = nullptr; }
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
    GdkRectangle bounds = app.sink.screens.front();
    int centerX = bounds.x + bounds.width / 2, centerY = bounds.y + bounds.height / 2;
    int oldX = centerX, oldY = centerY; app.sink.Cursor(oldX, oldY);
    auto move = [&](int x, int y) {
        XWarpPointer(d, None, DefaultRootWindow(d), 0, 0, 0, 0, x, y); XSync(d, False);
    };
    auto pump = [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        while (g_main_context_iteration(nullptr, FALSE)) {}
    };
    move(centerX, centerY);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.delay), 5);
    app.Toggle(); pump();
    app.sink.MoveCursor(bounds.x, centerY);
    app.CheckEmergency();
    check(app.engine.Status().state == ST_COUNTDOWN, "programmed edge moves do not emergency-stop");
    move(centerX, centerY); app.CheckEmergency();
    move(bounds.x, centerY); app.CheckEmergency(); pump();
    check(!app.engine.IsRunning() && app.engine.Status().emergencyStopped, "screen-edge movement cancels the worker countdown");
    check(app.engine.Status().clicksDone == 0, "screen-edge stop sends no clicks during countdown");
    check(app.emergencyOverlay && gtk_widget_get_visible(app.emergencyOverlay), "emergency visual is shown");
    check(gdk_window_get_pass_through(gtk_widget_get_window(app.emergencyOverlay)), "emergency visual passes clicks through");
    check(!gtk_window_get_accept_focus(GTK_WINDOW(app.emergencyOverlay)), "emergency visual cannot steal focus");
    check(std::strstr(gtk_label_get_text(GTK_LABEL(app.safety)), "EMERGENCY STOP"), "persistent emergency-stop text");
    app.effectStarted = g_get_monotonic_time() - 2600000; pump();
    check(!gtk_widget_get_visible(app.emergencyOverlay) && app.engine.Status().emergencyStopped, "visual expires but stop remains latched");
    move(centerX, centerY); app.Toggle(); pump();
    check(app.engine.IsRunning() && !app.engine.Status().emergencyStopped, "manual restart clears emergency-stop state");
    app.Toggle(); pump(); move(oldX, oldY);
    std::printf("Linux integration: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    bool selftest = argc == 2 && std::strcmp(argv[1], "--selftest") == 0;
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) { std::puts("Ultimate AutoClicker 1.2.3 (Linux X11)"); return 0; }
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
    guint timer = 0, safetyTimer = 0;
    if (selftest) result = SelfTest(app);
    else {
        safetyTimer = g_timeout_add(10, +[](gpointer data) -> gboolean {
            static_cast<App*>(data)->CheckEmergency(); return G_SOURCE_CONTINUE;
        }, &app);
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
    if (safetyTimer) g_source_remove(safetyTimer);
    app.Shutdown(); XCloseDisplay(display);
    return result;
}
