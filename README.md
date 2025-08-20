# WikiGraph Explorer
Explore any Wikipedia as an interconnected graph of articles!

Inspired by https://github.com/jwngr/sdow, written in C++ for blazing fast performance 🔥! This tool supports multithreading to go *as fast as possible* and takes advantage of modern C++23 features.

## Supported graph features
- **Find all shortest paths between two pages** (like https://www.sixdegreesofwikipedia.com/)
- More to be added soon!

## Terminal UI
### Download the latest Wikipedia dump automatically
| Select the project from the list | Wait for the download to complete |
|---|---|
| ![WikiSelector](https://github.com/user-attachments/assets/e498a78a-f07d-4cfd-89e6-c2e2970aeaec) | ![Downloader](https://github.com/user-attachments/assets/86e48057-80ca-419d-bb7e-40748637be80) |

<details>

<summary>Example amount of disk space and RAM required for different Wikipedia dumps</summary>
  <br>
  
  *Disk space is used for gzip-compressed Wikipedia dumps. Uncompressed files are 5-7x larger.* <br>
  *RAM is used to hold the adjacency list and title lookup hashtable. RAM usage during graph construction is up to 2x higher than listed.*
  
  | Wikipedia | Date | Disk Space | RAM |
  |---|---|---|---|
  | [English](https://en.wikipedia.org/wiki/Main_Page) | 2025-08-01 | **10.24 GB** | **6.79 GB** |
  | [German](https://de.wikipedia.org/wiki/Wikipedia:Hauptseite) | 2025-08-01 | 1.49 GB | 2.80 GB |
  | [Ukrainian](https://uk.wikipedia.org/wiki/%D0%93%D0%BE%D0%BB%D0%BE%D0%B2%D0%BD%D0%B0_%D1%81%D1%82%D0%BE%D1%80%D1%96%D0%BD%D0%BA%D0%B0) | 2025-08-01 | 828 MB | 2.13 GB |
  | [Simple English](https://simple.wikipedia.org/wiki/Main_Page) | 2025-08-01 | 136.2 MB | 765 MB |
  
</details>

> [!TIP]
> If you want to experiment with the project, I highly recommend using [Simple English Wikipedia](https://simple.wikipedia.org/wiki/Simple_English_Wikipedia)

Downloaded files are stored in the `data` directory. If you run the app again, you will be able to load them without redownloading.

### Usage Demonstration
<a href="https://asciinema.org/a/8BDAzxNWDLTrg6tAQ2BM1smex" target="_blank"><img src="https://asciinema.org/a/8BDAzxNWDLTrg6tAQ2BM1smex.svg" /></a>

## Building from source

### Library requirements
- curl (for downloading Wikipedia dumps)
- zlib (for decompressing Wikipedia dumps)

This project is using [CMake](https://cmake.org/), and features helpful (debug, profile, release) presets for both Linux/MacOS and Visual Studio on Windows. The dependencies will be automatically fetched with git.

On Linux and MacOS, run:
1. `git clone https://github.com/WeGoToMars/WikiGraph-Explorer`
2. `cd WikiGraph-Explorer`
3. `mkdir build && cd build`
4. `cmake .. --preset release && cd release`
5. `make`
6. `./wikigraph`

On Windows, use Visual Studio and open the cloned directory as a CMake project. I haven't tested this extensively, more detailed docs will be added in the future.

## Data source
Wikipedia's compressed dump data is fetched directly from Wikimedia's public servers. The tool automatically downloads the required files for the selected language:

- **pages**: List of all articles and their metadata.
- **pagelinks**: All internal links between articles.
- **linktarget**: Mapping of link targets to page IDs.

No data is stored or collected by this tool beyond what is downloaded to your local machine. All data is open and freely available under Wikimedia's Creative Commons licenses.

The first time you select a wiki, the relevant dump files will be downloaded automatically. Subsequent runs will reuse already-downloaded files unless you delete them.

For more details on the dump formats, see: https://meta.wikimedia.org/wiki/Data_dumps

### Async vs Parallel line readers
- **Parallel line reader is enabled by default.** Disable with `-DPARALLEL_DECOMPRESSION=OFF` if you have issues with compilation or running.
- **Backends**:
  - **AsyncLineReader**: single-threaded gzip decompression via [zstr](https://github.com/mateidavid/zstr), which uses regular zlib under the hood.
  - **ParallelLineReader**: multi-threaded gzip decompression via [rapidgzip](https://github.com/mxmlnkn/rapidgzip).
- **Threading & buffering**:
  - **Async**: starts a background thread for decompression, uses a mutex/CV with a small bounded queue.
  - **Parallel**: uses a lock-free queue with chunked/striped decompression and lightweight backpressure, using all of your computers
- **Indexing**: Parallel imports an existing [gziptool](https://github.com/circulosmeos/gztool) index (if present) and exports one after reading, speeding up future runs.
- **Performance**: Parallel mode typically yields 2–4x throughput on large dumps (more benchmarks will be available later).

## Alternative hashmap implementations
By default, this project uses [emhash](https://github.com/ktprime/emhash) as a higher-performance hashmap for internal data structures. If you prefer to use the standard C++ `std::unordered_map` instead, you can switch by setting a CMake option:

- To use `std::unordered_map` use `-DUSE_STD_UNORDERED_MAP=ON` option with `cmake`.

Only files that include `src/Utils/Hashmap.h` will be recompiled when toggling this option, so switching is fast.
