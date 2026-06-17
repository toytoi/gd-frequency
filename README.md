# gd-frequency

A [GoldenDict](https://github.com/xiaoyifang/goldendict-ng) plugin to lookup Japanese word frequency across multiple Yomitan style frequency dictionaries.

<img width="267" height="295" alt="image" src="https://github.com/user-attachments/assets/62ddf62b-60ee-4057-b9ec-3ee9e6974b5d" />


## Installation

Clone this repository into a directory

```
git clone 'https://github.com/toytoi/gd-frequency.git'
```

Run the installer. It is a self contained user install by default.

```
./quickinstall.sh
```

Default layout:

```
~/.local/share/gd-frequency/      binary, dictionaries, generated cache
~/.local/bin/gd-frequency         command symlink
```

The install step builds the binary, copies the zip dictionaries, generates the compact cache, and creates the command symlink. `clang++`, Meson, Ninja, and zlib development headers are required.

Install if not already present:

```
# Debian/Ubuntu
sudo apt install clang meson ninja-build zlib1g-dev

# Fedora
sudo dnf install clang meson ninja-build zlib-devel

# Arch
sudo pacman -S clang meson ninja zlib
```

Uninstall:

```
./quickinstall.sh --uninstall
```

System install:
```
./quickinstall.sh --system
```

System uninstall:

```
./quickinstall.sh --system --uninstall
```

Custom self contained prefix:

```
./quickinstall.sh --prefix "$HOME/opt/gd-frequency" --link-dir "$HOME/.local/bin"
```

Manual build:

```
CXX=clang++ meson setup builddir --prefix "$HOME/.local/share/gd-frequency" -Dnative_optimizations=true -Dblock_size=64
meson compile -C builddir
meson install -C builddir
```

## Setup

In GoldenDict, go to "Edit" > "Dictionaries" > "Programs" and add a new entry with the type set to `html` .
In Command Line put: `gd-frequency --word %GDWORD% --dict-path <path-to-dictionary.zip> --bin-path <path-to-cache.bin>`.
This program will then be treated as a dictionary.

If you installed the program using the provided script, simply use: `gd-frequency --word %GDWORD%`.

## Usage

```
gd-frequency --word %GDWORD% \
  --dict-path <PATH/TO/JPDB.zip> \
  --dict-path <PATH/TO/vn_freq.zip> \
  --bin-path <PATH/TO/dict.bin>
```

The repository includes two Yomitan zip frequency dictionaries in the `data` directory.

The bin file is generated from the zip dictionaries. If a configured zip path, file size, or modified time changes, `gd-frequency` rebuilds the cache automatically on the next lookup. You can also rebuild explicitly:

```
gd-frequency --build-cache \
  --dict-path <PATH/TO/JPDB.zip> \
  --dict-path <PATH/TO/vn_freq.zip> \
  --bin-path <PATH/TO/dict.bin>
```
