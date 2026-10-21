# ZeroZip

Extract huge zip files on a nearly full drive.

A normal unzip needs free space for everything inside the zip, while the zip itself still sits on the drive. A 100 GB game zip needs roughly another 115 GB free. ZeroZip gives the zip's space back while it extracts, so it needs only about the difference between the unpacked size and the zip, plus room for one file.

## How it works

A zip stores its files one after another, with an index at the end. ZeroZip:

1. Reads the index and saves a copy of it next to the zip (`<name>.zip.zerozip`, hidden), so a stopped run can resume.
2. Extracts the **last** file in the zip and checks its CRC-32 as it is written.
3. Flushes the file to disk, and only then cuts that file's data off the end of the zip. The drive gets the space back immediately.
4. Repeats, working towards the front, until the zip is empty. Then it deletes the zip.

Files that are already in the destination folder, for example from a WinRAR run that ran out of space, are CRC-checked and kept instead of being written again.

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

## Supported zips

| | |
|---|---|
| Compression | Store, Deflate, Zstandard |
| Sizes | ZIP64 (files and archives over 4 GB) |
| Names | UTF-8, legacy code-page names, Info-ZIP Unicode path extra |
| Not supported | Deflate64, BZip2, LZMA, XZ, PPMd, encrypted zips, split zips, self-extractors |

Only zip for now. RAR could be extracted in place too: front to back with hole punching, or for non-solid archives back to front like zips. But that needs the official unrar decoder and is not done yet.

## Requirements

Windows 10 or 11 (x64). Nothing to install: ZeroZip is one small exe.

The window is drawn natively with Direct2D in the Windows 11 style:
- Mica, Segoe UI Variable, and your accent color and light or dark theme;
- on Windows 10 it falls back to a solid background, Segoe UI and the Windows 10 icon font.

Measured on Windows 11, it uses about 16–20 MB of private memory while idle and about 32 MB while extracting.

Not done yet: screen readers can't read the window, and High Contrast themes aren't followed.

## Building

Needs Visual Studio 2022 or newer with the C++ desktop workload.

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
| `ZEROZIP_TEST_THEME=light` or `dark` | Ignore the Windows theme. |
| `ZEROZIP_TEST_NO_MICA=1` | Use the solid Windows 10 background instead of Mica. |

`tests/make_e2e_zip.py` builds a game-like zip, about 2.4 GB with `--big`, for trying the app by hand.

## Layout

```
src/engine.*      zip index, space planning, extract + truncate / hole punching
src/gfx.*         Direct2D / DirectWrite drawing with the Windows 11 colors and type
src/main.cpp      the window, its screens and input, and the engine on a worker thread
third_party/      zstd (decoder) and miniz (inflate)
tests/            engine tests
assets/           the icon and the script that draws it
```

## Credits

- [Zstandard](https://github.com/facebook/zstd) (BSD)
- [miniz](https://github.com/richgel999/miniz) (MIT)

Their licenses are next to each in `third_party/`.
