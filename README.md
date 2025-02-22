# gd-frequency

A [GoldenDict](https://github.com/xiaoyifang/goldendict-ng) plugin to easily lookup the frequency of Japanese words

![image](https://github.com/user-attachments/assets/e1c1a06a-9e96-49d3-a176-77cbf8b44579)


## Installation
Clone this repository into a directory
```
git clone 'https://github.com/toytoi/gd-frequency.git'
```

Run the quick install script using (NOTE: only tested on Arch-based distros)
```
./quickinstall.sh
```

After this, it is neccessary to run gd-frequency at least once using sudo to generate the dictionary bin files. Example:
```
sudo gd-frequency --word 双子
```

## Setup

In GoldenDict, go to "Edit" > "Dictionaries" > "Programs" and add a new entry with the type set to `html` .
In Command Line put: `gd-frequency --word %GDWORD% --dict-path <path> --bin-path <path>`.
This program will then be treated as a dictionary.

If you installed the program using the provided script, simply use: `gd-frequency --word %GDWORD%`.

## Usage

```
gd-frequency --word %GDWORD% --dict-path <PATH/TO/DICT/FILE.json> --bin-path <PATH/TO/BIN/FILE.bin>
```

The repository already includes a [JPDB frequency list](https://github.com/Kuuuube/yomitan-dictionaries?tab=readme-ov-file#jpdb-v21-frequency) in the data directory, so it is recommended to point the `--dict-path` to it. (This is already done in the install script). The dictionary must be in yomichan format.

The bin file is generated from the dictionary file on the first run of the script. In the case of switching to a new/different frequency list, delete the old bin file (found in `/usr/share/gd-frequency` if the quick install script is used) and a new one will be generated on the next run of the script.




