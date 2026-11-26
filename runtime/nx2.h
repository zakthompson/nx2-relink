// nx2-recomp: a Switch game's modules relinked into this program (tools/relink.py) and run in place.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
typedef int32_t s32;

// Where a port keeps everything of its own on the card: one folder for each port, at the card's root, under the tool's name.
// "<folder>" in this code's comments and messages is that folder.
//     sdmc:/nx2-relink/<name>/     a Switch 2's port            sdmc:/nx-relink/<name>/     a Switch's
//     (the port itself is in the homebrew menu's folder, /switch/<tool>/<name>.nro, with nothing else: tools/install.py)
//       romfs.bin              the game's files (or romfs.bin.00, .01, ...: parts of 2 GB, for a FAT32 card)
//       aoc<n>.romfs           its add-on content's
//       save/                  the game's saved data
//       log.txt, log.prev.txt  the log of the last run, and of the one before it
//       crash.txt              the last run that ended badly: the file to send with a report of it (log.c)
//       debug, ...             switch files (empty will do): "debug" turns the long log on
#ifdef __SWITCH2__
#define NX2_TOOL "nx2-relink"
#else
#define NX2_TOOL "nx-relink"
#endif
#define NX2_HOME "/" NX2_TOOL "/" NX2_NAME  // (as the file system service is given it)
#define NX2_DIR "sdmc:" NX2_HOME "/"
#define NX2_F(name) (NX2_DIR name)       // a file of the port's folder

// One module of the game. Addresses are the module's own (0 = its first byte of code) unless they are pointers. Code and read-only
// data are at text, as they were; data and bss, which followed them at data_off, are at data.
typedef struct {
    const char *name;
    u8 *text, *data;
    u64 data_off, end;  // where the data began, and where the bss ends (page rounded)
    u64 mod0, dynamic;
    u64 dynsym, nsyms, dynstr, dynstr_size;
    u64 module_object;  // rtld's record for the module, in its bss
} Nx2Module;
extern Nx2Module nx2_modules[];
// A module kept whole (tools/relink.py --whole): its image as the NSO has it, where its code ends, where its read-only data is, and
// where its system calls are (pairs: the place in the code, the call's number). The loader maps it at run time (nx2_map_modules).
typedef struct {
    const u8 *image;
    u64 image_size, text_size, ro_off, ro_size;
    const u32 *svcs;
    u32 nsvcs;
} Nx2Image;
extern const Nx2Image nx2_images[];
extern const int nx2_whole;  // 1: the modules are kept whole, and text and data above are set when they are mapped
extern const unsigned nx2_module_count;

static inline u8 *nx2_host(const Nx2Module *m, u64 a) { return a < m->data_off ? m->text + a : m->data + (a - m->data_off); }

// loader.c: the modules' relocations and symbols (what rtld does on the console), then the game's start
int nx2_load(void);                  // 0: done
void *nx2_symbol(const char *name);  // a symbol some module defines, or null
void nx2_start(u32 main_thread);     // doesn't return
int nx2_where(u64 host, const char **module, u64 *offset);  // which module an address is in

// svc.c: every system call of the game's comes here first (svc_entry.s). regs: x0-x7 in and out. 1: answered here; 0: the real one runs.
int nx2_svc(u64 *regs, u32 number);
void nx2_svc_verbose(int on);  // 1: every call logged (a budget of them)
void nx2_heap_set(void *base, u64 size);  // the block SetHeapSize hands the game
// ro.c: the modules a Switch 2 game loads as it runs (nn::ro), through kaiHEN's loader service
void nx2_ro_start(void);
int nx2_ro_ipc(u8 *m, const u8 *q, u32 cmd, const char *module, u64 off);  // 1: the message was seen to (the kernel's word in nx2_ro_kernel)
extern u32 nx2_ro_kernel;
void nx2_crash(const char *why);  // the run is ending badly, and why: <folder>/crash.txt is written as it ends (log.c)
void nx2_crash_now(const char *why);  // the same, written before it returns (a fault's handler)
void nx2_closing(void);           // the user is closing the game: not a run that ended badly
void nx2_log_end(void);           // the program returns to where it was started from
void nx2_heap_live(u64 base, u64 host, u64 most);  // (a Switch 2 game's: no block; the kernel's heap grows after the host's part)

// gl2.c (Switch 2): the game's imports that go to the host's OpenGL, or nowhere, in place of the SDK's graphics driver
void *nx2_override(const char *name);  // what an import of this name is bound to, or null: the modules' own
void *nx2_audio_override(const char *name);  // audio2.c: the same for the game's sound
void *nx2_nvn_override(const char *name);    // nvn2.c: the same for a game that draws with NVN
void *nx2_through_host(void *target);        // a host function, as the game may call it (through nx2_host_call)
u32 nx2_present(void *display, void *surface);  // a picture is done: shown, after the menu's check
void *nx2_traced(const char *name, void *target);  // loader.c: a function whose first call is said in the log
void nx2_gl_config(void);              // the picture's size, from the menu's file
// menu2.c (Switch 2): the port's menu. Its functions run as the host.
typedef struct {
    int screen_w, screen_h;  // the window
    int selected;            // 0: continue; 1: resolution; 2: the long log; 3: close the game
    int res_h, current_h;    // the resolution chosen for the next start, and the one in use
    int debug;               // the long log is on
} Nx2Menu;
void nx2_menu_begin(void);
void nx2_menu_draw(const Nx2Menu *m);
void nx2_menu_end(void);
void nx2_menu_quit(void);  // the system asked to close the program (it does so itself, with no error shown)
void nx2_menu_save(int res_h);
void nx2_menu_debug(int on);
int nx2_menu_load(void);
extern u32 nx2_audren_master;  // ... and of its audio renderer manager
extern u32 nx2_audio_master;  // a session of the Switch 2's audio output manager (0: none; the game's sound is then a silent one)
void nx2_host_capture(void);          // on the main thread, before the game has it

// rom.c: the game's files (RomFS), served from <folder>/romfs.bin in place of the ones the system would give
void nx2_rom_open(void);
int nx2_rom_ipc(u8 *message);  // a message to the file system service: 1 if answered here
int nx2_aoc_ipc(u8 *message);  // a message to the add-on content service: 1 if answered here
void nx2_save_open(void);      // the game's saved data: a folder of the card (<folder>/save)
int nx2_has_sd_save_fs(void);
u32 nx2_create_cache_storage(s32 index, s64 data_size, s64 journal_size);
int nx2_save_before(u8 *message, char (*paths)[0x301], u32 *handle);
void nx2_save_after(u8 *message, int how);

const u8 *nx2_ipc_data(const u8 *message, u64 *buf, u64 *buf_size);  // a request's data, and its first receive buffer
void nx2_ipc_answer(u8 *message, u32 result, const u8 *raw, u32 raw_size, u32 object);  // an answer in a domain, written

// log.c: a log any thread can write, the game's included (no C library, no thread-local storage: the game's threads have the
// console's own in theirs). %s %x %X (64 bit) %u %d %p %c only. A thread of the port's writes it to <folder>/log.txt.
void nx2_log(const char *fmt, ...);
void nx2_log_start(void);
void nx2_log_wait(int on);  // 1: every line is on the card before nx2_log returns
// 1: the long log, every system call the game makes for its first 1500 and more besides. For finding out why a game doesn't run;
// each line is on the card before the game goes on, which took the first sixty pictures of a game 3.3 seconds in place of
// one (a Switch 2, 2026-11-23). It is on when the file <folder>/debug is there (empty will do). Without it the log still has
// what ends a game: a request the system refused, a function called for the first time, the SDK's own abort.
extern int nx2_debug;
extern int nx2_ownapplet;  // the SDK registers with the system itself, as on a Switch (<folder>/ownapplet)
extern int nx2_noplt;  // calls between modules go straight to the function, none logged (<folder>/noplt)
extern int nx2_stackwatch;  // a thread's stack left at each system call, its least said (<folder>/stackwatch)
extern int nx2_bounce;  // the game's files are read into the port's memory first, then copied (<folder>/bounce)
extern int nx2_nvnlock;  // the watched calls of the game's graphics driver, one at a time (<folder>/nvnlock)
extern int nx2_hold_from, nx2_hold_to;  // the game's threads of these numbers are never started (<folder>/hold)
extern int nx2_pin;      // each of the game's threads stays on one core for good (<folder>/pin)
extern int nx2_ipclog;   // every message to a service is logged, with no budget (<folder>/ipclog)
extern int nx2_onecore;  // the game's threads stay on the system's default core (<folder>/onecore)
void nx2_log_flush(void);  // to the card now (the port's own threads only: it uses the C library)
void nx2_exit(void) __attribute__((noreturn));  // the log written out, then the process ends

// nvnwatch.c, netwatch.c: the watched calls of a game's own graphics driver, each one's beginning and end
enum { NX2_NVN_RING = 65536 };
struct nx2_nvn_mark {
    u64 thread, a, b, r;
    u32 call;
    u16 what, phase;
};
extern struct nx2_nvn_mark nx2_nvn_ring[NX2_NVN_RING];
extern u32 nx2_nvn_ring_in;
