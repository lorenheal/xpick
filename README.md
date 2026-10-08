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
