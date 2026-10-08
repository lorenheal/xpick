# xpipe

A small cross-platform X11 program that sits in a Unix text pipeline.

It reads standard input, shows the text in a window, lets you edit it, then:

- **OK** (or Ctrl+Enter / Ctrl+S) writes the buffer to standard output and exits 0
- **Cancel** (or Esc, or closing the window) writes `xpipe: cancelled` to the controlling terminal (`/dev/tty`, not the pipe) and exits 1

Nothing is written to stdout on cancel, so a downstream command does not see a partial result.

## Build

```sh
# Debian/Ubuntu
sudo apt install build-essential libx11-dev libxft-dev pkg-config

# Fedora
sudo dnf install gcc libX11-devel libXft-devel pkgconf-pkg-config

make
sudo make install   # optional, installs to /usr/local/bin
```

The source is POSIX C99 plus Xlib and Xft. It builds on Linux, the BSDs, illumos/Solaris, and macOS with XQuartz (`/opt/X11`). It does not use Linux-only APIs. Under Wayland it runs through XWayland like any other X11 client.

## Use

```sh
echo 'fix me' | xpipe | tee out.txt

# review a diff, then hand the edited text to the next stage
git diff | xpipe -t 'edit diff' | patch

# with pipefail, Cancel fails the pipeline
set -o pipefail
generate-report | xpipe > report.txt
```

If stdin is a terminal, the buffer starts empty (so launching `xpipe` does not swallow the keyboard). Otherwise the whole stream is read before the window opens. Input must be finite: do not use it as a coprocess that waits on `xpipe`'s stdout before closing the pipe.

```sh
xpipe [-t title] [-fn font] [-g WIDTHxHEIGHT]
```

`-fn` is an Xft font name, for example `DejaVu Sans Mono:size=14`.

## Editing

- Type, Backspace, Delete, Enter, Tab
- Arrows, Home, End, Page Up/Down; Ctrl+Home/End move through the document
- Shift+arrows selects; drag with the mouse selects
- Ctrl+A select all, Ctrl+C copy, Ctrl+X cut, Ctrl+V paste
- Middle-click pastes PRIMARY
- Ctrl+K deletes to end of line
- Ctrl+Z undo
- Mouse wheel scrolls; Shift+wheel scrolls horizontally

The buffer is kept as bytes. Valid UTF-8 is edited a character at a time; a bad byte is one unit. OK writes those bytes unchanged, including a missing final newline.

## Exit status

| code | meaning                                      |
|------|----------------------------------------------|
| 0    | accepted, buffer written to stdout           |
| 1    | cancelled; message on the controlling terminal |
| 2    | could not open the display, read, or write   |

# xpick

The companion filter for filenames. It reads a list of files and directories, shows them against the current working directory, and lets you check the ones that should continue down the pipe. There is no delete operation.

- Click a directory (or press Right) to expand it. Expansion is recursive: each level loads when opened. Symlinks are listed but not followed.
- A checkbox selects that path only. Checking a directory does not check its children. Select all / Select none (or Ctrl+A) applies to every row loaded so far, including children of directories you have expanded.
- **OK** (or Ctrl+Enter / Ctrl+S) writes each checked path to stdout, one per line, and exits 0.
- **Cancel** (or Esc, or closing the window) writes `xpick: cancelled` to the controlling terminal and exits 1. Stdout is left empty.

Printed paths are relative to the working directory at startup. `.` and `..` are collapsed, symlinks are not resolved, and a name that does not already start with a dot is prefixed with `./`, so `foo` is printed as `./foo`. A path outside the working directory is printed with `../`.

```sh
printf '%s\n' src tests | xpick | xargs -d '\n' wc -l

# no arguments and a terminal stdin starts at .
xpick -t 'pick sources'

# arguments are the roots; stdin is not read in that case
xpick ./Makefile ./README.md
```

Same build as `xpipe` (`make` builds both). Same exit codes: 0 accepted, 1 cancelled, 2 display or I/O failure.

# xpick
