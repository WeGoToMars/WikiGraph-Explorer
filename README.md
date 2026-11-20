# WikiGraph Explorer
[![Build](https://github.com/WeGoToMars/WikiGraph-Explorer/actions/workflows/build.yml/badge.svg)](https://github.com/WeGoToMars/WikiGraph-Explorer/actions/workflows/build.yml)

Explore any Wikipedia as an interconnected graph of articles!

Inspired by https://github.com/jwngr/sdow, written in C++ for blazing fast performance 🔥! It builds a compact on-disk graph, searches it in about a millisecond, and can stay within a RAM budget you choose by reading the rest of the graph from the SSD. It uses multithreading and modern C++23 features.

## Supported graph features
- **Find all shortest paths between two pages** (like https://www.sixdegreesofwikipedia.com/) using bidirectional BFS <br>
  <img width="860" height="179" alt="image" src="https://github.com/user-attachments/assets/8dd3d13c-cab7-4f93-b3c4-883749cd52d0" />
- More to be added soon!

## Terminal UI
This app features a detailed TUI, made with [FTXUI](https://github.com/ArthurSonzogni/FTXUI) that allows for easy file management, benchmarking, and progress tracking.

### Download the latest Wikipedia dump automatically
| Select the project from the list | Wait for the download to complete |
|---|---|
| ![WikiSelector](https://github.com/user-attachments/assets/e498a78a-f07d-4cfd-89e6-c2e2970aeaec) | ![Downloader](https://github.com/user-attachments/assets/86e48057-80ca-419d-bb7e-40748637be80) |

<details>

<summary>Disk space, RAM and search speed (measured on an M5 Pro)</summary>
  <br>

  *Dumps are the gzip-compressed files downloaded from Wikimedia. The graph cache is built from them once and reused.*

  | Wikipedia | Date | Dumps | Graph cache | Build time / peak RAM | Search RAM (default) | Mean search |
  |---|---|---|---|---|---|---|
  | [English](https://en.wikipedia.org/wiki/Main_Page) | 2026-11-01 | **10 GB** | 3.1 GB | 59 s / 9.9 GB | **3.2 GB** | 1.2 ms |
  | [Simple English](https://simple.wikipedia.org/wiki/Main_Page) | 2026-11-01 | 150 MB | 50 MB | 0.8 s / 0.77 GB | 0.06 GB | 0.09 ms |

  English Wikipedia is 7.26 M articles and 716 M links. With a smaller `--ram-budget` it still answers from the SSD:

  | `--ram-budget` | Process RAM | Blocks read from SSD per search | Mean search | p95 |
  |---|---|---|---|---|
  | `all` | 3.26 GB | 0 | 1.23 ms | 5.0 ms |
  | `2.0` | 2.13 GB | 60 | 1.65 ms | 5.8 ms |
  | `1.0` | 1.16 GB | 200 | 1.88 ms | 6.5 ms |
  | `0.6` | 0.78 GB | 314 | 2.06 ms | 7.3 ms |

</details>

> [!TIP]
> If you want to experiment with the project, I highly recommend using [Simple English Wikipedia](https://simple.wikipedia.org/wiki/Simple_English_Wikipedia)

Downloaded files are stored in the data folder. If you run the app again, you will be able to load them without redownloading. A file is saved as `.part` until it is complete; if a download is interrupted (or you press Esc), selecting the same wiki again resumes it where it stopped. A damaged dump is caught when the graph is built (gzip checks a CRC of the whole file), with a message saying which file to delete.

The first time a wiki is opened, its dumps are turned into a compact graph cache next to them (`<wiki>-<date>.graph/`). Later runs open it in a fraction of a second. It is rebuilt automatically when the dumps change, or on demand with `--rebuild`.

### Keys
| Where | Keys |
|---|---|
| Wiki list | Up/Down, PgUp/PgDn, mouse wheel: move · Enter: download or open · r / R: next / previous RAM budget for the wiki you open (auto, all, 8, 4, 2, 1, 0.5 GB; starts at `--ram-budget`) · q or Esc: quit |
| Downloading, building the graph | Esc: stop and go back to the wiki list (downloads resume next time; a build starts over) |
| Title input | Enter: next field, then search · Tab: complete the title to the most linked-to article, or switch field · Esc: back to the wiki list |
| Results | Up/Down, PgUp/PgDn, Home/End, mouse wheel: scroll · Esc: edit the search · any letter: start a new search |

Ctrl-C quits from anywhere; running downloads and builds are stopped first.

### Where files are stored
The first of these that applies:
1. `--data-dir <folder>`, or the `WIKIGRAPH_DATA_DIR` environment variable (logs go to `<folder>/logs`)
2. `resources/data` next to the build folder (`build/resources/data` when run from `build/release`), if it can be created
3. the per-user data folder: `~/Library/Application Support/WikiGraph-Explorer` on macOS, `$XDG_DATA_HOME/WikiGraph-Explorer` (default `~/.local/share/WikiGraph-Explorer`) on Linux, `%LOCALAPPDATA%\WikiGraph-Explorer` on Windows

It does not depend on the folder you start the app from. The log of the last run is `logs/log.txt` (`resources/logs` in case 2); set `SPDLOG_LEVEL=info` (or `warn`, `off`) to log less.

### Usage Demonstration
<a href="https://asciinema.org/a/8BDAzxNWDLTrg6tAQ2BM1smex" target="_blank"><img src="https://asciinema.org/a/8BDAzxNWDLTrg6tAQ2BM1smex.svg" /></a>

## Building from source

### Library requirements
- curl (for downloading Wikipedia dumps)
- zlib (for decompressing Wikipedia dumps)

[git](https://git-scm.com/downloads) must be installed to automatically fetch dependencies.

This project is using [CMake](https://cmake.org/) (3.28 or later), and features presets for both Linux/MacOS and Visual Studio on Windows:

| Preset | Build folder | Use |
|---|---|---|
| `release` | `build/release` | optimised build with link-time optimisation |
| `debug` | `build/debug` | AddressSanitizer + UndefinedBehaviorSanitizer, full debug info |
| `profile` | `build/profile` | release optimisation with debug info and frame pointers, for profilers |
| `tsan` | `build/tsan` | ThreadSanitizer, to find data races between the UI and the background threads |

### Linux and MacOS
1. `git clone https://github.com/WeGoToMars/WikiGraph-Explorer`
2. `cd WikiGraph-Explorer`
3. `cmake --preset release`
4. `cmake --build build/release -j`
5. `./build/release/wikigraph`

On x86-64 Linux, install [NASM](https://www.nasm.us/) too. With it, rapidgzip uses ISA-L, which decompresses about 2.5 times faster where rapidgzip already knows where the compressed blocks start (the second read of the links table). Without NASM, it builds without ISA-L.

For clangd and clang-tidy, link the compile commands of the preset you use into the project root (the link is ignored by git): `ln -s build/debug/compile_commands.json .`. The naming convention is checked by `.clang-tidy`: `clang-tidy -p build/debug src/main.cpp`.

### Windows
Open the cloned directory in Visual Studio 2022 or later. VS should automatically recognize it as a CMake project.
1. Select `vcpkg-msvc-debug` or `vcpkg-msvc-release` configuration
    - Windows doesn't have `curl` or `zlib` installed by default. If you see a CMake error about not finding them, you can use [vcpkg](https://learn.microsoft.com/en-us/vcpkg/get_started/overview) to install both libraries.
    - Go to Tools -> Command Line -> Developer Powershell
    - Run:
      ```
      vcpkg install
      ```
    - If CMake still can't find packages make sure the VCPKG_ROOT environment variable is set correctly
2. After CMake has finished running, select startup item wikigraph.exe

### Tests
The core has unit and integration tests. CMake fetches Catch2 for them, and Python 3 writes the small synthetic wiki they read:
```
cmake --preset release -DWIKIGRAPH_TESTS=ON
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

## Data source
Wikipedia's compressed dump data is fetched directly from Wikimedia's public servers. The tool automatically downloads the required files for the selected language:

- **pages**: List of all articles and their metadata.
- **pagelinks**: All internal links between articles.
- **linktarget**: Mapping of link targets to page IDs.
- **redirect**: Where each redirect page points, so links through redirects reach the real article (optional; without it those links are dropped).

No data is stored or collected by this tool beyond what is downloaded to your local machine. All data is open and freely available under Wikimedia's Creative Commons licenses.

The first time you select a wiki, the relevant dump files will be downloaded automatically. Subsequent runs will reuse already-downloaded files unless you delete them.

For more details on the dump formats, see: https://meta.wikimedia.org/wiki/Data_dumps

## How the graph is stored
- **Compressed adjacency lists.** Every article's outgoing and incoming links are stored as sorted, delta-encoded varints (about 1.8 bytes per link for English, for each direction), in 16 KB blocks. Articles are numbered so that pages linked to each other sit close together on disk.
- **Bidirectional BFS.** Searches expand from both ends, always on the side with fewer links to scan, and count all shortest paths as they go. Only a few hundred lists are touched per search.
- **RAM budget.** Degrees, offsets, titles and search state always stay in RAM (0.56 GB for English). If the budget covers the link lists too, they are loaded fully and searches never touch the disk. Otherwise, the blocks that searches read most often (profiled when the cache is built) stay in RAM, and the rest are read from the SSD in parallel batches, one batch per BFS level. The default (`auto`) loads everything if it fits in half of physical memory.

## Command line
```
wikigraph [options]                                  interactive terminal UI
wikigraph --wiki <name> [--query <from> <to>] [--bench <n>] [options]
                                                     headless: build or open one wiki, print the results

  --ram-budget <size>   RAM the graph may use: "1.5", "800M", "2G", "all" or "auto" (default)
  --rebuild             rebuild the graph cache even if it is up to date
  --data-dir <folder>   where dumps and graphs are stored (also WIKIGRAPH_DATA_DIR); see "Where files are stored"
  --wiki <name>         dump name in the data directory (simplewiki-20261101) or a path prefix (/dumps/enwiki-20261101)
  --query <from> <to>   print the shortest paths between two article titles
  --bench <n>           time n searches between random articles
  --paths <n>           paths to print for --query (default 10)
```
Example:
```
$ ./wikigraph --wiki enwiki-20261101 --ram-budget 1G --query "Kevin Bacon" "Philosophy"
opened in 0.11 s | 7260699 articles, 715660736 links | budget 1.00 GB -> SSD + hot set | ...
Kevin Bacon -> Philosophy: 2 clicks, 1 shortest path | 1.375 ms, 2 lists, 4 blocks from disk
  Kevin Bacon -> Philadelphia -> Philosophy
```
