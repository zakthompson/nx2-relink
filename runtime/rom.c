// The game's own files (its RomFS), given to it from the card. The SDK asks the file system service for "the data storage of the
// current process" (fsp-srv 200) and reads it as one long image; but the process is whatever program the port was started over, so
// the system would hand over that program's files (the game then finds none of its own: a null from its first file, and a fault;
// crash report, 2026-11-22). So that request is answered here, with an object that is the image tools/nsp.py --romfs extracts,
// copied to <folder>/romfs.bin: its reads are served from that file. (An image over 4 GB doesn't fit a file of a FAT32 card's:
// it can be in parts instead, <name>.romfs.00, .01, ..., all of one size but the last.)
// On a game's thread: system calls and the host library's service calls only (they use the thread's message buffer, nothing else).
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "nx2.h"

#ifndef NX2_NAME
#define NX2_NAME "game"
#endif
#define ROM_OBJECT 0x4E583200u  // the storages' ids in the session's domain, as the SDK is told (none of the service's are this high)

// An image on the card: the game's own RomFS (the first), then one for each add-on content it has there (DLC):
// <folder>/aoc<index>.romfs, which tools/nsp.py --aoc writes. The SDK asks for those by "data id", the last digits of which
// are the add-on's index (fsp-srv 202), after asking the add-on content service which there are (nx2_aoc_ipc).
#define ROM_PARTS 16
#define ROM_IMAGES 41
typedef struct {
    FsFile part[ROM_PARTS];
    s64 size, part_size;  // the whole image's size, and one part's (the whole, for an image in one file)
    u32 parts, index;     // (index: the add-on's; 0 for the game)
    int ok;
} Image;
static Image images[ROM_IMAGES];
static u32 image_count = 1, rom_logged;

static void image_open(Image *im, char *path) {
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    if (!sd) return;
    if (!fsFsOpenFile(sd, path, FsOpenMode_Read, &im->part[0]) && R_SUCCEEDED(fsFileGetSize(&im->part[0], &im->size))) {
        im->ok = 1, im->parts = 1, im->part_size = im->size;
        return;
    }
    u32 end = 0;  // in parts
    while (path[end]) end++;
    path[end] = '.', path[end + 3] = 0;
    for (; im->parts < ROM_PARTS; im->parts++) {
        s64 size = 0;
        path[end + 1] = (char)('0' + im->parts / 10), path[end + 2] = (char)('0' + im->parts % 10);
        if (fsFsOpenFile(sd, path, FsOpenMode_Read, &im->part[im->parts]) || R_FAILED(fsFileGetSize(&im->part[im->parts], &size))) break;
        if (!im->parts) im->part_size = size;
        im->size += size;
    }
    path[end] = 0;
    im->ok = im->parts > 0 && im->part_size > 0;
}

static void mods_open(Image *im);  // (<folder>/mods, further down)
static u32 rom_read(Image *im, u64 offset, u8 *to, u64 size, u64 *got);
// Is the image on the card all there? Its header says where its tables are, and they come last: a copy that was cut short, or a
// card that ran out of room, leaves a file the game's SDK calls damaged and ends the program on, with nothing said of why (a
// report, 2026-11-25: 20480 MB in 10 files, and "data is corrupted" at the game's first look). Said here, in words.
static void image_whole(Image *im) {
    if (!im->ok) return;
    static u8 head[0x50];
    u64 got = 0, w[10];
    if (rom_read(im, 0, head, sizeof head, &got) || got != sizeof head) {
        nx2_log("[rom] THE GAME'S FILES ON THE CARD CAN'T BE READ (the image's first bytes): install the port again, the game's files with it\n");
        return;
    }
    for (int k = 0; k < 10; k++) {
        w[k] = 0;
        for (int b = 7; b >= 0; b--) w[k] = w[k] << 8 | head[k * 8 + b];
    }
    u64 end = w[9];  // (where the files' own bytes begin; the four tables: where each is, and how long)
    for (int k = 1; k < 9; k += 2)
        if (w[k] + w[k + 1] > end) end = w[k] + w[k + 1];
    if (w[0] != 0x50)
        nx2_log("[rom] THE GAME'S FILES ON THE CARD DON'T BEGIN AS AN IMAGE OF THEM DOES (%x where 50 is): install the port again, the game's files with it\n", w[0]);
    else if (end > (u64)im->size)
        nx2_log("[rom] THE GAME'S FILES ON THE CARD ARE INCOMPLETE: the image is %u MB long by its own header, and %u MB of it are there. A copy cut "
                "short, or a card with no room left: install the port again, the game's files with it\n", end >> 20, (u64)im->size >> 20);
    else
        nx2_log("[rom] the image is whole by its own header (%u MB)\n", end >> 20);
}
static void mods_start(void);
void nx2_rom_open(void) {
    static char path[FS_MAX_PATH] = NX2_HOME "/romfs.bin";
    image_open(&images[0], path);
    if (images[0].ok) nx2_log("[rom] sdmc:%s: %u MB in %u file(s), the game's files\n", path, (u64)images[0].size >> 20, (u64)images[0].parts);
    else nx2_log("[rom] sdmc:%s isn't there: the game gets the files of the program this was started over\n", path);
    image_whole(&images[0]);
    mods_open(&images[0]);
    for (u32 index = 1; index < ROM_IMAGES && image_count < ROM_IMAGES; index++) {
        static char aoc[FS_MAX_PATH];
        const char *head = NX2_HOME "/aoc";
        u32 n = 0;
        for (; head[n]; n++) aoc[n] = head[n];
        if (index >= 10) aoc[n++] = (char)('0' + index / 10);
        aoc[n++] = (char)('0' + index % 10);
        for (const char *t = ".romfs"; *t; t++) aoc[n++] = *t;
        aoc[n] = 0;
        Image *im = &images[image_count];
        image_open(im, aoc);
        if (!im->ok) continue;
        im->index = index, image_count++;
        nx2_log("[rom] sdmc:%s: %u KB, add-on content %u\n", aoc, (u64)im->size >> 10, (u64)index);
    }
}

// size bytes of an image at offset (in range), from the part or parts they are in
static u32 rom_read(Image *im, u64 offset, u8 *to, u64 size, u64 *got) {
    *got = 0;
    while (size) {
        const u64 part = offset / (u64)im->part_size, in = offset % (u64)im->part_size;
        u64 take = (u64)im->part_size - in, n = 0;
        if (take > size) take = size;
        if (part >= im->parts) return 0x2EE202;
        // A read is a message to the file system with the memory to fill, and for as long as the message is out the kernel has that
        // memory locked and out of the program's reach. On its console a game's read takes a moment; from a card it takes far
        // longer, and whatever else needs those pages meanwhile fails: the graphics driver mapping the same pool of memory for
        // the graphics processor, as a game does while its textures load (it was closed with no word a few seconds
        // in, the thread reading and the thread mapping both mid-way; 2026-11-24). So the read fills memory of the port's, one
        // thread at a time, and what arrived is copied into the game's: the game's memory is never locked for long.
        static u8 bounce[4 << 20] __attribute__((aligned(0x1000)));
        static u32 bounce_lock;
        u32 rc = 0;
        if (nx2_bounce) {
            if (take > sizeof bounce) take = sizeof bounce;
            while (__atomic_exchange_n(&bounce_lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
            rc = fsFileRead(&im->part[part], (s64)in, bounce, take, FsReadOption_None, &n);
            for (u64 k = 0; k < n; k++) to[k] = bounce[k];
            __atomic_store_n(&bounce_lock, 0, __ATOMIC_RELEASE);
        } else {
            rc = fsFileRead(&im->part[part], (s64)in, to, take, FsReadOption_None, &n);
        }
        *got += n;
        if (rc) return rc;
        if (n != take) return 0;
        offset += take, to += take, size -= take;
    }
    return 0;
}

static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 rd64(const u8 *p) { return rd32(p) | (u64)rd32(p + 4) << 32; }
static void wr32(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24); }

// a request's data (after its header and buffer descriptors, on a 16 byte boundary), and its first receive buffer
// ---------------------------------------------------------------- <folder>/mods: files of the user's, laid over the game's own
// For trying a mod without making the port or its image again: a file put in <folder>/mods/ under the path it has in the game
// (mods/Tex/Some.bntx.zs) is what the game gets in place of its own file of that path, from the next time it is started.
// The game is told nothing. It finds its files itself, in the image's table of files, which it reads through this storage; that
// table is read into memory once, at the start, and the entry of each file that is in the folder is changed there to say where
// the file's bytes are now and how many: past the image's end, a place of this storage that is served from the loose file. The
// rest of the image is served as it always was.
// With no such folder, or nothing in it that the game has, none of this is in use: the storage is the image, read as before.
// Only files the game already has are replaced: one it hasn't is said in the log and left out (the tables of names would have to
// be made again for it). A file has to be in the form the game expects, its packing included.
#define MODS_MOST 8192
#define MODS_OPEN 6
typedef struct {
    u64 at, size;  // where its bytes are in the storage, and how many
    u32 path;      // its path under the port's folder, in mods.paths
    u32 read;      // the game has read from it (said once in the log: that a file put there is really the one in use)
} Mod;
static struct {
    int on;
    u64 size;                  // the storage's size, with the files laid over it
    u64 table_at, table_size;  // the image's table of files: where it is, and the copy of it that is given in its place
    u8 *table;
    Mod *files;
    u32 count;
    char *paths;
    u32 paths_used, paths_room;
    struct {
        FsFile file;
        u32 which, age;
        int open;
    } kept[MODS_OPEN];  // (the loose files being read: a few stay open, the one unused for longest closed for the next)
    u32 lock, clock;
} mods;

static int mods_name_is(const u8 *entry_name, u32 entry_len, const char *name, u32 len) {
    if (entry_len != len) return 0;
    for (u32 k = 0; k < len; k++)
        if (entry_name[k] != (u8)name[k]) return 0;
    return 1;
}
// a file's entry in the image's tables, by its path ("Tex/Some.bntx"): its offset in the table of files, or ~0
static u32 mods_find(const u8 *dirs, u64 dirs_size, const u8 *files, u64 files_size, const char *path) {
    u32 dir = 0;
    for (;;) {
        u32 len = 0;
        while (path[len] && path[len] != '/') len++;
        if (!path[len]) {
            if ((u64)dir + 0x18 > dirs_size) return ~0u;
            for (u32 f = rd32(dirs + dir + 0xC); f != ~0u; f = rd32(files + f + 4)) {
                if ((u64)f + 0x20 > files_size) return ~0u;
                const u32 n = rd32(files + f + 0x1C);
                if ((u64)f + 0x20 + n > files_size) return ~0u;
                if (mods_name_is(files + f + 0x20, n, path, len)) return f;
            }
            return ~0u;
        }
        if ((u64)dir + 0x18 > dirs_size) return ~0u;
        u32 child = rd32(dirs + dir + 8);
        for (; child != ~0u; child = rd32(dirs + child + 4)) {
            if ((u64)child + 0x18 > dirs_size) return ~0u;
            const u32 n = rd32(dirs + child + 0x14);
            if ((u64)child + 0x18 + n > dirs_size) return ~0u;
            if (mods_name_is(dirs + child + 0x18, n, path, len)) break;
        }
        if (child == ~0u) return ~0u;
        dir = child, path += len + 1;
    }
}
static u32 mods_keep(const char *text) {  // a string kept in mods.paths: where
    u32 len = 0;
    while (text[len]) len++;
    if (mods.paths_used + len + 1 > mods.paths_room) {
        const u32 room = mods.paths_room ? mods.paths_room * 2 : 1u << 16;
        char *more = realloc(mods.paths, room);
        if (!more) return ~0u;
        mods.paths = more, mods.paths_room = room;
    }
    const u32 at = mods.paths_used;
    for (u32 k = 0; k <= len; k++) mods.paths[at + k] = text[k];
    mods.paths_used += len + 1;
    return at;
}
// (at the start, before the game has a thread: the C library's memory is the host's to use here)
// (the game loader's: the image is its own making, files of a mods folder among the rest: read through game.c)
static void mods_start(void) {
}
static void mods_open(Image *im) {
    static const char folder[] = NX2_HOME "/mods";
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    FsDir top;
    if (!sd || !im->ok || R_FAILED(fsFsOpenDirectory(sd, folder, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &top))) return;
    fsDirClose(&top);
    u8 head[0x50];
    u64 got = 0;
    if (rom_read(im, 0, head, sizeof head, &got) || got != sizeof head || rd64(head) != 0x50) {
        nx2_log("[mods] sdmc:%s is there, but the game's image isn't one whose tables this reads: nothing is laid over it\n", folder);
        return;
    }
    const u64 dirs_at = rd64(head + 0x18), dirs_size = rd64(head + 0x20), files_at = rd64(head + 0x38), files_size = rd64(head + 0x40), data_at = rd64(head + 0x48);
    if (dirs_size > (64u << 20) || files_size > (256u << 20) || dirs_at + dirs_size > (u64)im->size || files_at + files_size > (u64)im->size) return;
    u8 *dirs = malloc(dirs_size ? dirs_size : 1), *files = malloc(files_size ? files_size : 1);
    mods.files = malloc(sizeof(Mod) * MODS_MOST);
    if (!dirs || !files || !mods.files || rom_read(im, dirs_at, dirs, dirs_size, &got) || got != dirs_size || rom_read(im, files_at, files, files_size, &got) ||
        got != files_size) {
        nx2_log("[mods] the game's tables of files (%u KB) couldn't be read into memory: nothing is laid over the image\n", (u64)((dirs_size + files_size) >> 10));
        free(dirs), free(files), free(mods.files), mods.files = NULL;
        return;
    }
    // the folder, and the folders in it, one after another: each one's path (under the folder, "" for itself) waits in mods.paths
    u64 end = ((u64)im->size + 15) & ~15ull;
    u32 missing = 0, said = 0;
    static FsDirectoryEntry entries[8];
    static char full[FS_MAX_PATH], rel[FS_MAX_PATH];
    u32 first = mods_keep(""), waiting = first, waiting_end = mods.paths_used;
    while (waiting != ~0u && waiting < waiting_end) {
        // (the paths of files found go after the folders', in the same room: a folder's is copied out before the room may move)
        u32 rel_len = 0;
        for (const char *p = mods.paths + waiting; *p; p++) rel[rel_len++] = *p;
        rel[rel_len] = 0;
        waiting += rel_len + 1;
        u32 n = 0;
        for (const char *p = folder; *p; p++) full[n++] = *p;
        if (rel_len) full[n++] = '/';
        for (u32 k = 0; k < rel_len; k++) full[n++] = rel[k];
        full[n] = 0;
        FsDir d;
        if (R_FAILED(fsFsOpenDirectory(sd, full, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d))) continue;
        for (;;) {
            s64 count = 0;
            if (R_FAILED(fsDirRead(&d, &count, 8, entries)) || count <= 0) break;
            for (s64 e = 0; e < count; e++) {
                u32 len = rel_len;
                if (len) rel[len++] = '/';
                for (const char *p = entries[e].name; *p && len < sizeof rel - 1; p++) rel[len++] = *p;
                rel[len] = 0;
                if (entries[e].type == FsDirEntryType_Dir) {
                    // (a folder met: its path waits its turn, after the others'. Folders are all met before any file's path is
                    // kept, level by level, only if files' paths were kept elsewhere: so a file's path is kept in a second pass)
                    const u32 at = mods_keep(rel);
                    if (at != ~0u) waiting_end = mods.paths_used;
                } else if (mods.count < MODS_MOST) {
                    const u32 f = mods_find(dirs, dirs_size, files, files_size, rel);
                    if (f == ~0u) {
                        if (missing++ < 12) nx2_log("[mods] %s: the game has no file of that path: left out\n", rel);
                    } else {
                        // (its path under the port's folder is kept past the folders' still waiting: waiting_end moves with it,
                        // and a path that isn't a folder's is told apart by its first character, which no folder's has)
                        static char kept[FS_MAX_PATH];
                        u32 k = 0;
                        kept[k++] = '*';
                        for (u32 q = 0; q < len && k < sizeof kept - 1; q++) kept[k++] = rel[q];
                        kept[k] = 0;
                        const u32 at = mods_keep(kept);
                        if (at != ~0u) {
                            waiting_end = mods.paths_used;
                            Mod *m = &mods.files[mods.count++];
                            m->at = end, m->size = (u64)entries[e].file_size, m->path = at + 1, m->read = 0;
                            const u64 where = end - data_at;
                            for (u32 b = 0; b < 8; b++) files[f + 8 + b] = (u8)(where >> (8 * b)), files[f + 0x10 + b] = (u8)(m->size >> (8 * b));
                            end = (end + m->size + 15) & ~15ull;
                            if (said++ < 20) nx2_log("[mods] %s: %u bytes, in place of the game's own\n", rel, m->size);
                        }
                    }
                }
                rel[rel_len] = 0;
            }
        }
        fsDirClose(&d);
        // (what waits next may be a file's kept path: those are passed over)
        while (waiting < waiting_end && mods.paths[waiting] == '*') {
            while (mods.paths[waiting]) waiting++;
            waiting++;
        }
    }
    free(dirs);
    if (!mods.count) {
        nx2_log("[mods] sdmc:%s: nothing in it that the game has (%u files left out): the game's files are as they were\n", folder, (u64)missing);
        free(files), free(mods.files), mods.files = NULL;
        return;
    }
    mods.table = files, mods.table_at = files_at, mods.table_size = files_size, mods.size = end, mods.on = 1;
    nx2_log("[mods] %u files of sdmc:%s are laid over the game's own (%u left out, which the game hasn't); the storage is %u MB now\n", (u64)mods.count, folder,
            (u64)missing, mods.size >> 20);
}
// size bytes of a loose file at offset, into memory of the game's
static u32 mods_file_read(u32 which, u64 offset, u8 *to, u64 size, u64 *got) {
    static const char home[] = NX2_HOME "/mods/";
    static char path[FS_MAX_PATH];
    u32 rc = 0;
    *got = 0;
    while (__atomic_exchange_n(&mods.lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
    int slot = -1, oldest = -1;
    for (int k = 0; k < MODS_OPEN; k++)
        if (mods.kept[k].open && mods.kept[k].which == which) slot = k;
    for (int k = 0; k < MODS_OPEN && slot < 0; k++)  // (one not in use, or else the one read longest ago)
        if (!mods.kept[k].open) {
            oldest = k;
            break;
        } else if (oldest < 0 || mods.kept[k].age < mods.kept[oldest].age) {
            oldest = k;
        }
    if (slot < 0) {
        slot = oldest;
        if (mods.kept[slot].open) fsFileClose(&mods.kept[slot].file), mods.kept[slot].open = 0;
        u32 n = 0;
        for (const char *p = home; *p; p++) path[n++] = *p;
        for (const char *p = mods.paths + mods.files[which].path; *p && n < sizeof path - 1; p++) path[n++] = *p;
        path[n] = 0;
        FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
        rc = sd ? fsFsOpenFile(sd, path, FsOpenMode_Read, &mods.kept[slot].file) : 0x2EE202;
        if (!rc) mods.kept[slot].open = 1, mods.kept[slot].which = which;
    }
    if (!rc && !mods.files[which].read) {
        mods.files[which].read = 1;
        nx2_log("[mods] %s: the game reads it now, for the first time this run\n", mods.paths + mods.files[which].path);
    }
    if (!rc) {
        mods.kept[slot].age = ++mods.clock;
        u64 n = 0;
        rc = fsFileRead(&mods.kept[slot].file, (s64)offset, to, size, FsReadOption_None, &n);
        *got = n;
    }
    __atomic_store_n(&mods.lock, 0, __ATOMIC_RELEASE);
    return rc;
}
// size bytes of the storage as the game is given it, at offset (in range): the image's own, but for its table of files, which is
// the copy with the entries changed, and what lies past the image's end, which is the loose files'
static u32 mods_read(Image *im, u64 offset, u8 *to, u64 size, u64 *got) {
    *got = 0;
    while (size) {
        u64 take = size, n = 0;
        u32 rc = 0;
        if (offset >= (u64)im->size) {
            // (which loose file: the last that begins at or before the place)
            u32 lo = 0, hi = mods.count;
            while (lo + 1 < hi) {
                const u32 mid = (lo + hi) / 2;
                if (mods.files[mid].at <= offset) lo = mid;
                else hi = mid;
            }
            const Mod *m = &mods.files[lo];
            if (offset >= m->at && offset < m->at + m->size) {
                if (take > m->at + m->size - offset) take = m->at + m->size - offset;
                rc = mods_file_read(lo, offset - m->at, to, take, &n);
            } else {  // between two files, or past the last: nothing
                const u64 next = lo + 1 < mods.count && mods.files[lo + 1].at > offset ? mods.files[lo + 1].at : mods.size;
                if (offset < m->at) take = take > m->at - offset ? m->at - offset : take;
                else if (take > next - offset) take = next - offset;
                for (u64 k = 0; k < take; k++) to[k] = 0;
                n = take;
            }
        } else if (offset >= mods.table_at && offset < mods.table_at + mods.table_size) {
            if (take > mods.table_at + mods.table_size - offset) take = mods.table_at + mods.table_size - offset;
            const u8 *from = mods.table + (offset - mods.table_at);
            for (u64 k = 0; k < take; k++) to[k] = from[k];
            n = take;
        } else {
            const u64 stop = offset < mods.table_at ? mods.table_at : (u64)im->size;
            if (take > stop - offset) take = stop - offset;
            rc = rom_read(im, offset, to, take, &n);
        }
        *got += n;
        if (rc) return rc;
        if (n != take) return 0;
        offset += take, to += take, size -= take;
    }
    return 0;
}

static u32 buf_mode;  // (how the last receive buffer looked at was to be mapped: 0 as usual, 1 non-secure allowed, 3 non-device)
static const u8 *data_of(const u8 *m, u64 *buf, u64 *buf_size) {
    const u32 w0 = rd32(m), w1 = rd32(m + 4);
    u32 o = 8;
    if (w1 >> 31) {
        const u32 s = rd32(m + o);
        o += 4 + ((s & 1) ? 8 : 0) + ((s >> 1 & 15) + (s >> 5 & 15)) * 4;
    }
    o += (w0 >> 16 & 15) * 8 + (w0 >> 20 & 15) * 12;
    *buf = *buf_size = 0;
    if (w0 >> 24 & 15) {
        const u32 size_lo = rd32(m + o), addr_lo = rd32(m + o + 4), hi = rd32(m + o + 8);
        *buf = addr_lo | (u64)(hi >> 28 & 15) << 32 | (u64)(hi >> 2 & 7) << 36;
        *buf_size = size_lo | (u64)(hi >> 24 & 15) << 32;
        buf_mode = hi & 3;
    }
    o += ((w0 >> 24 & 15) + (w0 >> 28 & 15)) * 12;
    return m + ((o + 15) & ~15u);
}

// an answer in a domain: the domain's header, the result, the data, the object handed over (0: none)
static void answer(u8 *m, u32 result, const u8 *raw, u32 raw_size, u32 object) {
    for (u32 k = 0; k < 0x100; k++) m[k] = 0;
    u8 *d = m + 0x10;
    wr32(d, object ? 1 : 0);
    d += 16;
    wr32(d, 0x4F434653), wr32(d + 8, result);  // "SFCO"
    d += 16;
    for (u32 k = 0; k < raw_size; k++) d[k] = raw[k];
    d += raw_size;
    if (object) wr32(d, object), d += 4;
    wr32(m + 4, (u32)(d - m - 8 + 8 + 3) / 4);
}

// A message to a session of the file system service's: 1 if it was answered here (in the message buffer m).
int nx2_rom_ipc(u8 *m) {
    const u32 type = rd32(m) & 0xFFFF;
    if (!images[0].ok || (type != 4 && type != 6)) return 0;  // (a request, with or without a context: not a control message)
    u64 buf, buf_size;
    const u8 *d = data_of(m, &buf, &buf_size);
    if (d + 32 > m + 0x100) return 0;
    const u32 kind = d[0], object = rd32(d + 4);
    const int has_cmd = rd32(d + 16) == 0x49434653;  // "SFCI"
    const u32 cmd = has_cmd ? rd32(d + 24) : ~0u;
    if (object < ROM_OBJECT || object >= ROM_OBJECT + image_count) {
        if (kind != 1 || !has_cmd) return 0;
        if (cmd == 200) {
            nx2_log("[rom] the SDK asks for the process's data storage: given the file on the card\n");
            answer(m, 0, NULL, 0, ROM_OBJECT);
            return 1;
        }
        if (cmd == 202 && image_count > 1 && d + 48 <= m + 0x100) {  // OpenDataStorageByDataId(storage, data id)
            const u64 id = rd64(d + 40);
            if (id >> 12 == 0x0100000000000ull) return 0;  // (the system's own data: its fonts and the like)
            for (u32 k = 1; k < image_count; k++)
                if (images[k].index == (id & 0xFFF)) {
                    nx2_log("[rom] the SDK asks for data %x: add-on content %u, given the file on the card\n", id, (u64)images[k].index);
                    answer(m, 0, NULL, 0, ROM_OBJECT + k);
                    return 1;
                }
            nx2_log("[rom] the SDK asks for data %x: no add-on content of that index is on the card\n", id);
        }
        return 0;
    }
    Image *im = &images[object - ROM_OBJECT];
    if (kind == 2) {  // the object closed
        answer(m, 0, NULL, 0, 0);
        return 1;
    }
    if (cmd == 0) {  // Read(offset, size) into the buffer
        const u64 offset = rd64(d + 32), size = rd64(d + 40);
        u32 rc = 0;
        u64 got = 0;
        const int laid = mods.on && im == &images[0];  // (<folder>/mods: the game's own image, with files of the user's over it)
        const u64 whole = laid ? mods.size : (u64)im->size;
        if (size > buf_size || offset > whole || size > whole - offset) rc = 0x2EE202;  // out of range (fs)
        else if (size) rc = laid ? mods_read(im, offset, (u8 *)buf, size, &got) : rom_read(im, offset, (u8 *)buf, size, &got);
        if (rc || got != size || rom_logged < 12 || nx2_ipclog) {  // (<folder>/ipclog: every read)
            rom_logged++;
            // (with every read said, a sum of what arrived too: checked against the files on a computer, tools/bench/romsum.py)
            u64 s1 = 0, s2 = 0;
            if (nx2_ipclog && !rc)
                for (u64 k = 0; k < got; k++) s1 += ((const u8 *)buf)[k], s2 += s1;
            nx2_log("[rom] read %x bytes at %x: %x, %x read, sum %x %x, to %x (the game's buffer: mode %u)\n", size, offset, (u64)rc, got, s1, s2, buf, (u64)buf_mode);
        }
        answer(m, rc ? rc : (got != size ? 0x2EE202 : 0), NULL, 0, 0);
        return 1;
    }
    if (cmd == 4) {  // GetSize
        u8 out[8];
        const u64 whole = mods.on && im == &images[0] ? mods.size : (u64)im->size;
        wr32(out, (u32)whole), wr32(out + 4, (u32)(whole >> 32));
        answer(m, 0, out, 8, 0);
        return 1;
    }
    nx2_log("[rom] command %u of the storage isn't handled: answered as done\n", (u64)cmd);
    static const u8 zeros[0x40];
    answer(m, 0, zeros, 0x40, 0);
    return 1;
}

// A message to the add-on content service (aoc:u): 1 if it was answered here. The system would count the add-ons of the program this
// was started over; the game is told of the ones on the card instead. Asked only when there are some: without any, the service's
// own answers stand. (Its other commands, the event of a change among them, are the service's still.)
int nx2_aoc_ipc(u8 *m) {
    const u32 type = rd32(m) & 0xFFFF;
    if (image_count < 2 || (type != 4 && type != 6)) return 0;
    u64 buf, buf_size;
    const u8 *d = data_of(m, &buf, &buf_size);
    if (d + 32 > m + 0x100) return 0;
    const int domain = rd32(d) != 0x49434653 && rd32(d + 16) == 0x49434653;
    const u8 *q = domain ? d + 16 : d;  // "SFCI", the command, then its data
    if (rd32(q) != 0x49434653 || (domain && d[0] != 1)) return 0;
    const u32 cmd = rd32(q + 8), count = image_count - 1;
    u8 raw[8] = {0};
    u32 raw_size = 0;
    if (cmd == 2) {  // CountAddOnContent
        wr32(raw, count), raw_size = 4;
    } else if (cmd == 3) {  // ListAddOnContent(offset, count): the indexes, into the buffer
        const u32 from = rd32(q + 16), want = rd32(q + 20);
        u32 n = 0;
        for (u32 k = from; k < count && n < want && (n + 1) * 4 <= buf_size; k++, n++) wr32((u8 *)buf + n * 4, images[1 + k].index);
        wr32(raw, n), raw_size = 4;
    } else if (cmd != 6 && cmd != 7 && cmd != 11 && cmd != 12 && cmd != 50) {
        // (6, 7: PrepareAddOnContent, done. 11, 12: NotifyMountAddOnContent and NotifyUnmountAddOnContent, which the SDK sends as
        // the game mounts the add-on content given from the card, and 50: CheckAddOnContentMountStatus, which it asks before it
        // opens a user. The console's own service knows nothing of content mounted in the name of the title this was started
        // over: it answered 50 with 320a6 and the SDK ended the game (2166-0400) as it opened its user. So none of the three
        // reaches it: they have no data to give back. nx2-relink's issue 25, whose finding and change this is.)
        return 0;
    }
    nx2_log("[aoc] command %u: answered from the card (%u add-on content there)\n", (u64)cmd, (u64)count);
    if (domain) {
        answer(m, 0, raw, raw_size, 0);
        return 1;
    }
    for (u32 k = 0; k < 0x100; k++) m[k] = 0;
    wr32(m + 4, (16 + 16 + raw_size + 3) / 4);
    wr32(m + 0x10, 0x4F434653);  // "SFCO", then the result (0)
    for (u32 k = 0; k < raw_size; k++) m[0x20 + k] = raw[k];
    return 1;
}

// The game's saved data, kept in a folder of the card: <folder>/save. The SDK asks the file system service for the save data
// of the running program, which is the program this was started over: the game then writes in another game's save, and a game, which empties its save's folder first, was ended at its first write there (the SDK's abort, result c9f46402;
// log, 2026-11-22). So that request is sent as one for the card's file system instead, and every path the SDK then gives that file
// system has the folder put before it. A request to it that can't be read here is refused, never passed on as it is: it would act on
// the card's root.
#define SAVE_DIR NX2_HOME "/save"
#define CACHE_DIR NX2_HOME "/cache"
#define SAVE_UP NX2_HOME
#define SAVE_OBJECTS 8
static u32 save_objects[SAVE_OBJECTS];
static u32 save_logged;

// On a Switch 2 the file system service refuses the SDK's session the card's file system (result 6400, "permission denied", at
// A game's first mount of its saved data; log, 2026-11-23), though the host library has one, from its own start. So
// there the save data's file system is the host's: the game is given an object of the port's own making, and each request to it,
// and to the files and folders opened through it, is sent on the host's session with the host's object in place of the port's.
// The objects the host's session hands back are the host's too: each is given a number of the port's before the game sees it.
#define LENT_OBJECT 0x4E585300u
#define LENT 96
static struct {
    u32 real;  // the object in the host's session (0: a free place)
    u32 fs;    // 0: file or directory; 1: save filesystem; 2 and up: cache filesystem by index
} lent[LENT];
static u32 lent_lock;
static FsFileSystem *host_fs;  // the host's card file system, when its session is a domain

static u32 lend(u32 real, u32 fs) {
    while (__atomic_exchange_n(&lent_lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
    u32 id = 0;
    for (u32 k = 0; k < LENT && !id; k++)
        if (!lent[k].real) lent[k].real = real, lent[k].fs = fs, id = LENT_OBJECT + k;
    __atomic_store_n(&lent_lock, 0, __ATOMIC_RELEASE);
    return id;
}

static void cache_folder(char path[sizeof CACHE_DIR + 5], u16 index) {
    static const char hex[] = "0123456789abcdef";
    const size_t at = sizeof CACHE_DIR;
    memcpy(path, CACHE_DIR "/", at);
    for (unsigned i = 0; i < 4; i++) path[at + i] = hex[(index >> (12 - 4 * i)) & 15];
    path[at + 4] = 0;
}

static u32 ensure_cache_directory(u16 index) {
    char root[FS_MAX_PATH] = CACHE_DIR;
    char path[FS_MAX_PATH] = {0};
    cache_folder(path, index);
    FsDirEntryType type;
    if (R_SUCCEEDED(fsFsGetEntryType(host_fs, path, &type)))
        return type == FsDirEntryType_Dir ? 0 : 0x402;
    u32 rc = fsFsCreateDirectory(host_fs, root);
    if (rc && rc != 0x402) return rc;
    rc = fsFsCreateDirectory(host_fs, path);
    if (rc == 0x402 && R_SUCCEEDED(fsFsGetEntryType(host_fs, path, &type)) && type == FsDirEntryType_Dir)
        rc = 0;
    nx2_log("[save] creating cache index %u in %s: result %x\n", (u64)index, path, (u64)rc);
    return rc;
}

u32 nx2_create_cache_storage(s32 index, s64 data_size, s64 journal_size) {
    if (!host_fs || index < 0 || index > UINT16_MAX || data_size < 0 || journal_size < 0) return 0x2EE202;
    return ensure_cache_directory((u16)index);
}

int nx2_has_sd_save_fs(void) { return host_fs != NULL; }

void nx2_save_open(void) {
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    if (!sd) return;
    static char up[FS_MAX_PATH], dir[FS_MAX_PATH];
    strncpy(up, SAVE_UP, sizeof up - 1), strncpy(dir, SAVE_DIR, sizeof dir - 1);
    fsFsCreateDirectory(sd, up);
    const u32 rc = fsFsCreateDirectory(sd, dir);
    nx2_log("[save] the game's saved data is kept in sdmc:%s (made: %x)\n", dir, (u64)rc);
#ifdef __SWITCH2__
    if (sd->s.object_id) host_fs = sd;
    nx2_log("[save] the host's file system of the card: session %x, object %x: %s\n", (u64)sd->s.session, (u64)sd->s.object_id,
            host_fs ? "the game's saved data goes through it" : "not an object of a domain, so the SDK's own session is asked for the card");
#endif
}

// where a message's pointer descriptors are, and how many
static u8 *pointers_of(u8 *m, u32 *count) {
    const u32 w0 = rd32(m), w1 = rd32(m + 4);
    u32 o = 8;
    if (w1 >> 31) {
        const u32 s = rd32(m + o);
        o += 4 + ((s & 1) ? 8 : 0) + ((s >> 1 & 15) + (s >> 5 & 15)) * 4;
    }
    *count = w0 >> 16 & 15;
    return m + o;
}

// CMIF input size for a domain request with no buffers, handles, or input objects.
static u32 inline_request_size(const u8 *m) {
    const u32 header = rd32(m + 4), bytes = (header & 0x3ff) * 4;
    if (rd32(m) >> 16 || (header & 0x80003c00u) || bytes < 40 || bytes > 0x100 - 8) return 0;
    const u32 domain = rd32(m + 0x10), size = domain >> 16;
    return (domain & 0xffff) == 1 && size >= 16 && size <= bytes - 24 ? size : 0;
}

// the paths of a request to the save data's file system, put in its folder (paths: room for two). 0: a request that isn't understood
static int in_folder(u8 *m, u32 cmd, int plain, const char *folder, char (*paths)[0x301]) {
    u32 count;
    u8 *x = pointers_of(m, &count);
    int ok = plain && cmd <= 15 && count <= 2 && (count > 0 || cmd == 10);  // Commit has no path.
    if (cmd == 16) ok = plain && inline_request_size(m) == 16;  // GetFileSystemAttribute
    for (u32 k = 0; ok && k < count; k++) {
        const u32 w = rd32(x + k * 8);
        const char *from = (const char *)(rd32(x + k * 8 + 4) | (u64)(w >> 12 & 15) << 32 | (u64)(w >> 6 & 7) << 36);
        const u32 size = w >> 16;
        char *to = paths[k];
        u32 n = 0;
        for (const char *p = folder; *p; p++) to[n++] = *p;
        if (!from || !size || from[0] != '/') ok = 0;
        for (u32 i = 0; ok && i < size && from[i]; i++) {
            if (from[i] == '.' && i + 1 < size && from[i + 1] == '.') ok = 0;  // (nothing above the folder)
            if (n >= 0x300) ok = 0;
            else to[n++] = from[i];
        }
        to[n] = 0;
        if (!ok) break;
        if (save_logged < 24) save_logged++, nx2_log("[save] command %u, %s\n", (u64)cmd, to);
        wr32(x + k * 8, (w & 0xE3F) | (u32)((u64)to >> 32 & 15) << 12 | (u32)((u64)to >> 36 & 7) << 6 | 0x301u << 16);
        wr32(x + k * 8 + 4, (u32)(u64)to);
    }
    if (!ok) nx2_log("[save] a request to the save data's file system that isn't understood (command %d, %u paths): refused\n", (s64)(s32)cmd, (u64)count);
    return ok;
}

// Before a message to the file system service is sent. 0: not about saved data. 1: a request for the save data's file system, now
// one for the card's (nx2_save_after reads its answer). 2: a request to that file system, its paths in the folder now (paths: room
// for two, which must last until the message is sent). 3: answered here. 5: to be sent on the host's session instead (*handle),
// and nx2_save_after given its answer.
int nx2_save_before(u8 *m, char (*paths)[0x301], u32 *handle) {
    const u32 type = rd32(m) & 0xFFFF;
    if (type != 4 && type != 6) return 0;
    u64 buf, buf_size;
    u8 *d = (u8 *)data_of(m, &buf, &buf_size);
    if (d + 32 > m + 0x100) return 0;
    const u32 kind = d[0], object = rd32(d + 4);
    const int has_cmd = rd32(d + 16) == 0x49434653;  // "SFCI"
    const u32 cmd = has_cmd ? rd32(d + 24) : ~0u;
    if (object >= LENT_OBJECT && object < LENT_OBJECT + LENT && lent[object - LENT_OBJECT].real) {  // one of the host's, lent
        const u32 k = object - LENT_OBJECT;
        if (lent[k].fs) {
            if (kind == 2) {  // the file system closed: the host's stays open
                lent[k].real = 0;
                answer(m, 0, NULL, 0, 0);
                return 3;
            }
            char cache[sizeof CACHE_DIR + 5];
            if (lent[k].fs > 1) cache_folder(cache, (u16)(lent[k].fs - 2));
            if (!in_folder(m, cmd, kind == 1 && has_cmd, lent[k].fs > 1 ? cache : SAVE_DIR, paths)) {
                answer(m, 0x2EE202, NULL, 0, 0);
                return 3;
            }
        }
        wr32(d + 4, lent[k].real);
        if (kind == 2) lent[k].real = 0;  // (a file or a folder closed: the host's object goes with this message)
        *handle = host_fs->s.session;
        return 5;
    }
    int mine = -1;
    for (int k = 0; k < SAVE_OBJECTS; k++)
        if (object && save_objects[k] == object) mine = k;
    if (mine < 0) {
        // (How large the game's "cache storage" is, command 34: a second kind of saved data, which a game mounts like the first
        // (command 51, below) and then asks the size of. The system has none for the title a port
        // is started over, says "not found" (7d402), and the game's SDK ends the program on that; report, 2026-11-25. Answered
        // here: there is as much as a game could ask, 2 GB of data and as much again of journal.)
        if (kind == 1 && has_cmd && cmd == 34) {
            u8 sizes[16] = {0};
            sizes[3] = 0x80, sizes[11] = 0x80;
            nx2_log("[save] the SDK asks how large the game's cache storage is (command 34): told of 2 GB\n");
            answer(m, 0, sizes, sizeof sizes, 0);
            return 3;
        }
        if (kind != 1 || !has_cmd || (cmd != 51 && cmd != 53)) return 0;  // OpenSaveDataFileSystem, and the read-only one
        if (host_fs) {
            u32 fs = 1;
            const u8 *attr = d + 40; // After the domain and CMIF headers and the aligned save-data space ID.
            if (inline_request_size(m) >= 24 + sizeof(FsSaveDataAttribute) &&
                attr[offsetof(FsSaveDataAttribute, save_data_type)] == FsSaveDataType_Cache) {
                const u16 index = (u16)rd32(attr + offsetof(FsSaveDataAttribute, save_data_index));
                // The size query can succeed before a game explicitly creates the cache.
                const u32 rc = ensure_cache_directory(index);
                if (rc) {
                    answer(m, rc, NULL, 0, 0);
                    return 3;
                }
                fs = 2u + index;
                nx2_log("[save] mounting SD-backed cache index %u\n", (u64)index);
            } else {
                nx2_log("[save] the SDK asks for the program's save data (command %u): given the host's file system of the card, in %s\n", (u64)cmd, SAVE_DIR);
            }
            const u32 id = lend(host_fs->s.object_id, fs);
            answer(m, id ? 0 : 0x2EE202, NULL, 0, id);
            return 3;
        }
        nx2_log("[save] the SDK asks for the program's save data (command %u): given the card's file system, in %s\n", (u64)cmd, SAVE_DIR);
        const u32 token = rd32(d + 28);
        for (u32 k = 0; k < 0x40; k++) m[k] = 0;
        wr32(m, type), wr32(m + 4, 12);
        u8 *n = m + 0x10;
        n[0] = 1, n[2] = 0x10, wr32(n + 4, object);
        wr32(n + 16, 0x49434653), wr32(n + 24, 18), wr32(n + 28, token);  // OpenSdCardFileSystem
        return 1;
    }
    if (kind == 2) {  // the file system closed
        save_objects[mine] = 0;
        return 2;
    }
    if (in_folder(m, cmd, kind == 1 && has_cmd, SAVE_DIR, paths)) return 2;
    answer(m, 0x2EE202, NULL, 0, 0);
    return 3;
}

// the answer to a request nx2_save_before changed (how: what it returned)
void nx2_save_after(u8 *m, int how) {
    u64 buf, buf_size;
    u8 *d = (u8 *)data_of(m, &buf, &buf_size);
    if (how == 5) {  // from the host's session: an object it hands back (a file, a folder) gets a number of the port's
        if (d + 36 <= m + 0x100 && rd32(d + 16) == 0x4F434653 && !rd32(d + 24) && rd32(d) == 1) {
            const u32 id = lend(rd32(d + 32), 0);
            if (!id) nx2_log("[save] too many files open at once\n");
            wr32(d + 32, id);
        }
        return;
    }
    if (d + 36 > m + 0x100 || rd32(d + 16) != 0x4F434653 || rd32(d + 24) || rd32(d) != 1) {
        nx2_log("[save] the card's file system wasn't given: %x\n", d + 28 <= m + 0x100 ? (u64)rd32(d + 24) : 0);
        return;
    }
    const u32 object = rd32(d + 32);
    for (int k = 0; k < SAVE_OBJECTS; k++)
        if (!save_objects[k]) {
            save_objects[k] = object;
            return;
        }
    nx2_log("[save] too many file systems open at once\n");
}

// (the same two for svc.c's own answers)
const u8 *nx2_ipc_data(const u8 *m, u64 *buf, u64 *buf_size) { return data_of(m, buf, buf_size); }
void nx2_ipc_answer(u8 *m, u32 result, const u8 *raw, u32 raw_size, u32 object) { answer(m, result, raw, raw_size, object); }
