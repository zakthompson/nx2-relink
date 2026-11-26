"""A Switch 2 game's dump, whichever way it is kept, read for a port: its program and its files, with its updates laid over it.

    python tools/game2.py info <dump> [--update <dump>]          what is there: the game, its updates, what a port would take
    python tools/game2.py port <dump> <out> [--update <dump>]    what a port is built from: <out>/exefs, info.json, icon.jpg
                                                                 (tools/build.py and nxrelink.py do this themselves)
    python tools/game2.py romfs <dump> <to> [--parts] [--update <dump>]
                                                                 the game's files as one RomFS image, as a port reads it: to a
                                                                 file, or to a console (ftp://host:port/nx2-relink/<name>/romfs.bin, or
                                                                 ttx://host/nx2-relink/<name>/romfs.bin with Toronja Transfer open on it)

A dump is either of:
  - a NUP container (tools/nup.py): its packages are the game, its updates and its added content;
  - a folder of loose files, in one of the shapes dumpers leave:
        <folder>/exefs, <folder>/romfs                      one of them: the game, or an update alone
        <folder>/base/exefs, <folder>/base/romfs            the game, and
        <folder>/updates/<version>/exefs, .../romfs         its updates, each in a folder named by its version's number
    (a folder that holds nothing but one folder is looked into: "Game/Game/base").
--update gives one more dump, of the same game, to be laid over: an update kept apart from the game.

How they are put together, as a Switch game's update is (tools/nsp.py): the program is the newest one's (an update carries the
whole program), and the files are the game's with each update's laid over them in the order of their versions: a file an update
has replaces the game's, and one it doesn't have stays. Nothing is written here but the program: the files go from where they are
into the image as it is sent (tools/install.py).
"""
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nup as container  # noqa: E402

CHUNK = 4 << 20


def from_disk(path):
    """A file of this computer's, as RomFS takes one: a function that gives its bytes in pieces."""
    def pieces():
        with open(path, 'rb') as f:
            while True:
                block = f.read(CHUNK)
                if not block:
                    return
                yield block
    return pieces


def version_text(v):
    """A package's version number in words. It counts the game's updates (0x10000 each), and is not the version the game shows:
    a game's 262144, its fourth update, shows "1.1.1". So it is said as what it is."""
    return f'update {v >> 16}' if v else ''


class Layer:
    """One of a dump's: the game, or an update. modules: {name: a function that gives the module's bytes} (its ExeFS; empty if it
    has none); files: {path: (size, its bytes' function)} (its RomFS; empty if it has none)."""

    def __init__(self, what, version, modules, files, where, facts=None):
        self.what, self.version, self.modules, self.files, self.where, self.facts = what, version, modules, files, where, facts or {}

    def said(self):
        return (f'{self.what}{f" {version_text(self.version)} ({self.version})" if self.version else ""}: '
                f'{f"a program of {len(self.modules)} modules" if self.modules else "no program"}, '
                f'{f"{len(self.files)} files, {sum(s for s, _ in self.files.values()) >> 20} MB" if self.files else "no files"} ({self.where})')


def folder_layer(folder, what, version):
    modules, files = {}, {}
    exefs, romfs = os.path.join(folder, 'exefs'), os.path.join(folder, 'romfs')
    if os.path.isdir(exefs):
        for name in sorted(os.listdir(exefs)):
            if os.path.isfile(os.path.join(exefs, name)):
                modules[name] = lambda p=os.path.join(exefs, name): open(p, 'rb').read()
    if os.path.isdir(romfs):
        for d, _, names in os.walk(romfs):
            for name in names:
                p = os.path.join(d, name)
                files[os.path.relpath(p, romfs).replace(os.sep, '/')] = (os.path.getsize(p), from_disk(p))
    return Layer(what, version, modules, files, folder.replace(os.sep, '/'))


def layers_of(path, as_update=False):
    """A dump's layers, and what else it has that isn't taken (added content): ([Layer], [a line about each thing left])."""
    left = []
    if os.path.isfile(path):
        box = container.Nup(path)
        out = []
        for k, p in enumerate(box.packages):
            if p['kind'] not in (0, 1):
                left.append(f'package {k} ({container.KINDS.get(p["kind"], p["kind"])}): not taken yet')
                continue
            modules, files = {}, {}
            if p['exefs'] != 0xFFFFFFFF:
                for name, i in box.walk(p['exefs']):
                    def read(i=i, name=name):
                        data = box.read(i)
                        if hashlib.sha256(data).digest() != box.file(i)[4]:
                            raise SystemExit(f'exefs/{name}: its bytes do not match the sum the container gives for them')
                        return data
                    modules[name] = read
            if p['romfs'] != 0xFFFFFFFF:
                files = container.package_files(box, k)
            said = box.facts(k)
            text = lambda n: said.get(n, b'').decode('utf-8', 'replace').strip(chr(0))  # noqa: E731
            facts = {'title': text('title') or text('dump name'), 'author': text('publisher'), 'version': text('display version'),
                     'title_id': p['title'], 'icon': said.get('icon', b'')}
            out.append(Layer('an update' if p['kind'] == 1 or as_update else 'the game', p['version'], modules, files, f'{path}, package {k}', facts))
        return out, left
    if not os.path.isdir(path):
        raise SystemExit(f'{path} isn\'t there.')
    folder = path
    for _ in range(3):  # ("Game/Game/base": a folder with nothing but one folder in it is looked into)
        inside = os.listdir(folder)
        if any(os.path.isdir(os.path.join(folder, n)) for n in ('exefs', 'romfs', 'base', 'updates')):
            break
        if len(inside) == 1 and os.path.isdir(os.path.join(folder, inside[0])):
            folder = os.path.join(folder, inside[0])
        else:
            break
    out = []
    if os.path.isdir(os.path.join(folder, 'exefs')) or os.path.isdir(os.path.join(folder, 'romfs')):
        out.append(folder_layer(folder, 'an update' if as_update else 'the game', 0))
    if os.path.isdir(os.path.join(folder, 'base')):
        out.append(folder_layer(os.path.join(folder, 'base'), 'an update' if as_update else 'the game', 0))
    updates = os.path.join(folder, 'updates')
    if os.path.isdir(updates):
        names = [n for n in os.listdir(updates) if os.path.isdir(os.path.join(updates, n))]
        for order, n in enumerate(sorted(names, key=lambda n: (int(n) if n.isdigit() else 0, n))):
            out.append(folder_layer(os.path.join(updates, n), 'an update', int(n) if n.isdigit() else order + 1))
    for n in os.listdir(folder):
        if n.lower() in ('dlc', 'dlcs', 'aoc', 'addons', 'add-ons') and os.path.isdir(os.path.join(folder, n)):
            pass  # (added content: taken by aoc_of)
    if not out:
        raise SystemExit(f'{path}: no game found there. A folder dump has exefs/ and romfs/, or base/ and updates/<version>/ with those in them.')
    for layer in out:
        layer.facts.setdefault('title', os.path.basename(os.path.normpath(path)))
    return out, left


def aoc_of(path):
    """The add-on content of a dump: [(index, {path: (size, pieces_fn)})] for each one there. The index is the lower 12 bits of its
    title's number, as the game asks for it (OpenDataStorageByDataId). From a NUP's kind-2 packages, or from a folder dump's
    dlc/ (or aoc/, addons/, add-ons/): <title id>/<version>/romfs/, the newest version of each title."""
    out = []
    if os.path.isfile(path):
        box = container.Nup(path)
        for k, p in enumerate(box.packages):
            if p['kind'] != 2 or p['romfs'] == 0xFFFFFFFF:
                continue
            index = p['title'] & 0xFFF
            out.append((index, container.package_files(box, k)))
        return out
    folder = path
    for _ in range(3):
        inside = os.listdir(folder)
        if any(os.path.isdir(os.path.join(folder, n)) for n in ('exefs', 'romfs', 'base', 'updates')):
            break
        if len(inside) == 1 and os.path.isdir(os.path.join(folder, inside[0])):
            folder = os.path.join(folder, inside[0])
        else:
            break
    for n in os.listdir(folder):
        if n.lower() not in ('dlc', 'dlcs', 'aoc', 'addons', 'add-ons') or not os.path.isdir(os.path.join(folder, n)):
            continue
        dlc_root = os.path.join(folder, n)
        for title_name in sorted(os.listdir(dlc_root)):
            title_dir = os.path.join(dlc_root, title_name)
            if not os.path.isdir(title_dir) or title_name.startswith('.'):
                continue
            try:
                index = int(title_name, 16) & 0xFFF
            except ValueError:
                continue
            # the newest version's romfs (the highest-numbered subfolder that has one, or the title folder itself)
            versions = sorted([v for v in os.listdir(title_dir) if os.path.isdir(os.path.join(title_dir, v)) and not v.startswith('.')],
                              key=lambda v: (int(v) if v.isdigit() else 0, v))
            romfs = None
            for v in reversed(versions):
                r = os.path.join(title_dir, v, 'romfs')
                if os.path.isdir(r):
                    romfs = r
                    break
            if not romfs:
                r = os.path.join(title_dir, 'romfs')
                if os.path.isdir(r):
                    romfs = r
            if not romfs:
                continue
            files = {}
            for d, _, names in os.walk(romfs):
                for name in names:
                    p = os.path.join(d, name)
                    files[os.path.relpath(p, romfs).replace(os.sep, '/')] = (os.path.getsize(p), from_disk(p))
            if files:
                out.append((index, files))
    return out


def program_id(npdm):
    """The program's number, from its main.npdm (0 if that isn't one)."""
    if len(npdm) < 0x80 or npdm[:4] != b'META':
        return 0
    at = struct.unpack_from('<I', npdm, 0x70)[0]
    return struct.unpack_from('<Q', npdm, at + 0x10)[0] if npdm[at:at + 4] == b'ACI0' and at + 0x18 <= len(npdm) else 0


class Dump:
    """A game and its updates, put together: .code is the layer whose program a port takes, .files() the files, laid over."""

    def __init__(self, path, updates=()):
        self.path, self.updates = path, list(updates)
        self.layers, self.left = layers_of(path)
        for u in self.updates:
            more, left = layers_of(u, as_update=True)
            self.layers += more
            self.left += left
        # the game first, then the updates by their versions (as they were given, where those are the same)
        self.layers.sort(key=lambda layer: (layer.what != 'the game', layer.version))
        with_code = [layer for layer in self.layers if 'main' in layer.modules]
        if not with_code:
            raise SystemExit(f'{path}: no program there (an exefs folder with "main" in it)')
        self.code = with_code[-1]
        self.aoc = sorted(aoc_of(path), key=lambda a: a[0])

    def files(self, say=None):
        merged = {}
        for layer in self.layers:
            if not layer.files:
                continue
            replaced = sum(1 for p in layer.files if p in merged)
            before = len(merged)
            merged.update(layer.files)
            if say:
                say(f'  files: {len(layer.files)} of {layer.what}{f" {version_text(layer.version)}" if layer.version else ""}'
                    + (f': {replaced} replace earlier ones, {len(merged) - before} are new, {before - replaced} earlier ones stay' if before else ''))
        return merged

    def facts(self):
        """The name, maker, version, number and icon a port is shown with: the newest layer's that says each."""
        out = {'title': '', 'author': '', 'version': '', 'title_id': 0, 'icon': b''}
        for layer in self.layers:
            for k in out:
                if layer.facts.get(k):
                    out[k] = layer.facts[k]
        out['version'] = out['version'] or version_text(self.code.version)
        if 'main.npdm' in self.code.modules:
            out['title_id'] = program_id(self.code.modules['main.npdm']()) or out['title_id']
        return out

    def said(self):
        lines = [layer.said() for layer in self.layers]
        lines.append(f'a port takes: the program of {self.code.what}{f" {version_text(self.code.version)}" if self.code.version else ""}'
                     f' ({", ".join(sorted(self.code.modules))}); the files of all of them, the later over the earlier')
        for index, files in self.aoc:
            lines.append(f'added content {index}: {len(files)} files, {sum(s for s, _ in files.values()) >> 10} KB (aoc{index}.romfs)')
        return lines + [f'left: {x}' for x in self.left]


def damage(name, data):
    """What is wrong with a module's file, in words, or None: checked against what the file says of itself (where its three parts
    end, and each part's sum) before anything is built from it.
    One kind of damage is told apart, because it has been met (2026-11-25): a dump whose files without an extension (main, sdk,
    subsdk0) had been copied as text. An FTP program in "ASCII" or "Auto" mode does that to such files: every CR LF pair in them
    becomes an LF, so the file is a little shorter than its header says, and what was packed in it can't be unpacked."""
    if data[:4] != b'NSO0' or len(data) < 0x100:
        return None  # (not a module: main.npdm)
    flags = struct.unpack_from('<I', data, 0x0C)[0]
    packed = struct.unpack_from('<3I', data, 0x60)
    parts = [struct.unpack_from('<3I', data, 0x10 + k * 0x10) for k in range(3)]
    end = max(off + n for (off, _, _), n in zip(parts, packed))
    pairs, returns = data.count(bytes([13, 10])), data.count(bytes([13]))
    as_text = (f': it has {pairs} CR LF pairs where about {returns // 256} would be, which is what copying it as text does (an FTP program in '
               f'"ASCII" or "Auto" mode, to a file with no extension). Copy the exefs folders again in binary mode'
               if returns > 2560 and pairs * 8 < returns // 256 else '')
    if len(data) < end:
        return f'exefs/{name} is damaged: {end - len(data)} bytes shorter than its own header says ({len(data)} of {end}){as_text}'
    try:
        import lz4.block
    except ImportError:
        return None  # (nothing to unpack it with here: tools/nso.py will say if it can't)
    import zbic
    for k, ((off, _, size), n) in enumerate(zip(parts, packed)):
        raw = data[off:off + n]
        try:
            if not flags >> k & 1:
                out = raw[:size]
            elif flags & 0x80 and raw[:4] == zbic.MAGIC:
                out = zbic.unpack(raw, size)
            else:
                out = lz4.block.decompress(raw, uncompressed_size=size)
        except Exception:  # noqa: BLE001
            return f'exefs/{name} is damaged: its {("code", "read-only data", "data")[k]} can\'t be unpacked{as_text}'
        if flags >> (3 + k) & 1 and hashlib.sha256(out).digest() != data[0xA0 + k * 0x20:0xC0 + k * 0x20]:
            return f'exefs/{name} is damaged: its {("code", "read-only data", "data")[k]} doesn\'t match the sum the file gives for it{as_text}'
    return None


NOT_YET = 'NOT SUPPORTED YET'  # (how a refusal that isn't a fault of the dump's begins: nxrelink.py's window looks for it)


def not_yet(name, data):
    """What of a module's file this release can't open, in words, or None. Not damage: the dump is as its console has it, and a
    later release may open it. Looked for before damage() is, which would call such a file damaged.
    Two packings are known: LZ4, and ZBIC (flag 0x80; tools/zbic.py), first met on 2026-11-25 in a game for the Switch 2 and refused as "not supported yet" until it was read. A flag beyond those is one this doesn't know."""
    if data[:4] != b'NSO0' or len(data) < 0x100:
        return None
    flags = struct.unpack_from('<I', data, 0x0C)[0]
    if flags & ~0xFF:  # (0x40 is set in the one ZBIC module met; what it says isn't known, and the module reads right without it)
        return f'exefs/{name} has packing flags this release doesn\'t know ({flags:#x})'
    if flags & 0x80:
        try:
            import zstandard  # noqa: F401
        except ImportError:
            return f'exefs/{name} is packed as "ZBIC", which needs the Python package "zstandard" to open:  python nxrelink.py setup'
    return None


def is_switch2(main):
    """Whether a program's main module is a Switch 2 game's: its image begins with a header's words there (1, then where its two
    headers are), and with an instruction in a Switch game's."""
    from nso import Nso
    path = os.path.join(os.environ.get('TEMP', '.'), f'nx2_main_{os.getpid()}')
    open(path, 'wb').write(main)
    try:
        first = struct.unpack_from('<I', bytes(Nso(path).image()[:4]))[0]
    finally:
        os.remove(path)
    return first == 1


def port(path, out, updates=()):
    """What a port is built from, written under <out> as tools/nsp.py leaves a Switch game's: exefs/ (the program's modules),
    info.json (its name, maker, version and icon, and that it is a Switch 2 game: where its dump is, and its updates') and icon.jpg.
    Its files are not written here: tools/install.py makes their image from the dump as it sends it."""
    dump = Dump(path, updates)
    for line in dump.said():
        print(line)
    modules = {name: read() for name, read in dump.code.modules.items()}
    later = [w for w in (not_yet(name, data) for name, data in sorted(modules.items()) if name != 'rtld') if w]  # (rtld isn't used)
    if later:
        raise SystemExit(f'{NOT_YET}: this game can\'t be made into a port by this release.\n  ' + '\n  '.join(later) +
                         '\n  Nothing is wrong with your dump, and nothing was written. A later release may open it: see the tool\'s page for '
                         'the games that work.')
    wrong = [w for w in (damage(name, data) for name, data in sorted(modules.items()) if name != 'rtld') if w]
    if wrong:
        raise SystemExit('The program can\'t be used as it is:\n  ' + '\n  '.join(wrong))
    if not is_switch2(modules['main']):
        raise SystemExit(f'{path}: its program is a Switch game\'s, which this doesn\'t take: a Switch game is taken from its NSP (nx-relink is the tool for those)')
    os.makedirs(os.path.join(out, 'exefs'), exist_ok=True)
    for old in os.listdir(os.path.join(out, 'exefs')):
        os.remove(os.path.join(out, 'exefs', old))
    for name, data in modules.items():
        with open(os.path.join(out, 'exefs', name), 'wb') as o:
            o.write(data)
    facts = dump.facts()
    count = len(dump.files(print))
    # a Switch 2 game: its port is for a Switch 2 alone, has the game's own SDK drive the display and the graphics, and is given
    # this many MB for the game's heap (tools/build.py)
    # What the game is to be told of memory (tools/heapdef.py): 7800 MB, a little less than a port has, for a game that takes all
    # it is told of; the console's own figure for one that keeps a stretch of address space apart from its heap, which its own
    # table of heaps says. "apart" is that stretch: the runtime tells of no more than what there is and that together.
    import heapdef
    kept_apart = heapdef.apart(dump.files())
    if kept_apart:
        print(f'  the game\'s own table of heaps keeps {kept_apart} MB of address space apart from its heap: it is told of the console\'s '
              f'{heapdef.CONSOLE} MB, as on its console')
    info = {'title': facts['title'], 'author': facts['author'], 'version': facts['version'], 'title_id': f'{facts["title_id"]:016x}',
            'switch2': True, 'heap': heapdef.CONSOLE if kept_apart else 7800, 'apart': kept_apart or 0,
            'dump': os.path.abspath(path).replace(os.sep, '/'),
            'updates': [os.path.abspath(u).replace(os.sep, '/') for u in updates]}
    if facts['icon'][:2] == bytes([0xFF, 0xD8]):
        import icon as icons
        open(os.path.join(out, 'icon.jpg'), 'wb').write(icons.fit(facts['icon']))
        info['icon'] = 'icon.jpg'
    # (a name, version or icon given since by hand or taken from the console, tools/control.py: kept, where the dump has none)
    before = os.path.join(out, 'info.json')
    if os.path.isfile(before):
        old = json.load(open(before, encoding='utf-8'))
        if old.get('title_id') == info['title_id']:
            if not facts['icon'] and old.get('icon') and os.path.isfile(os.path.join(out, old['icon'])):
                info['icon'] = old['icon']
            if old.get('identity'):
                info.update({k: old[k] for k in ('title', 'author', 'version', 'identity') if k in old})
    json.dump(info, open(os.path.join(out, 'info.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print(f'{out}: {len(modules)} modules, "{info["title"]}" {info["version"]}{" by " + info["author"] if info["author"] else ""}, program {info["title_id"]}'
          f'{"; icon.jpg" if "icon" in info else "; no icon"}; {count} files to go on the card')
    if 'icon' not in info:
        print('  The dump has no name or icon of the game\'s own: the port shows what is above. To give it the game\'s:\n'
              '    python nxrelink.py identity <name> --ftp <host>                      from a console that has the game (two steps)\n'
              '    python nxrelink.py identity <name> --title "..." --version "..." --icon <picture>       by hand')
    return info


def image_of(info, say=None):
    """The RomFS image of the game a port's info.json tells of (tools/install.py), or None if it has no files."""
    dump = Dump(info.get('dump') or info['nup'], info.get('updates') or ())
    files = dump.files(say)
    return container.RomFS(files) if files else None


def main():
    args = sys.argv[1:]
    updates = []
    while '--update' in args:
        i = args.index('--update')
        updates.append(args[i + 1])
        del args[i:i + 2]
    parts = '--parts' in args
    if parts:
        args.remove('--parts')
    if len(args) < 2 or args[0] not in ('info', 'port', 'romfs') or (args[0] != 'info' and len(args) < 3):
        sys.exit(__doc__)
    if args[0] == 'port':
        return port(args[1], args[2], updates)
    dump = Dump(args[1], updates)
    if args[0] == 'info':
        for line in dump.said():
            print(line)
        dump.files(print)
        facts = dump.facts()
        print(f'"{facts["title"]}" {facts["version"]}, program {facts["title_id"]:016x}')
        return None
    files = dump.files(print)
    if not files:
        sys.exit('no files there (no romfs folder)')
    ok = container.send_image(container.RomFS(files), args[2], parts)
    for index, aoc_files in dump.aoc:
        dest = args[2]
        # romfs.bin → aoc<n>.romfs (the same folder, whether a path, ftp:// or ttx://)
        slash = dest.rfind('/')
        dest = (dest[:slash + 1] if slash >= 0 else '') + f'aoc{index}.romfs'
        ok = container.send_image(container.RomFS(aoc_files), dest, False) and ok
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
