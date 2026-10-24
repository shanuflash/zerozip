# ZeroZip

Extract huge zip and RAR files on a nearly full drive.

A normal unzip needs free space for everything inside the zip, while the zip itself still sits on the drive. A 100 GB game zip needs roughly another 115 GB free. ZeroZip gives the zip's space back while it extracts, so it needs only about the difference between the unpacked size and the zip, plus room for one file.

## Download

[ZeroZip.exe](https://github.com/shanuflash/zerozip/releases/latest/download/ZeroZip.exe), the latest release. There's nothing to install: run it, then drop a zip on the window or on the exe.

Or with winget:

```
winget install shanuflash.ZeroZip
```

The exe isn't code-signed yet, so Windows SmartScreen may warn the first time: choose "More info", then "Run anyway".

## Right-click menu

The switch on the first screen adds **Extract with ZeroZip** to the right-click menu of zip files and split zips (any part on Windows 11, the first part on Windows 10), for your Windows account only. It sits next to WinRAR or 7-Zip without changing which app opens zips.

- **Windows 11:** the entry is at the top of the menu. Windows only allows that for an app package, so ZeroZip installs a small one (`assets/package`) that points Explorer at `ZeroZipShell.dll` in `%LOCALAPPDATA%\Programs\ZeroZip`. The package isn't signed, so turning the switch on asks for admin once. Turning it off doesn't.
- **Windows 10:** the entry is a plain registry entry, with no admin needed.

Moving or updating ZeroZip.exe doesn't break the entry. Turn the switch off before deleting ZeroZip for good.

## How it works

A zip stores its files one after another, with an index at the end. ZeroZip:

1. Reads the index and saves a copy of it next to the zip (`<name>.zip.zerozip`, hidden), so a stopped run can resume.
2. Extracts the **last** file in the zip and checks its CRC-32 as it is written.
3. Flushes the file to disk, and only then cuts that file's data off the end of the zip. The drive gets the space back immediately.
4. Repeats, working towards the front, until the zip is empty. Then it deletes the zip.

Files that are already in the destination folder, for example from a WinRAR run that ran out of space, are CRC-checked and kept instead of being written again.

### Split zips

Zips that come in parts work the same way, opened from any of their parts:

- **Split**, as 7-Zip makes them: `Game.zip.001`, `Game.zip.002`, …, which is one zip cut into pieces.
- **Spanned**, as WinZip and WinRAR make them: `Game.z01`, `Game.z02`, …, `Game.zip`.

ZeroZip reads the parts as one zip. As the cut moves back past the start of a part, that part is deleted, so the parts disappear one by one from the last. The resume data sits next to the first part, which goes last. A set with a part missing is refused before anything is touched, naming the missing part where it can.

Before starting, ZeroZip simulates the whole run against your actual free space and shows:
- the lowest point free space will reach,
- how much will be free at the end,
- whether a regular unzip would even fit.

It refuses to start if free space would drop below a 1 GB safety floor.

### Files bigger than the free space

When a single file won't fit, and the zip and the folder are on the same NTFS or ReFS drive, ZeroZip extracts that file in **hole-punch mode**. It marks the zip as a sparse file and frees the file's compressed data in 64 MB steps as it is decompressed, using only data that has already been written and flushed.

The trade-off is that the file's CRC can only be checked once its data is already gone. ZeroZip lists these files before you start and warns in the confirm step:

- If one fails its check, or the PC loses power while it is being extracted, that one file is lost. ZeroZip says which one, skips it on the next run, and carries on with the rest.
- A stop during such a file takes effect once the file is finished.

FAT32 and exFAT drives can't free part of a file, so there the biggest file has to fit whole.

## Safety

- A file's data is only removed from the zip after the file has been written, CRC-checked and flushed to disk. Hole-punch mode, above, is the one exception, and it is shown before you start.
- New files are written as `*.zzpart`, then renamed into place and the folder is flushed. A finished `.zzpart` left behind by a crash is recovered on the next run.
- Stopping, closing the window, a crash or a power cut leave a resumable state. Open the zip again and ZeroZip picks up where it stopped, in the same folder.
- The resume data is tied to the zip file itself. A new download that reuses the old name starts fresh instead of being cut using the old zip's layout.
- Before anything is touched, ZeroZip checks every entry and refuses the zip if any would fail partway:
  - unsupported compression or encryption;
  - unsafe paths such as `..`;
  - names too long for Windows;
  - a file and a folder with the same name;
  - files over 4 GB on FAT32;
  - entries whose data overlaps.
- Paths are cleaned up: Windows reserved names (`CON`, `NUL`, …) and invalid characters are renamed. Names that differ only in case get a numbered copy (`readme (2)`) instead of overwriting each other.

**The zip is consumed.** When ZeroZip finishes, the zip is gone. Keep a copy elsewhere if you need it.

## Supported archives

| | |
|---|---|
| Compression | Store, Deflate, Zstandard |
| Sizes | ZIP64 (files and archives over 4 GB) |
| Parts | split (`.zip.001`, …) and spanned (`.z01`, …, `.zip`) |
| Names | UTF-8, legacy code-page names, Info-ZIP Unicode path extra |
| Not supported | Deflate64, BZip2, LZMA, XZ, PPMd, encrypted zips, self-extractors |
| RAR | RAR 4 and 5, in one file or in parts, solid or not; not password protected |

## RAR

RAR archives are read with the official UnRAR code, front to back, so their space comes back a volume at a time:

- **In parts** (`Game.part01.rar`, … or the old `Game.rar`, `Game.r00`, …): each volume is deleted once every file with data in it is written, checked and flushed. Free space dips by about one volume, plus the file being written. Any volume opens the set, and a stopped run resumes from the first volume left.
- **In one file**: it's only deleted at the end, so its files have to fit next to it. The plan shows whether they do.
- **Solid** RARs can only be read from their start. ZeroZip keeps their volumes until the space is actually needed, so with room to spare a solid RAR stops and resumes like any other. When it has to delete one, it says so before starting, and from then on it can be paused but not stopped.

Password-protected RARs and links inside a RAR aren't supported yet, and are refused before anything is touched. NTFS streams stored in a RAR aren't kept.

## Requirements

Windows 10 or 11 (x64). Nothing to install: ZeroZip is one exe of about 1 MB.

The window is drawn natively with Direct2D and DirectWrite, in ZeroZip's own dark look with the Geist fonts built in. It looks the same on Windows 10 and 11.

Measured on Windows 11, it uses about 10–14 MB of private memory and no CPU while idle, and about 23 MB while extracting.

Not done yet: screen readers can't read the window, and High Contrast themes aren't followed.

## Building

Needs Visual Studio 2022 or newer with the C++ desktop workload, which includes the Windows SDK (for `makeappx`, which packs the right-click menu package).

```bat
build.bat          :: builds ZeroZip.exe
build.bat test     :: builds the engine test driver, build\zztest.exe
```

Tests, which need Python 3 with `pip install zstandard`:

```bat
build.bat test
python tests\run_tests.py
```

Test switches, read from the environment:

| Variable | Effect |
|---|---|
| `ZEROZIP_TEST_PUNCH_OVER=<bytes>` | Files at least this big use hole-punch mode, so it can be tested on a drive with plenty of space. |

`tests/make_e2e_zip.py` builds a game-like zip, about 2.4 GB with `--big`, for trying the app by hand.

## Layout

```
src/engine.*      zip and RAR reading, space planning, extract + truncate / hole punching
src/gfx.*         Direct2D / DirectWrite drawing in the ZeroZip style, with the embedded fonts
src/main.cpp      the window, its screens and input, and the engine on a worker thread
src/shell.cpp     ZeroZipShell.dll, the Windows 11 right-click entry
third_party/      zstd (decoder), miniz (inflate) and UnRAR
tests/            engine tests
assets/           the icon, the script that draws it, the fonts, and the menu package
```

## Credits

- [Zstandard](https://github.com/facebook/zstd) (BSD)
- [UnRAR](https://www.rarlab.com/rar_add.htm) by Alexander Roshal, under its own license: "UnRAR source code may be used in any software to handle RAR archives without limitations free of charge, but cannot be used to develop RAR (WinRAR) compatible archiver and to re-create RAR compression algorithm, which is proprietary. Distribution of modified UnRAR source code in separate form or as a part of other software is permitted, provided that full text of this paragraph, starting from "UnRAR source code" words, is included in license, or in documentation if license is not available, and in source code comments of resulting package."
- [miniz](https://github.com/richgel999/miniz) (MIT)
- [Geist](https://vercel.com/font) and Geist Mono fonts (SIL OFL 1.1)

Their licenses are next to each in `third_party/` and `assets/fonts/`.
