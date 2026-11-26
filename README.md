# nx2-relink

Makes a **Nintendo Switch 2** game you own into a homebrew program (`.nro`) that runs it on a **Switch 2**: the game's own code,
relinked, not emulated.

A fork of [nx-relink](https://github.com/Olmectron/nx-relink), which does the same for Switch games on a Switch.

> [!CAUTION]
> **Experimental software. Use it at your own risk.**
> This runs unofficial code on your console. Games can crash or freeze, saved data can be lost or corrupted, files on your SD
> card can be damaged, and running homebrew at all can have consequences with the console's maker, a ban included. It is provided
> "as is", with no warranty of any kind. **The authors take no responsibility** for any damage or loss: to your console, your card,
> your data or your accounts. If that isn't acceptable to you, don't use it.

> [!IMPORTANT]
> **Use your own backups only.**
> This project contains no games, no keys and no Nintendo files, and it won't get them for you. It works only with dumps of games
> you own, made by you from your own console. Nothing leaves your computer or your card. Don't ask for game files here, and don't
> post them: such issues are closed.

> [!TIP]
> **A game crashed or didn't start? There is a file for that.**
> After a run that ended badly, the port writes **`crash.txt`** in its own folder of the SD card:
>
> **`/nx2-relink/<name>/crash.txt`**  (`<name>` is the port's short name)
>
> It says what happened (with the console's own error code, when it gave one) and has the end of that run's log. **Open an issue
> and attach that file** (and `log.txt`, beside it), with the game's name and version: that is everything needed to look into it.
> It is plain text you can read before sending, and has no account or serial number in it.

<p align="center"><img src="docs/window.png" alt="The nx2-relink window: choose the game, the card, and press Build and install" width="760"></p>

## What it does

A Switch 2 game is already code for the Switch 2's processor, and its own SDK already drives that console's display, graphics
processor and sound. Nothing is translated: the game's modules run as they are, and what surrounds them is replaced.

| Part of the game | What happens to it |
|---|---|
| Code (`main`, `sdk`, `subsdk*`) | Kept whole and mapped by the console's loader, or relinked into the `.nro` (the tool decides which, by the game) |
| `rtld`, the console's loader | Replaced by the port's own |
| System calls | Pass through the port first, which answers them or lets them through |
| The game's files (RomFS) | Served from the SD card |
| Saved data | A folder of the SD card |
| Mods | Optional: loose files in a folder of the SD card, laid over the game's own when it starts |
| Memory | Left to the console: the game takes it as it asks for it |
| Graphics and sound | The game's own SDK, on the console's real drivers |

## Compatibility

No list of games is kept here. This is an early release, and nothing in the tool is written for a particular game: what differs
from one game to the next is read from the game itself, so a game that was never tried has a fair chance.

**How it was tried.** A few commercial Switch 2 games, from NUP dumps with their updates, built with this tool and run on a
Switch 2 with **kaiHEN 20261117.1**: games of SDK 20 and of SDK 22, some from their menus into a new game (one of them saving
and loading a saved game), others to their title and first screens.

> [!NOTE]
> **Compatibility has been tried with kaiHEN 20261117.1 only.** A port leans on things that are kaiHEN's own (its loader
> service, the memory it leaves a program), which another version may do differently. If you use another version, say which in
> your report.

**What is decided by the game, with nothing to set:**

- **Packing.** A module packed as LZ4 or as `ZBIC` (system 22 and later) is read either way.
- **Modules.** A game that loads none of its own as it runs has all its modules kept whole, as on its console. One that loads
  some (through `nn::ro`; one of those tried loads 36) has only the modules that must be whole kept so, to leave the loader
  room for the rest. Both kinds were tried.
- **Memory.** No amount is set aside or guessed for a game: it takes memory from the console as it asks for it. A game whose
  own table of heaps keeps part of the console's memory apart from its heap is told of the console's full figure, as it sizes
  itself by it; any other is told of what the port really has.
- **Updates.** The newest program, and each update's files over the game's.

**Not every game will run.** When one doesn't, the port leaves `crash.txt` in its folder on the card
(`/nx2-relink/<name>/crash.txt`): open an issue with that file, the game's name and version, and your kaiHEN version.

**The tool tells you when it can't take a game.** Before building, it checks every module of the game's program:

- A packing it doesn't know: `NOT SUPPORTED YET`, with the module's name. Nothing is wrong with your dump; a later release
  may open it. (LZ4 and `ZBIC`, the two met so far, are both read.)
- A damaged dump: which file, and how. One case is named outright: a folder copied over FTP in "ASCII" or "Auto" mode arrives
  damaged in the files that have no extension (`main`, `sdk`, `subsdk0`). Copy the `exefs` folders again in **binary** mode.
- A Switch game's program: that is nx-relink's.

## What you need

- A Switch 2 that runs homebrew. This was made and tried with **kaiHEN 20261117.1**, and with no other version: the homebrew
  menu started over a game (hold R while starting it). A port needs the whole of the console's memory and kaiHEN's loader
  service.
- Your game's dump: a **NUP** container, or a **folder** of its loose files. No keys are needed: these dumps aren't encrypted.
- A computer with Python 3.10 or newer, [devkitPro](https://devkitpro.org/wiki/Getting_Started) (devkitA64 and switch-tools,
  from the `switch-dev` group), and [ounce-devkit](https://github.com/ouncebrew/ounce-devkit) installed into it.
- A way to put files on the console's card: the card in your computer (the fastest, for a game's gigabytes), or an FTP server
  on the console (port 5000).

> [!IMPORTANT]
> **Which game you start the homebrew menu over matters.** To the console, a port *is* that game, and it has what that game's
> own description allows, not what its own game was made for. Three such things are known:
>
> - the **memory for graphics** the console keeps for the program;
> - its **system resource**: without any, a program can't map memory as it runs, which some games do from their start;
> - the **system services** it may open.
>
> **Services are stood in for.** When the console refuses the host game a service that the port's game asks for, and the
> service is one a game can run without (the network and its sockets, online data, amiibo, friends, parental controls, play
> reports and the like), the port answers in its place: every request is "done", with nothing in the answer, and the network
> is "down". What the game does with that service won't work; the game itself goes on. `log.txt` has a line for each:
> `THE PORT STANDS IN FOR "<service>"`. This only happens when a service is refused: with a host game that has it, the game
> talks to the console's own.
>
> **What can't be stood in for:** the services of the graphics processor, the display, the controllers and the game's files,
> and the two memory figures above. A port whose game needs one of those and whose host game lacks it ends at its start, or as
> it runs, and its report says which. Games made with an older graphics path (some "Switch 2 Edition" releases) need a host
> game of the same kind.
>
> **Start the homebrew menu over the same game the port was made from, whenever you can**: it has exactly what the game
> needs. Otherwise try another game, a large and recent one for choice.
>
> `log.txt` (and so `crash.txt`) says near its start what the host game had and what the port's game was made for, and names
> a service the console refused. The tool says the game's own figures when it builds a port.

Then, in this folder:

```bash
python nxrelink.py setup
```

checks all of the above, installs the Python packages that are missing (`lz4`, `zstandard`, `Pillow`), and says where to get
whatever it can't install itself.

## Use

With a window:

```bash
python nxrelink.py
```

Choose the game, the card, and press **Build and install**. The bottom of the window always shows the step, a bar, and in long
copies the speed and the time left.

From a terminal, everything in one go:

```bash
python nxrelink.py make game.nup --name mygame --sd E:
python nxrelink.py make game.nup --name mygame --ftp 192.168.0.100:5000
python nxrelink.py make "dump folder" --name mygame --update "update folder or nup" --ftp 192.168.0.100:5000
```

| Command | What it does |
|---|---|
| `setup` | Checks what the tool needs; installs the Python packages that are missing |
| `make <game.nup \| folder>` | Checks the dump and builds the port; with `--sd` or `--ftp`, installs too |
| `install <name>` | Puts a port already built on a card |
| `identity <name>` | Gives a port the name, version and icon the homebrew menu shows |
| `remake [<name> ...]` | Builds every port again (or the ones named) with this version of the tool, from what was kept of each: no dump is needed. With `--sd` or `--ftp`, copies each one's program to the card. **For after you update the tool**; the window has it too, in **My ports** |
| `list` | The ports already built |

Options of `make`:

| Option | For |
|---|---|
| `--update <nup \| folder>` | An update kept apart from the game, laid over it |
| `--name <name>` | The port's short name: its folder on the card |
| `--title`, `--version`, `--icon <picture>` | What the homebrew menu shows; by default, the game's own, if the dump has them |
| `--sd <drive>` or `--ftp <host>` | Installs on the card in this computer, or over the network |
| `--nro-only` | Only the program (`.nro`) is copied: the game's files are on the card already. For a port built again |
| `--sd-format auto \| fat32 \| exfat` | On FAT32 a RomFS of 4 GB or more goes in parts of 2 GB; on exFAT, in one file |

Every file copied is read back from the card and compared with what was sent. The game's files aren't extracted to the
computer: their image is made from the dump as it is sent.

### Dumps and updates

A folder is read in any of these shapes (a folder that holds only another folder is opened by itself):

| Folder | What it is |
|---|---|
| `exefs/`, `romfs/` | One dump: the game, or an update |
| `base/exefs`, `base/romfs` | The game |
| `updates/<version>/exefs`, `.../romfs` | Its updates, each in a folder named by its version number |

Updates are applied as the console applies them: the program is the newest one's, and the files are the game's with each
update's over them, in order of version. The same goes for the packages inside a `.nup`. Add-on content (DLC) isn't installed yet.

`python tools/game2.py info <dump>` says what a dump has and what a port would take from it, without building anything.

### Name, version and icon

They are taken from the dump when it has them. When it hasn't (older `.nup` files, folders), the port shows the dump's file name
and no icon. Give it whatever you like:

```bash
python nxrelink.py identity mygame --title "My Game" --version "1.2.0" --icon picture.png --sd E:
```

or take the game's own from the console, if the game has been used on it (installed, or with its card in), in two steps with
the port opened once in between:

```bash
python nxrelink.py identity mygame --ftp 192.168.0.100:5000
```

### On the console

- The ports show in the homebrew menu, all together in the folder `nx2-relink`.
- Start the homebrew menu **over a game**, not from the album.
- Leaving a port returns to the HOME menu.
- After a port ends with an error, starting the homebrew menu again at once can leave a black screen. Start the host game
  normally, close it, and then start it again holding R.

## What is on the card: saves, logs, the crash report

A port is two things on the SD card: its program, **`/switch/nx2-relink/<name>.nro`**, which the homebrew menu lists in the folder
`nx2-relink` with nothing else around it, and a folder at the card's root, **`/nx2-relink/<name>/`**, with everything else of it:

| In `/nx2-relink/<name>/` | What it is |
|---|---|
| `romfs.bin` (or `romfs.bin.00`, `.01`, ...) | The game's files |
| **`save/`** | **The game's saved data** |
| **`crash.txt`** | **Written when a run ended badly: the file to send with a report** |
| `log.txt`, `log.prev.txt` | The log of the last run, and of the one before |
| `debug` | If this file exists (empty will do), the log is the detailed one |
| `nvnwatch` | If this file exists when the port starts, log selected calls to the game's native NVN driver |
| **`mods/`** | **Optional: files of yours that the game gets in place of its own (below)** |

**Saved data.** A port keeps its saves in `/nx2-relink/<name>/save/`, as loose files, exactly as the game writes them.
When the port uses the host's SD file system, each cache storage index gets its own directory under `/nx2-relink/<name>/cache/`.
They are **apart from the console's own save data**: a port doesn't read or touch the saves of the installed game, and the
installed game doesn't see the port's. To back a port's saves up, copy that folder; to start over, delete it. Deleting a port's
folder deletes its saves with it.

Building a port again doesn't touch any of this: install it with `--nro-only` (or "Copy only the program" in the window) and
only the `.nro` is replaced.

Earlier installs kept these elsewhere: in `/nx2/` before 0.1.0 (saves in `/nx2/save/<name>/`), and in `/switch/nx2-relink/<name>/`
in 0.1.0 and 0.1.1. Installing the port again with this release moves its files and its saves to the new folder; nothing is
copied again.

**When a game doesn't work.** `log.txt` always notes what usually ends a game: a request the system refuses, a module the loader
refuses (`[ro] ... REFUSED`), the SDK's own closing message. When a run ends with an error (or was still running when the console
was turned off), the next time the port starts it writes `crash.txt`: what ended it, the console's error code for it when there
is one, and the last lines of that run's log. Attach it to an issue, with the game's name and version.

Error codes seen so far: `2168-0000` an instruction the processor doesn't know, `2168-0001` code that isn't there, `2168-0002`
data that isn't there, `2168-0003` a misaligned access, `2168-0006` the game stopped itself, `2168-0008` a system call the
console refuses.

For a harder case there is a **detailed log**: `--long-log` when installing, or an empty file named `debug` in the port's folder.
It is much slower, since each line is on the card before the game goes on.

## Mods: replacing a game's files (work in progress)

A file put in the port's folder under **`mods/`**, at the path it has inside the game, is what the game gets in place of its own
file of that path, from the next time it is started. Nothing is built again and the game's image isn't touched:

```
/nx2-relink/<name>/mods/Tex/Some_Texture.bntx.zs      in place of the game's  Tex/Some_Texture.bntx.zs
```

- **It is optional.** With no `mods/` folder, or nothing in it that the game has, a port runs exactly as it did before.
- **To take a mod out**, delete its file from `mods/`. The game's own file was never changed.
- **Only files the game already has** are replaced. One the game hasn't is left out, and the log says so. Adding new files
  isn't done yet.
- **The file must be in the form the game expects**, its packing included: the port passes it on as it is. Some games also
  keep a table of every file's size, and a replacement larger than the original may need that table replaced too. That is the
  game's own matter, and its modding community's.
- **The log tells you it worked.** `log.txt` begins with one `[mods]` line for each file laid over the game's, and has another
  the first time the game reads each one.

The game's paths are those of its dump: `python tools/game2.py info <dump>` and the dump's own `romfs/` folder show them.

## Advanced options

> [!WARNING]
> **These are risky to change.** They change what the game is given when it runs. A wrong value can make a game that worked stop
> at its start or while it loads. The defaults are what every game tried runs with: leave them alone unless a game needs
> something else, change one at a time, and say what you changed if you report a problem. They are used at your own risk.

| Option of `make` | Default | What it does |
|---|---|---|
| `--memory <MB>` | by the game | How much memory the game is told there is. From `2048` to `8992`; anything else is refused |
| `--whole auto \| all \| none \| <names>` | `auto` | Which modules are kept whole, as on the console, and which are relinked |
| `--long-log` | off | The port writes its detailed log from its first run |

The window has the same under **Advanced**, with a confirmation before a port is built with any of them changed.

**Memory.** No amount has to be known for each game, and none is guessed. The port sets nothing aside: when it starts it gives
back to the console nearly all the memory it received, and the game takes it as it asks for it, as on its console. The one number
is the most the game is told there is. The console gives an application about 9100 MB; the homebrew loader and the port keep
400 to 700 of them, by the size of the game's program. By default a game is told of what is left less 400 MB, and of 8192 at
most: some games take all they are told of and then need a little more. One kind of game is told of more, the console's own
8992: a game whose own table of heaps (read from its files when the port is made) keeps a stretch of address space apart from
its heap. Such a game sizes its heap by the console's full figure, and with less its heaps don't fit. A number you give is
used as it is: a smaller one makes a game that sizes itself by it take less; a larger one than there is is cut down to what
there is. **A game that closes itself or aborts right at its start may want more: try 8992.**

**Whole modules.** The console's loader maps eight images for a program. A game that loads no modules of its own as it runs has
all of them kept whole (`auto` does this). One that does load some has only the ones that must be whole kept so (`main` and
`sdk`, and those with thread-local storage), and the rest relinked, so that there is room for the ones it loads. A game with
more modules than fit won't run this way, and its log says so.

## Known limits

- **Few games tried.** A game may build and still not start: report it with its `crash.txt`.
- **Add-on content (DLC)** isn't installed.
- **Host game.** To the system, the running application is the game the homebrew menu was started over, and what a program
  may have is that game's, not the port's: its memory for graphics, its system resource, the services it may open (see *What
  you need*). The port stands in for the services a game can run without; nothing in a port can give the rest.
- **Memory.** A port keeps some 430 MB of the console's memory for itself (the game's program is held twice while it runs). A
  game made to use every byte of its console maps less than it would there, and is refused memory at times: its log has
  `MapPhysicalMemory ... REFUSED` lines. Some games take that in their stride; one that doesn't ends there.
- **A fault leaves no place.** When the console closes a port for a fault, it hands the program nothing: `crash.txt` has the
  kind of fault, from the console's own record, and the log up to it, but not where it happened. A report made with the
  detailed log on (*Advanced options*) says much more.
- Only a Switch 2 game on a Switch 2. A Switch game doesn't run on a Switch 2 through this tool.

## License

[MIT](LICENSE). The software is provided "as is", without warranty of any kind.
