// A Switch 2's graphics for a Switch game that draws with OpenGL ES. The game's own driver (in its SDK modules) talks to the Switch's
// GPU through the nvdrv service, which a Switch 2 refuses a homebrew program (sm: result 1015; log, 2026-11-22), and whose GPU is
// another. But such a game reaches the driver through very little: EGL's entry points, imported by name, and every GL function
// asked of eglGetProcAddress. So those imports are bound to the host's EGL (ounce-devkit's, over Vulkan) instead of the SDK's, and
// the SDK functions that start the driver and make its window do nothing.
// The host's functions run with the host library's thread-local data in place of the SDK's (svc_entry.s, nx2_host_call).
#ifdef __SWITCH2__
#include <switch.h>

#include <EGL/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx2.h"

#ifndef NX2_NAME
#define NX2_NAME "game"
#endif
#define HOST_MAX 1024
extern const u32 nx2_host_pool[];
void *nx2_host_targets[HOST_MAX];
u64 nx2_tls_game[32], nx2_tls_host[32];
static u32 host_count;
static int res_w = 1920, res_h = 1080;  // the size of the picture: the window's, and what the game is told (the menu's setting)

static void font_load(void);
void nx2_gl_config(void) {
    res_h = nx2_menu_load(), res_w = res_h * 16 / 9;
    font_load();
    if (nx2_debug) {
        // (What the host's OpenGL and its Vulkan driver say of their own troubles goes to a file, <folder>/stderr.txt: they say it
        // to the error stream, which a homebrew program has nowhere to show. Asked of a game, whose pictures stop
        // with the driver saying that its work for the GPU failed and nothing of why; 2026-11-23. loader.c's watcher has the file
        // put on the card every few seconds.)
        if (freopen(NX2_F("stderr.txt"), "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
        setenv("MESA_DEBUG", "1", 1);
        setenv("MESA_LOG_LEVEL", "debug", 1);
        fprintf(stderr, "nx2: the host's own messages follow\n");
    }
    nx2_log("[gl] the picture: %ux%u (<folder>/settings.cfg, or the menu: Plus and Minus held five seconds)\n", (u64)res_w, (u64)res_h);
}

// the upper half of the main thread's thread-local region, as the host library has it: taken before the SDK makes it its own
void nx2_host_capture(void) {
    u8 *tls;
    __asm__("mrs %0, tpidrro_el0" : "=r"(tls));
    memcpy(nx2_tls_host, tls + 0x100, sizeof nx2_tls_host);
}

// The kernel's handle of the thread that is running, for the host's half of its thread-local region (svc_entry.s), or 0 when it
// isn't known. The SDK keeps it in its record of the thread (nn::os::GetCurrentThread), where isn't looked up: the record is
// searched for one of the handles the kernel gave the game's threads (svc.c keeps them) or the main thread's, once per thread.
u32 nx2_main_handle;
u32 nx2_thread_handle(void) {
    extern u32 nx2_threads[96];
    static void *(*current)(void);
    static struct {
        const void *record;
        u32 handle;
    } known[128];
    static u32 known_count;
    if (!nx2_threads[0]) return 0;  // (the game hasn't made a thread yet: the SDK may not be ready to be asked)
    if (!current) current = (void *(*)(void))nx2_symbol("_ZN2nn2os16GetCurrentThreadEv");
    const u32 *record = current ? current() : NULL;
    if (!record) return 0;
    const u32 count = __atomic_load_n(&known_count, __ATOMIC_ACQUIRE);
    for (u32 k = 0; k < count && k < 128; k++)
        if (known[k].record == record) return known[k].handle;
    u32 handle = 0;
    for (u32 i = 0; i < 0x1C0 / 4 && !handle; i++) {
        const u32 v = record[i];
        if (!v) continue;
        if (v == nx2_main_handle) handle = v;
        for (u32 k = 0; k < 96 && !handle; k++)
            if (nx2_threads[k] == v) handle = v;
    }
    const u32 at = __atomic_fetch_add(&known_count, 1, __ATOMIC_ACQ_REL);
    if (at < 128) known[at].handle = handle, known[at].record = record;
    else __atomic_store_n(&known_count, 128, __ATOMIC_RELEASE);
    return handle;
}

// what the game calls to reach a host function
static void *through_host(void *target) {
    for (u32 k = 0; k < host_count; k++)
        if (nx2_host_targets[k] == target) return (void *)(nx2_host_pool + 2 * k);
    if (host_count >= HOST_MAX) return NULL;
    nx2_host_targets[host_count] = target;
    return (void *)(nx2_host_pool + 2 * host_count++);
}

// ---------------------------------------------------------------- EGL (these run as the host: called through nx2_host_call)
static u32 proc_logged;
static void *h_get_proc(const char *name) {
    void *f = (void *)eglGetProcAddress(name);
    void *g = f ? through_host(f) : NULL;
    if (!g || proc_logged++ < 12) nx2_log("[gl] %s: %s\n", name, !f ? "the host's OpenGL doesn't have it" : g ? "bound" : "no room left");
    return g;
}
static EGLDisplay h_get_display(EGLNativeDisplayType id) {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    nx2_log("[gl] eglGetDisplay(%p): %p (error %x)\n", (u64)id, (u64)d, (u64)eglGetError());
    return d;
}
static EGLBoolean h_initialize(EGLDisplay d, EGLint *major, EGLint *minor) {
    EGLint a = 0, b = 0;
    const EGLBoolean ok = eglInitialize(d, &a, &b);
    nx2_log("[gl] eglInitialize: %u, EGL %u.%u (error %x)\n", (u64)ok, (u64)a, (u64)b, (u64)eglGetError());
    if (major) *major = a;
    if (minor) *minor = b;
    return ok;
}
static EGLBoolean h_choose_config(EGLDisplay d, const EGLint *attribs, EGLConfig *configs, EGLint size, EGLint *count) {
    for (u32 k = 0; attribs && attribs[k] != EGL_NONE && k < 40; k += 2) nx2_log("[gl]   config asked: %x = %x\n", (u64)(u32)attribs[k], (u64)(u32)attribs[k + 1]);
    const EGLBoolean ok = eglChooseConfig(d, attribs, configs, size, count);
    nx2_log("[gl] eglChooseConfig: %u, %d configs (error %x)\n", (u64)ok, (s64)(count ? *count : -1), (u64)eglGetError());
    return ok;
}
static EGLContext h_create_context(EGLDisplay d, EGLConfig config, EGLContext share, const EGLint *attribs) {
    for (u32 k = 0; attribs && attribs[k] != EGL_NONE && k < 20; k += 2) nx2_log("[gl]   context asked: %x = %x\n", (u64)(u32)attribs[k], (u64)(u32)attribs[k + 1]);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLContext c = eglCreateContext(d, config, share, attribs);
    nx2_log("[gl] eglCreateContext: %p (error %x)\n", (u64)c, (u64)eglGetError());
    return c;
}
static EGLSurface h_create_surface(EGLDisplay d, EGLConfig config, EGLNativeWindowType window, const EGLint *attribs) {
    // the window at the size the game is told the screen has (below): as it comes, the two differed and a corner of the picture
    // filled the screen (seen on the console, 2026-11-22)
    const Result sized = nwindowSetDimensions(nwindowGetDefault(), (u32)res_w, (u32)res_h);
    u32 ww = 0, wh = 0;
    nwindowGetDimensions(nwindowGetDefault(), &ww, &wh);
    nx2_log("[gl] the window: %ux%u asked (%x), %ux%u now\n", (u64)res_w, (u64)res_h, (u64)sized, (u64)ww, (u64)wh);
    EGLSurface s = eglCreateWindowSurface(d, config, nwindowGetDefault(), attribs);
    nx2_log("[gl] eglCreateWindowSurface (window %p): %p (error %x)\n", (u64)window, (u64)s, (u64)eglGetError());
    return s;
}
static EGLBoolean h_make_current(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext c) {
    const EGLBoolean ok = eglMakeCurrent(d, draw, read, c);
    nx2_log("[gl] eglMakeCurrent: %u (error %x)\n", (u64)ok, (u64)eglGetError());
    return ok;
}
static u32 swaps;
static EGLBoolean h_swap(EGLDisplay d, EGLSurface s) {
    const EGLBoolean ok = eglSwapBuffers(d, s);
    if (swaps < 3 || !ok || swaps % 600 == 0) nx2_log("[gl] eglSwapBuffers %u: %u\n", (u64)swaps, (u64)ok);
    swaps++;
    return ok;
}
static EGLBoolean h_destroy_surface(EGLDisplay d, EGLSurface s) { return eglDestroySurface(d, s); }

// ---------------------------------------------------------------- the SDK's own (these run as the game: no host library in them)
// The screen, as the game is told: the size of the window above. (A game picks its size by one of these.)
static void display_resolution(int *w, int *h) { *w = res_w, *h = res_h; }
static int operation_mode(void) { return res_h > 720; }    // 1: docked
static int performance_mode(void) { return res_h > 720; }  // 1: boost

// The controllers' buttons, as the game reads them: seen here for the menu's sake.
static u64 pad_seen;  // every button down in a state read since the last picture
#define PAD(n) \
    static void (*real_pad##n)(u64 *, const u32 *); \
    static void pad##n(u64 *state, const u32 *id) { \
        real_pad##n(state, id); \
        pad_seen |= state[1]; \
    }
PAD(0) PAD(1) PAD(2) PAD(3) PAD(4)
enum { B_A = 1 << 0, B_B = 1 << 1, B_PLUS = 1 << 10, B_MINUS = 1 << 11, B_LEFT = 1 << 12 | 1 << 16, B_UP = 1 << 13 | 1 << 17,
       B_RIGHT = 1 << 14 | 1 << 18, B_DOWN = 1 << 15 | 1 << 19 };

// the buttons now, asked of the SDK as the game does (player 1 in each style it has, and the handheld)
static u64 pad_now(void) {
    u64 state[16], all = 0;
    static const u32 first = 0, handheld = 0x20;
    void (*const fn[5])(u64 *, const u32 *) = {real_pad0, real_pad1, real_pad2, real_pad3, real_pad4};
    for (int k = 0; k < 5; k++) {
        if (!fn[k]) continue;
        state[1] = 0;
        fn[k](state, k == 3 ? &handheld : &first);
        all |= state[1];
    }
    return all;
}

// The menu: the game waits here, in its eglSwapBuffers, while the menu's own pictures are shown.
static void menu(EGLDisplay d, EGLSurface s) {
    void (*const begin)(void) = (void (*)(void))through_host(nx2_menu_begin);
    void (*const end)(void) = (void (*)(void))through_host(nx2_menu_end);
    void (*const draw)(const Nx2Menu *) = (void (*)(const Nx2Menu *))through_host(nx2_menu_draw);
    void (*const save)(int) = (void (*)(int))through_host(nx2_menu_save);
    void (*const set_debug)(int) = (void (*)(int))through_host(nx2_menu_debug);
    EGLBoolean (*const swap)(EGLDisplay, EGLSurface) = (EGLBoolean (*)(EGLDisplay, EGLSurface))through_host(eglSwapBuffers);
    if (!begin || !end || !draw || !save || !swap || !set_debug) return;
    static Nx2Menu m;
    m.screen_w = res_w, m.screen_h = res_h, m.current_h = res_h, m.selected = 0, m.debug = nx2_debug;
    if (!m.res_h) m.res_h = res_h;
    begin();
    u64 before = ~0ull;  // (what is down as the menu opens isn't a press)
    for (;;) {
        const u64 now = pad_now(), press = now & ~before;
        before = now;
        if (press & B_UP) m.selected = (m.selected + 3) % 4;
        if (press & B_DOWN) m.selected = (m.selected + 1) % 4;
        if (m.selected == 2 && (press & (B_LEFT | B_RIGHT | B_A))) {
            m.debug = !m.debug;
            set_debug(m.debug);
        }
        if (m.selected == 1 && (press & (B_LEFT | B_RIGHT | B_A))) {
            m.res_h = m.res_h == 720 ? 1080 : 720;
            save(m.res_h);
        }
        if ((press & B_B) || (m.selected == 0 && (press & B_A))) break;
        if (m.selected == 3 && (press & B_A)) {
            nx2_log("[menu] the game is closed from the menu\n");
            ((void (*)(void))through_host(nx2_menu_quit))();
            nx2_exit();
        }
        draw(&m);
        swap(d, s);
    }
    end();
}

// eglSwapBuffers, as the game calls it: Plus and Minus held five seconds open the menu
static EGLBoolean (*host_swap)(EGLDisplay, EGLSurface);
static EGLBoolean g_swap(EGLDisplay d, EGLSurface s) {
    static u64 since;
    static int opened;
    const u64 seen = pad_seen, now = armGetSystemTick();
    pad_seen = 0;
    if ((seen & (B_PLUS | B_MINUS)) == (B_PLUS | B_MINUS)) {
        if (!since) since = now;
        else if (!opened && armTicksToNs(now - since) >= 5000000000ull) opened = 1, menu(d, s);
    } else {
        since = 0, opened = 0;
    }
    return host_swap(d, s);
}

// The screen's refresh, as an event a game may wait on (nn::vi::GetDisplayVsyncEvent): the SDK's own would ask the display service
// about a display that is nothing of its making, and a game was ended there (a Switch 2, 2026-11-23). The game is
// given an event the SDK makes for it, of the kind that stays within the program, and it is signalled when a picture has been
// shown (the host's swap returns at a refresh): once at the start too, so that a game which waits before its first picture goes on.
// (Not an event of the kernel's, signalled by a thread of the port's: a homebrew program isn't let make one on a Switch 2. The
// four games built with that ended before their first line of log was on the card; 2026-11-23.)
static void *vsync_event;
static u64 vsync_last;  // when it was last signalled (the system's ticks)
static void (*vsync_signal)(void *);
// A thread of the game's kind (the SDK's own: made and started by its functions) that does nothing but sleep four thousandths of
// a second at a time. The refresh event is signalled when one of the game's threads is about to sleep (nx2_vsync_tick, below), and
// a game whose threads all wait, for the refresh among other things, has none that sleeps: a game showed three
// pictures and then stood still, every thread of it waiting (a Switch 2, 2026-11-23). This one always sleeps.
static void (*ticker_sleep)(s64);
static void ticker(void *arg) {
    (void)arg;
    for (;;) ticker_sleep(4000000);
}
static void ticker_start(void) {
    static int started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
    u32 (*create)(void *, void (*)(void *), void *, void *, u64, int) =
        (u32 (*)(void *, void (*)(void *), void *, void *, u64, int))nx2_symbol("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi");
    void (*start)(void *) = (void (*)(void *))nx2_symbol("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    void *(*aligned)(u64, u64) = (void *(*)(u64, u64))nx2_symbol("aligned_alloc");
    ticker_sleep = (void (*)(s64))nx2_symbol("_ZN2nn2os11SleepThreadENS_8TimeSpanE");
    // (the thread's record and its stack are the game's memory: the SDK maps a stack where the kernel has stacks, and that is
    // done with memory of a heap)
    u8 *room = create && start && aligned && ticker_sleep ? aligned(0x1000, 0x12000) : NULL;
    u32 rc = ~0u;
    if (room) {
        memset(room, 0, 0x12000);
        rc = create(room + 0x10000, ticker, NULL, room, 0x10000, 16);
        if (!rc) start(room + 0x10000);
    }
    nx2_log("[gl] a thread that only sleeps, for the refresh event to be signalled by: %s (%x)\n", !room ? "not to be had" : rc ? "the SDK refused it" : "started",
            (u64)rc);
}
static u32 vi_vsync(void *event, void *display) {
    (void)display;
    void (*create)(void *, int, int) = (void (*)(void *, int, int))nx2_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    vsync_signal = (void (*)(void *))nx2_symbol("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    nx2_log("[gl] the game asks for the screen's refresh event: %s\n", create && vsync_signal ? "given one of the SDK's own making" : "the SDK has no way to make one");
    if (create && vsync_signal) {
        create(event, 1, 0);  // (cleared by the wait that sees it; within this program)
        vsync_signal(event);
        vsync_event = event;
        ticker_start();
    }
    return 0;
}

// (for nvn2.c: a host function as the game may call it, and the end of a picture, the menu's check with it)
void *nx2_through_host(void *target) { return through_host(target); }
u32 nx2_present(void *d, void *s) {
    if (!host_swap) host_swap = (EGLBoolean (*)(EGLDisplay, EGLSurface))through_host(h_swap);
    const u32 ok = g_swap((EGLDisplay)d, (EGLSurface)s);
    if (vsync_event) {
        __atomic_store_n(&vsync_last, armGetSystemTick(), __ATOMIC_RELAXED);
        vsync_signal(vsync_event);
    }
    return ok;
}
// The refresh event again, when a sixtieth of a second has gone by with none: a game that waits for a refresh before it shows its
// first picture would wait for ever on one signalled only by pictures shown. A game did: every thread of it idle, and
// no picture asked for (a Switch 2, 2026-11-23). Called by svc.c when a thread of the game's is about to sleep: the signal has to
// be made by one of the game's threads (the SDK's event is not the kernel's), and one that sleeps holds nothing of the event's.
void nx2_vsync_tick(void) {
    static int busy;
    if (!vsync_event) return;
    const u64 now = armGetSystemTick(), last = __atomic_load_n(&vsync_last, __ATOMIC_RELAXED);
    if (now - last < armGetSystemTickFreq() / 60 || __atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&vsync_last, now, __ATOMIC_RELAXED);
    vsync_signal(vsync_event);
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

// The system's font, which a game asks the SDK for (nn::pl) to draw its text with. On a Switch 2 the service behind it answers a
// homebrew program with an error, and the SDK ends the game (A game, result 3e80aa; 2026-11-23). So the game is
// given a font of the port's: a TrueType file at <folder>/font.ttf, which tools/install.py puts there (a free one that comes with
// devkitPro's examples). It isn't the console's, so the letters look different, and it has no Japanese: enough for a game to run.
// Without the file the SDK's own functions are left in place.
static void *font_data;
static u64 font_size;
static void font_load(void) {  // (as the host)
    FILE *f = fopen(NX2_F("font.ttf"), "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        font_data = n > 0 ? malloc((size_t)n) : NULL;
        if (font_data && fread(font_data, 1, (size_t)n, f) == (size_t)n) font_size = (u64)n;
        fclose(f);
    }
    nx2_log("[gl] the font games are given in place of the system's (<folder>/font.ttf): %s, %u bytes\n", font_size ? "there" : "not there", font_size);
}
static u32 font_state(u32 type) {
    (void)type;
    return 1;  // loaded
}
static u64 font_bytes(u32 type) {
    (void)type;
    return font_size;
}
static void *font_address(u32 type) {
    (void)type;
    return font_data;
}
// nn::pl::GetSharedFont: the fonts a language is to be drawn with, in order. One is given, the port's. (Each entry is 24 bytes:
// which font, where it is and how long; a game reads the last two, main+41e67c. The call behind it is refused as the
// others are, result 3e80aa; log, 2026-11-23.)
static void font_list(int *count, u64 *fonts, int room, u64 language) {
    (void)language;
    if (room > 0 && fonts) fonts[0] = 0, fonts[1] = (u64)font_data, fonts[2] = font_size;
    if (count) *count = room > 0 ? 1 : 0;
}

static void nothing(void) {}
static u32 nothing_ok(void) { return 0; }
static u32 hdr_none(u8 *on, u32 *format, u16 *nits, const void *display) {
    (void)display;
    *on = 0, *format = 0, *nits = 0;
    return 0;
}
// (A try: the header of a file of the game's, asked for its first block. A game has ended many times right after its
// first such call from main+898040: said here before it can, if the header or the block isn't memory that can be read.)
static const u8 *(*real_first_block)(const u8 *);
static int can_read(u64 at, u64 size) {
    MemoryInfo mi;
    u32 pi;
    if (at < 0x10000 || R_FAILED(svcQueryMemory(&mi, &pi, at)) || !(mi.perm & Perm_R)) return 0;
    return at + size <= mi.addr + mi.size || can_read(mi.addr + mi.size, 1);
}
static const u8 *first_block_watched(const u8 *header) {
    static u32 calls, said;
    const u32 k = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    const u64 *frame = __builtin_frame_address(0);
    const char *m = "?";
    u64 o = frame[1];
    nx2_where(frame[1], &m, &o);
    const int ok = can_read((u64)header, 0x20);
    if (!ok || (o - 4 == 0x898040 && said < 4)) {
        if (o - 4 == 0x898040) said++;
        nx2_log("[file] GetFirstBlock, call %u, from %s+%x: the header at %p %s\n", (u64)k, m, o - 4, (u64)header, ok ? "can be read" : "CAN'T BE READ");
        if (ok)
            nx2_log("[file]   it begins %x %x %x %x\n", *(const u64 *)header, *(const u64 *)(header + 8), *(const u64 *)(header + 16), *(const u64 *)(header + 24));
    }
    const u8 *block = ok ? real_first_block(header) : NULL;
    if (ok && !can_read((u64)block, 0x20))
        nx2_log("[file] GetFirstBlock, call %u, from %s+%x: the block it gives, %p, CAN'T BE READ (header at %p: %x %x %x)\n", (u64)k, m, o - 4, (u64)block,
                (u64)header, *(const u64 *)header, *(const u64 *)(header + 8), *(const u64 *)(header + 16));
    return ok ? block : real_first_block(header);
}
static void asleep_for_good(void) {
    nx2_log("[os] the thread that starts the amiibo library is put to sleep for good\n");
    for (;;) svcSleepThread(1000000000ull);
}
static void process_name(const char **name, u64 *length) {
    static const char mine[] = NX2_NAME;
    *name = mine, *length = sizeof mine - 1;
}
static u64 dummy[8];
static u32 vi_out(void **out) {  // nn::vi::OpenDefaultDisplay, CreateLayer: an object the game only hands back
    *out = dummy;
    return 0;
}
static u32 vi_window(void **out, void *layer) {  // nn::vi::GetNativeWindow
    (void)layer;
    *out = dummy;  // (eglCreateWindowSurface, above, uses the host's window whatever it is handed)
    return 0;
}
static void nv_memory_info(u64 *info) {  // nv::GetGraphicsMemoryInfo
    for (u32 k = 0; k < 4; k++) info[k] = 0;
}

// nn::util::VSNPrintf, watched (<folder>/said): what the game writes with it, in the log. A game that stops itself over
// something it finds wrong puts the reason in words first, and those words go nowhere a homebrew program can show.
static int (*real_vsnprintf)(char *, u64, const char *, void *);
static char said_filter[64];
static int vsnprintf_watched(char *to, u64 size, const char *format, void *arguments) {
    const int n = real_vsnprintf(to, size, format, arguments);
    if (to && size) {
        char line[161];
        u32 k = 0;
        for (; k < sizeof line - 1 && k + 1 < size && to[k]; k++) line[k] = to[k] == 10 || to[k] == 13 ? ' ' : to[k];
        line[k] = 0;
        // (with text in <folder>/said: only what has that text in it is logged, and who asked for it, by the frames)
        if (!said_filter[0] || strstr(line, said_filter)) {
            nx2_log("[said] %s" "\n", line);
            if (said_filter[0]) {
                const u64 *frame = __builtin_frame_address(0);
                for (int i = 0; i < 16 && frame && !((u64)frame & 7); i++) {
                    const char *m = "?";
                    u64 o = frame[1] & ((1ull << 39) - 1);
                    nx2_where(o, &m, &o);
                    nx2_log("[said]   called from %s+%x" "\n", m, o - 4);
                    const u64 *up = (const u64 *)frame[0];
                    if (up <= frame || (u64)up - (u64)frame > 0x100000) break;
                    frame = up;
                }
            }
        }
    }
    return n;
}

// nn::os::CreateThread, watched: what it is given and whether it comes back (A game ends inside one, on a Switch 2,
// before the kernel is asked for the thread; 2026-11-23)
static u32 (*real_create_thread)(void *, void *, void *, void *, u64, int);
static u32 create_thread_watched(void *thread, void *entry, void *arg, void *stack, u64 size, int priority) {
    static u32 n;
    const u32 k = ++n;
    if (nx2_debug) nx2_log("[os] CreateThread %u: record %p, stack %p, %x bytes, priority %d\n", (u64)k, (u64)thread, (u64)stack, size, (s64)priority);
    if (nx2_debug) {
        const char *em = "?";
        u64 eo = (u64)entry & ((1ull << 39) - 1);
        nx2_where(eo, &em, &eo);
        nx2_log("[os]   its function: %s+%x, given %p\n", em, eo, (u64)arg);
    }
    if (nx2_debug) {
        // (who asked, as far up as the frames go; and, when it is a game's own thread class asking (main+6184dc), the
        // thread's name and the heap its stack was asked of: its 30th thread arrives with no record and no stack. Diagnosis, to go.)
        const u64 *frame = __builtin_frame_address(0);
        const char *m = "?";
        u64 o = 0, first = 0;
        for (int i = 0; i < 10 && frame && !((u64)frame & 7); i++) {
            m = "?", o = frame[1];
            nx2_where(frame[1], &m, &o);
            if (!i) first = o;
            nx2_log("[os]   called from %s+%x\n", m, o - 4);
            const u64 *up = (const u64 *)frame[0];
            if (up <= frame || (u64)up - (u64)frame > 0x10000) break;
            frame = up;
        }
        const Nx2Module *game = NULL;
        for (unsigned i = 0; i < nx2_module_count; i++)
            if (!strcmp(nx2_modules[i].name, "main")) game = &nx2_modules[i];
        if (first - 4 == 0x6184dc && arg && game) {
            const char *name = *(const char **)((u8 *)arg + 40);
            nx2_log("[os]   thread named %s\n", name ? name : "(none)");
            {  // (<folder>/trace may name a thread instead of a function: every call is logged from that thread's making)
                extern char nx2_trace[96];
                extern int nx2_trace_on;
                if (nx2_trace_on == 1 && name && nx2_trace[0] && strstr(name, nx2_trace)) nx2_trace_on = 2;
            }
            u64 **at = *(u64 ***)nx2_host(game, 0x12f9000 + 3656);
            u64 *manager = at ? *at : NULL;
            u64 *heap = manager ? ((u64 * (*)(u64 *)) nx2_host(game, 0x609f64))(manager) : NULL;
            nx2_log("[os]   heap manager %p, the current heap %p\n", (u64)manager, (u64)heap);
            if (heap) {
                for (int i = 0; i < 40; i += 4) nx2_log("[os]   heap +%x: %x %x %x %x\n", (u64)(i * 8), heap[i], heap[i + 1], heap[i + 2], heap[i + 3]);
                m = "?", o = heap[0];
                nx2_where(heap[0], &m, &o);
                nx2_log("[os]   the heap's table at %s+%x\n", m, o);
                // (the heap's blocks in use, when it gave nothing: where each is, its header, and how it begins)
                const u64 lo = heap[6], hi = heap[6] + heap[7];
                const u64 *p = (const u64 *)heap[31];
                for (int i = 0; !thread && i < 260 && (u64)p >= lo && (u64)p < hi && !((u64)p & 7); i++) {
                    m = "", o = p[4];
                    nx2_where(p[4], &m, &o);
                    nx2_log("[os]   block %p: %x %x | %x %x %x %x %s\n", (u64)p, p[2], p[3], p[4], p[5], p[6], p[7], m);
                    p = (const u64 *)p[1];
                }
            }
        }
    }
    const u32 rc = real_create_thread(thread, entry, arg, stack, size, priority);
    if (nx2_debug) nx2_log("[os] CreateThread %u: back, %x\n", (u64)k, (u64)rc);
    return rc;
}

// nn::os::InitializeMutex, watched where it is a game's heap class making its own (main+6089d4): which heap, of what
// size, in which, and who asked. The game ends on a Switch 2 right after one is made (log, 2026-11-23). Diagnosis, to go.
static void (*real_initialize_mutex)(void *, int, int);
static void initialize_mutex_watched(void *mutex, int recursive, int level) {
    real_initialize_mutex(mutex, recursive, level);
    if (!nx2_debug) return;
    const u64 *frame = __builtin_frame_address(0);
    const char *m = "?";
    u64 o = frame[1];
    nx2_where(frame[1], &m, &o);
    if (strcmp(m, "main") || o - 4 != 0x6089d4) return;
    static u32 said;  // (the first few hundred: the game makes one for each picture later on, and the log held it back)
    if (__atomic_add_fetch(&said, 1, __ATOMIC_RELAXED) > 300) return;
    const u64 *heap = (const u64 *)((u8 *)mutex - 0xb0);
    const char *name = (const char *)heap[5];
    nx2_log("[os] a heap named %s: at %p, %x bytes from %p, in the heap at %p\n", name ? name : "(none)", (u64)heap, heap[7], heap[6], heap[8]);
    for (int i = 0; i < 10 && frame && !((u64)frame & 7); i++) {
        m = "?", o = frame[1];
        nx2_where(frame[1], &m, &o);
        nx2_log("[os]   called from %s+%x\n", m, o - 4);
        const u64 *up = (const u64 *)frame[0];
        if (up <= frame || (u64)up - (u64)frame > 0x10000) break;
        frame = up;
    }
}

// A video decoder that decodes nothing. The SDK's player decodes with the console's own video decoder, through a driver a homebrew
// program on a Switch 2 isn't given; told that the start had failed, a game went on asking for room to put the video's
// data in, and the menu that shows the video ("one player") waited on its first picture for ever (2026-11-24). So the decoder's
// part is played here: the game is given room for its data, which is dropped, and for each piece it sends it is given a dark
// picture with that piece's time, by the same events the real one would tell it with. The game's own decoder object is left
// as its constructor made it; none of its other functions run.
static struct {
    void **events;  // (the game's: room for input, output ready, a change of format, an error; the order found by trying)
    int w, h, count, said;
    s64 time[16];
    u32 flags[16];
    u8 out[4];  // the pictures the game holds, by their numbers: four at most, as a real decoder has only so many buffers
} mv;
static u8 mv_in[2u << 20], mv_out[3u << 20];
static void mv_signal(int which) {
    static void (*signal)(void *);
    if (!signal) signal = (void (*)(void *))nx2_symbol("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    if (signal && mv.events && mv.events[which]) signal(mv.events[which]);
}
static u32 mv_size(void) { return (u32)(mv.w > 0 && mv.h > 0 ? mv.w * mv.h * 3 / 2 : 0); }
static void mv_say(const char *what, s64 a, s64 b) {
    if (mv.said < 400) mv.said++, nx2_log("[movie] %s (%d, %d)\n", what, a, b);
}
static void mv_buffer(void *buffer, void *memory, u32 room, u32 used) {
    static void (*set)(void *, void *, u64);
    static void (*range)(void *, int, int);
    if (!set) set = (void (*)(void *, void *, u64))nx2_symbol("_ZN5movie6Buffer18SetDataAndCapacityEPvm");
    if (!range) range = (void (*)(void *, int, int))nx2_symbol("_ZN5movie6Buffer8SetRangeEii");
    if (set) set(buffer, memory, room);
    if (range) range(buffer, 0, (int)used);
}
static void mv_format(void *data) {
    static void (*set)(void *, const char *, int);
    static void (*rect)(void *, const char *, int, int, int, int);
    if (!set) set = (void (*)(void *, const char *, int))nx2_symbol("_ZN5movie9MediaData8SetInt32EPKci");
    if (!rect) rect = (void (*)(void *, const char *, int, int, int, int))nx2_symbol("_ZN5movie9MediaData7SetRectEPKciiii");
    if (!data || !set) return;
    set(data, "width", mv.w), set(data, "height", mv.h), set(data, "stride", mv.w), set(data, "slice-height", mv.h), set(data, "color-format", 21);
    if (rect) rect(data, "crop", 0, 0, mv.w - 1, mv.h - 1);
}
static u32 mv_configure(void *decoder, void *format) {
    (void)decoder;
    static int (*find)(const void *, const char *, int *);
    if (!find) find = (int (*)(const void *, const char *, int *))nx2_symbol("_ZNK5movie9MediaData9FindInt32EPKcPi");
    mv.w = mv.h = 0, mv.count = 0;
    if (find && format) find(format, "width", &mv.w), find(format, "height", &mv.h);
    if (mv.w <= 0 || mv.h <= 0 || mv_size() > sizeof mv_out) mv.w = 1280, mv.h = 720;
    memset(mv_out, 16, (size_t)mv.w * (size_t)mv.h);  // (a dark picture: luma, then the two colors' middle)
    memset(mv_out + (size_t)mv.w * (size_t)mv.h, 128, (size_t)mv.w * (size_t)mv.h / 2);
    nx2_log("[movie] the game sets a video decoder up: %d x %d. There is none for this port on a Switch 2: the game is given dark pictures, at the video's own times\n", (s64)mv.w, (s64)mv.h);
    return 0;
}
static u32 mv_events(void *decoder, void **events) {
    (void)decoder;
    mv.events = events;
    return 0;
}
static u32 mv_out_size(void *decoder, int *size, int *align) {
    (void)decoder;
    if (size) *size = (int)mv_size();
    if (align) *align = 16;
    return 0;
}
static u32 mv_start(void *decoder) {
    (void)decoder;
    mv.count = 0;
    mv_say("started", 0, 0);
    {
        extern void nx2_trail(int lines);
        nx2_trail(6000);
    }
    mv_signal(2), mv_signal(0);
    return 0;
}
static u32 mv_stop(void *decoder) {
    (void)decoder;
    mv.count = 0;
    memset(mv.out, 0, sizeof mv.out);
    mv_say("stopped, flushed or reset", 0, 0);
    return 0;
}
static u32 mv_in_index(void *decoder, int *index, int *remaining) {
    (void)decoder;
    if (mv.count >= 3) {  // (pictures wait that the game hasn't taken: no room for more data until it does)
        if (index) *index = -1;
        if (remaining) *remaining = 0;
        mv_say("room for input asked: none for now", 0, 0);
        return 1;
    }
    if (index) *index = 0;
    if (remaining) *remaining = 0;
    mv_say("room for input asked", 0, 0);
    return 0;
}
static u32 mv_in_buffer(void *decoder, int index, void *buffer) {
    (void)decoder, (void)index;
    mv_buffer(buffer, mv_in, sizeof mv_in, 0);
    return 0;
}
static u32 mv_send(void *decoder, int index, int offset, int size, s64 time, u32 flags) {
    (void)decoder, (void)index, (void)offset;
    mv_say("input sent: its size, its flags", size, flags);
    {
        // (No faster than the video itself, a fifth of a second ahead at most: the real decoder takes its time, and the game
        // counts on it. Given every picture at once, as fast as it could send the data, a game ended.)
        static s64 first_time, last_time;
        static u64 first_tick;
        const u64 now = armGetSystemTick();
        if (!first_tick || time < last_time || time > last_time + 2000000) first_tick = now, first_time = time;
        last_time = time;
        for (int tries = 0; tries < 400; tries++) {
            const s64 played = (s64)(armTicksToNs(armGetSystemTick() - first_tick) / 1000);
            if (time - first_time <= played + 200000) break;
            svcSleepThread(5000000);
        }
    }
    if (mv.count < 16) mv.time[mv.count] = time, mv.flags[mv.count] = flags, mv.count++;
    mv_signal(1);
    if (mv.count < 3) mv_signal(0);
    return 0;
}
static u32 mv_out_index(void *decoder, int *index, s64 *time, u32 *flags, int *remaining) {
    (void)decoder;
    if (!mv.count) {
        if (index) *index = -1;
        if (remaining) *remaining = 0;
        mv_say("a picture asked for, none ready", 0, 0);
        return 1;
    }
    int number = -1;
    for (int k = 0; k < 4 && number < 0; k++)
        if (!mv.out[k]) number = k;
    if (number < 0) {  // (the game holds every picture there is: it lets one go first)
        if (index) *index = -1;
        if (remaining) *remaining = mv.count;
        mv_say("a picture asked for: the game holds all four", 0, 0);
        return 1;
    }
    mv.out[number] = 1;
    if (index) *index = number;
    if (time) *time = mv.time[0];
    if (flags) *flags = mv.flags[0];
    mv.count--;
    memmove(mv.time, mv.time + 1, sizeof mv.time[0] * (size_t)mv.count), memmove(mv.flags, mv.flags + 1, sizeof mv.flags[0] * (size_t)mv.count);
    if (remaining) *remaining = mv.count;
    mv_say("a picture given: its number, how many more wait", number, mv.count);
    mv_signal(0);
    return 0;
}
static u32 mv_out_buffer(void *decoder, int index, void *buffer) {
    (void)decoder, (void)index;
    // (the picture is copied into the memory the game's buffer already has: the game gives it its own, and reads the picture there)
    static void *(*base)(const void *);
    static u64 (*capacity)(const void *);
    static void (*range)(void *, int, int);
    if (!base) base = (void *(*)(const void *))nx2_symbol("_ZNK5movie6Buffer4BaseEv");
    if (!capacity) capacity = (u64 (*)(const void *))nx2_symbol("_ZNK5movie6Buffer8CapacityEv");
    if (!range) range = (void (*)(void *, int, int))nx2_symbol("_ZN5movie6Buffer8SetRangeEii");
    void *to = base && buffer ? base(buffer) : NULL;
    const u64 room = capacity && buffer ? capacity(buffer) : 0;
    const u64 size = mv_size() < room ? mv_size() : room;
    mv_say("a picture copied to the game's buffer: its room, what is copied", (s64)room, (s64)size);
    if (to && size) memcpy(to, mv_out, size);
    if (range && to) range(buffer, 0, (int)size);
    return 0;
}
static u32 mv_out_buffer_format(void *decoder, int index, void *buffer, void *format) {
    mv_format(format);
    return mv_out_buffer(decoder, index, buffer);
}
static u32 mv_out_copy(void *decoder, int index, void *to, int *size) {
    (void)decoder, (void)index;
    mv_say("a picture copied out", size ? *size : -1, 0);
    if (to) memcpy(to, mv_out, mv_size());
    if (size) *size = (int)mv_size();
    return 0;
}
static u32 mv_release(void *decoder, int index) {
    (void)decoder;
    if (index >= 0 && index < 4) mv.out[index] = 0;
    mv_say("a picture let go: its number", index, 0);
    if (mv.count) mv_signal(1);
    return 0;
}
static u32 mv_get_format(void *decoder, void *format) {
    (void)decoder;
    mv_say("the pictures' format asked", 0, 0);
    mv_format(format);
    return 0;
}
static u32 mv_get_buffer_format(void *decoder, int index, void *format) {
    (void)decoder, (void)index;
    mv_format(format);
    return 0;
}
static u32 mv_no_error(void *decoder) {
    (void)decoder;
    return 0;
}

void *nx2_override(const char *name) {
    static const struct {
        const char *name;
        void *with;
    } movie[] = {{"9ConfigureEPNS_9MediaDataE", mv_configure}, {"20RegisterClientEventsEPNS_13DecoderEventsE", mv_events},
                 {"19GetOutputBufferSizeEPiS1_", mv_out_size}, {"5StartEv", mv_start}, {"4StopEv", mv_stop}, {"5FlushEv", mv_stop}, {"5ResetEv", mv_stop},
                 {"23AcquireInputBufferIndexEPiS1_", mv_in_index}, {"14GetInputBufferEiPNS_6BufferE", mv_in_buffer},
                 {"24SendInputBufferForDecodeEiiilj", mv_send}, {"24AcquireOutputBufferIndexEPiPlPjS1_", mv_out_index},
                 {"15GetOutputBufferEiPNS_6BufferE", mv_out_buffer}, {"15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", mv_out_buffer_format},
                 {"15GetOutputBufferEiPvPi", mv_out_copy}, {"24ReleaseOutputBufferIndexEi", mv_release}, {"15GetOutputFormatEPNS_9MediaDataE", mv_get_format},
                 {"21GetOutputBufferFormatEiPNS_9MediaDataE", mv_get_buffer_format}, {"12GetLastErrorEv", mv_no_error}};
    if (!strncmp(name, "_ZN5movie11DecoderImpl", 22))
        for (u32 k = 0; k < sizeof movie / sizeof *movie; k++)
            if (!strcmp(name + 22, movie[k].name)) return movie[k].with;
    // The process's name, which a Switch 2 game's SDK puts on its log's lines: it is read from the header of the module nnMain is
    // in, found by walking the memory map from nnMain's address back to where its memory begins. Here that is the port's own start,
    // not the game's module, and a game ended in that search at its first printed line (log, 2026-11-24). The name is
    // the port's to give.
    if (!strcmp(name, "_ZN2nn4diag6detail21GetProcessNamePointerEPPKcPm")) return process_name;
#ifdef NX2_GAME_MB
    // (A try: the thread that starts the amiibo library never comes back from it. A game is closed by the system a
    // moment after that thread's start, every run; 2026-11-24.)
    {  // (tries, chosen by <folder>/nfptry: letters for what is left out)
        static int tries = -1;
        if (tries < 0) {
            tries = 0;
            FILE *f = fopen(NX2_F("nfptry"), "rb");
            if (f) {
                char t[16] = {0};
                fread(t, 1, sizeof t - 1, f);
                fclose(f);
                for (const char *c = t; *c; c++) tries |= *c == 'n' ? 1 : *c == 'b' ? 2 : *c == 'h' ? 4 : *c == 'q' ? 8 : *c == 'a' ? 16 : 0;
            }
        }
        if (!strcmp(name, "_ZNK2nn4util16BinaryFileHeader13GetFirstBlockEv")) {
            real_first_block = (const u8 *(*)(const u8 *))nx2_symbol(name);
            if (real_first_block) return first_block_watched;
        }
        if ((tries & 1) && !strcmp(name, "_ZN2nn3nfp10InitializeEv")) return asleep_for_good;
        if ((tries & 2) && !strcmp(name, "_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE")) return nothing;
        if ((tries & 4) && (!strcmp(name, "_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE") ||
                            !strcmp(name, "_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE")))
            return nothing_ok;
        if ((tries & 16) && !strcmp(name, "_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE")) return nothing_ok;
        if ((tries & 8) && !strcmp(name, "_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE")) return hdr_none;
    }
#endif
    if (!strcmp(name, "_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi")) {
        real_initialize_mutex = (void (*)(void *, int, int))nx2_symbol(name);
        if (real_initialize_mutex) return initialize_mutex_watched;
    }
    if (!strcmp(name, "_ZN2nn4util9VSNPrintfEPcmPKcSt9__va_list")) {
        static int said = -1;
        if (said < 0) {
            FILE *f = fopen(NX2_F("said"), "rb");
            said = f != NULL;
            if (f) {
                size_t got = fread(said_filter, 1, sizeof said_filter - 1, f);
                while (got && (said_filter[got - 1] == 10 || said_filter[got - 1] == 13 || said_filter[got - 1] == ' ')) got--;
                said_filter[got] = 0;
                if (got <= 1) said_filter[0] = 0;  // (a file made with one character in it: no filter)
                fclose(f);
            }
        }
        real_vsnprintf = (int (*)(char *, u64, const char *, void *))nx2_symbol(name);
        if (said && real_vsnprintf) return vsnprintf_watched;
    }
    if (!strcmp(name, "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi")) {
        real_create_thread = (u32 (*)(void *, void *, void *, void *, u64, int))nx2_symbol(name);
        if (real_create_thread) return create_thread_watched;
    }
    // A Switch 2 game (<folder>/native is there): its SDK's own graphics are left to it, the display and NVN's driver, which
    // is made for this console's graphics processor. Nothing of those is replaced, and the host's own are never started.
    static int native = -1;
    if (native < 0) {
        FILE *f = fopen(NX2_F("native"), "rb");
        native = f != NULL;
        if (f) fclose(f);
#ifdef NX2_GAME_MB
        native = 1;  // (built as a Switch 2 game, tools/build.py: the file is for a try with any other)
#endif
        if (native) nx2_log("[gl] a Switch 2 game, or <folder>/native is there: the game's own graphics are left to its SDK\n");
    }
    if (native) {
        extern void *nx2_nvn_watch(const char *name);  // (nvnwatch.c: the driver's own functions, counted as the game calls them)
        void *watch = nx2_nvn_watch(name);
        if (watch) return watch;
#ifdef NX2_GAME_MB
        if (!strcmp(name, "_ZN2nn2fs18CreateCacheStorageEill") && nx2_has_sd_save_fs())
            return nx2_create_cache_storage;

        // A Switch 2 game's AudioOut is its SDK's own, as its audio renderer is: this console's SDK reaches both through aud:u
        // (its ListAudioOuts and its audio renderer manager open the same session of it), which the game the port is started
        // over may open. The silent output of audio2.c is for a Switch game, whose SDK asks for audout:u and is refused; it was
        // given here as well, and a Switch 2 game that plays through AudioOut ran with no sound (reports, 2026-11-26).
        // (<folder>/silentaudio: the silent output all the same, for a game that doesn't get past opening its sound.)
        static int silent = -1;
        if (silent < 0) {
            FILE *f = fopen(NX2_F("silentaudio"), "rb");
            silent = f != NULL;
            if (f) fclose(f);
            if (silent) nx2_log("[audio] <folder>/silentaudio is there: the game's AudioOut is given a silent output\n");
        }
        if (!silent) {
            extern int nx2_audio_out_name(const char *name);  // (audio2.c)
            static int said;
            if (!said && nx2_audio_out_name(name))
                said = 1, nx2_log("[audio] the game asks for the SDK's AudioOut: left to its SDK, as its audio renderer is\n");
            return NULL;
        }
#endif
        return nx2_audio_override(name);
    }
    static const struct {
        const char *name;
        void *fn;
        int host;
    } table[] = {
        {"eglGetDisplay", h_get_display, 1},
        {"eglInitialize", h_initialize, 1},
        {"eglChooseConfig", h_choose_config, 1},
        {"eglCreateContext", h_create_context, 1},
        {"eglCreateWindowSurface", h_create_surface, 1},
        {"eglMakeCurrent", h_make_current, 1},
        {"eglSwapBuffers", g_swap, 0},
        {"_ZN2nn2oe27GetDefaultDisplayResolutionEPiS1_", display_resolution, 0},
        {"_ZN2nn2oe16GetOperationModeEv", operation_mode, 0},
        {"_ZN2nn2oe18GetPerformanceModeEv", performance_mode, 0},
        {"eglDestroySurface", h_destroy_surface, 1},
        {"eglGetProcAddress", h_get_proc, 1},
        {"_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nothing, 0},
        {"_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nothing, 0},
        {"_ZN2nv18InitializeGraphicsEPvm", nothing, 0},
        {"_ZN2nv22DumpGraphicsMemoryInfoEv", nothing, 0},
        {"_ZN2nv21GetGraphicsMemoryInfoEPNS_10MemoryInfoE", nv_memory_info, 0},
        {"_ZN2nn2vi10InitializeEv", nothing, 0},
        {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_out, 0},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_out, 0},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEii", vi_out, 0},
        {"_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", nothing, 0},
        {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_window, 0},
        // (the layer handed out above is nothing of the SDK's: what it would do to one is left undone. A game was ended in
        // SetLayerScalingMode, given it; log, 2026-11-23)
        {"_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", nothing, 0},
        {"_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", nothing, 0},
        {"_ZN2nn2vi12SetLayerSizeEPNS0_5LayerEii", nothing, 0},
        {"_ZN2nn2vi13SetLayerAlphaEPNS0_5LayerEf", nothing, 0},
        {"_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", nothing, 0},
        {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_vsync, 0},
    };
    if (!host_swap) host_swap = (EGLBoolean (*)(EGLDisplay, EGLSurface))through_host(h_swap);
    // nn::hid::GetNpadState, for each kind of state the game reads: the SDK's, with a look at the buttons
    static const struct {
        const char *name;
        void *fn;
        void (**real)(u64 *, const u32 *);
    } pads[] = {
        {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", pad0, &real_pad0},
        {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", pad1, &real_pad1},
        {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", pad2, &real_pad2},
        {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", pad3, &real_pad3},
        {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", pad4, &real_pad4},
    };
    for (u32 k = 0; k < sizeof pads / sizeof *pads; k++)
        if (!strcmp(pads[k].name, name)) {
            *pads[k].real = (void (*)(u64 *, const u32 *))nx2_symbol(name);
            return *pads[k].real ? pads[k].fn : NULL;
        }
    for (u32 k = 0; k < sizeof table / sizeof *table; k++)
        if (!strcmp(table[k].name, name)) return table[k].host ? through_host(table[k].fn) : table[k].fn;
    if (font_size) {
        if (!strcmp(name, "_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE")) return font_state;
        if (!strcmp(name, "_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE")) return font_bytes;
        if (!strcmp(name, "_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE")) return font_address;
        if (!strcmp(name, "_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE")) return nothing;
        if (!strcmp(name, "_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE")) return font_list;
    }
    void *nvn = nx2_nvn_override(name);
    return nvn ? nvn : nx2_audio_override(name);
}
#endif
