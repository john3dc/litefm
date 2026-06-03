# litefm

A minimal two-pane file manager for the Linux terminal

- two independent panes side by side — copy / move from one into the other
- mouse support, marks, recursive search, chmod / chown
- one C file, only libc — no ncurses, no Rust, no Python (~45 KB binary)

![litefm with two panes](images/screenshot.png)


## Build

Needs only a C compiler + libc:

```sh
cc -O2 -Wall -o litefm litefm.c
chmod +x litefm
./litefm
```


### Install (optional)

```sh
sudo cp litefm /usr/local/bin/
```


## Usage

```sh
litefm                      # both panes start in the current directory
litefm ~/projekt ~/backup   # left pane, right pane
```


## Editor

`F4` / `e` opens the file in `$LITEFM_EDITOR` if set, otherwise in
[litefe](https://github.com/john3dc/litefe) if it is installed, then `$EDITOR`,
`nano`, and finally `vi`.
