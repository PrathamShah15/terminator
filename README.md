# Terminator — Custom Terminal Emulator

Terminator is a lightweight graphical terminal emulator and shell-like command environment for Linux. It is written in C using Xlib and Xft. The project implements its own command parsing and process-execution logic and demonstrates operating-system concepts such as process creation, pipes, file-descriptor redirection, signals, process groups, and job control.

The application uses a graphical X11 window with multiple tabs. Each tab maintains its own working directory, input line, output, and history-navigation state.

## Features

### Graphical interface
- Xlib window with Xft text rendering.
- Black terminal-style background and colored prompt.
- Prompt format: `pratham@terminator:/path/to/current/directory$`.
- Multiple tabs, with controls to create and close tabs.
- Independent working directory and command state for each tab.
- Output wrapping, scrolling, and window-resize handling.
- Off-screen Pixmap backbuffer for complete-frame rendering.
- Mouse selection of rendered terminal output.

### Shell and command execution
- External command execution using `fork()`, `execvp()`, and `waitpid()`.
- Built-in commands: `cd`, `clear`, `exit`, `history`, `jobs`, `fg`, `bg`, and `multiWatch`.
- Quoted arguments and multiline command continuation.
- Environment-variable expansion, including `$NAME` and `${NAME}`.
- Command substitution using `$(...)`, including nested substitutions.
- Filename wildcard expansion using `glob()`.

### Pipelines and redirection
- Multi-stage pipelines using `|`.
- Input redirection using `<`.
- Output redirection using `>`.
- File-descriptor setup using `pipe()` and `dup2()`.

### Process and job control
- Background commands using `&`.
- Process-group tracking for jobs and pipelines.
- `jobs`, `fg`, and `bg` commands.
- Foreground interruption with `Ctrl+C`.
- Foreground suspension with `Ctrl+Z`.
- GUI event processing while a foreground command is running.

### multiWatch
- Run multiple commands concurrently.
- Each watched command may contain a pipeline.
- Rerun each command two seconds after its previous execution completes.
- Stop the active monitoring session with `Ctrl+C`.

### History, completion, and clipboard
- Persistent command history in `~/.terminator_history`.
- History capacity of 10,000 commands.
- Search history with `Ctrl+R`.
- Filename/path completion with repeated `Tab` presses.
- Copy selected terminal output with `Ctrl+Shift+C`.
- Paste clipboard text with `Ctrl+Shift+V`.

## Requirements

- Linux with an X11-compatible graphical session (X11 or XWayland).
- GCC.
- GNU Make.
- `pkg-config`.
- X11 and Xft development libraries.
- A monospace font; the application requests `DejaVu Sans Mono`.

### Arch Linux

```bash
sudo pacman -S --needed gcc make pkgconf libx11 libxft fontconfig
```

### Debian / Ubuntu

```bash
sudo apt install build-essential make pkg-config libx11-dev libxft-dev fontconfig
```

## Build and run

Clone the repository:

```bash
git clone https://github.com/PrathamShah15/terminator.git
cd terminator
```

Build:

```bash
make
```

Run:

```bash
./terminator
```

To remove generated object files and the executable:

```bash
make clean
```

The Makefile compiles these source files and links against X11 and Xft:

```text
main.c gui.c shell.c parser.c builtins.c hotkeys.c multiwatch.c
```

## Example commands

```bash
pwd
ls -la
cd ..
echo "$HOME"
echo "${HOME}"
echo "Current directory: $(pwd)"
echo "Files in $(basename $(pwd)): $(ls | wc -l)"
ls *.c
cat < input.txt
echo "hello" > output.txt
cat input.txt | grep error | wc -l
sleep 30 &
```

The parser handles command substitution internally; it does not pass the command line to `/bin/sh -c`.

## Built-in commands

| Command | Description |
|---|---|
| `cd [path]` | Change the active tab's working directory. |
| `clear` | Clear the active tab's output. |
| `exit` | Exit the application. |
| `history` | Display the most recent 1,000 stored commands. |
| `jobs` | List tracked background or stopped jobs. |
| `fg [job]` | Resume a tracked job in the foreground. |
| `bg [job]` | Resume a stopped job in the background. |
| `multiWatch ["cmd1", "cmd2"]` | Repeatedly execute a list of commands. |

Job identifiers can be supplied as a number or with a `%` prefix, for example `fg 1` or `bg %1`.

### multiWatch example

```text
multiWatch ["date +%T", "ls | wc -l"]
```

The commands run concurrently. Each command is launched again two seconds after its previous execution completes. Only one `multiWatch` session can be active at a time. Focus the Terminator window and press `Ctrl+C` to stop it.

The compact spellings `date+%T` and `wc-l` are also accepted for the assignment example and normalized to `date +%T` and `wc -l`.

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl+A` | Move the input cursor to the beginning. |
| `Ctrl+E` | Move the input cursor to the end. |
| `Ctrl+U` | Delete input before the cursor. |
| `Ctrl+K` | Delete input after the cursor. |
| `Ctrl+W` | Delete the previous word. |
| `Ctrl+L` | Clear the active tab's output. |
| `Ctrl+D` | Send EOF / exit on an empty input line. |
| `Ctrl+T` | Create a new tab. |
| `Alt+1` … `Alt+8` | Switch to the corresponding tab. |
| `Ctrl+C` | Interrupt the foreground process group; stop `multiWatch`. |
| `Ctrl+Z` | Suspend the foreground process group. |
| `Up` / `Down` | Navigate command history. |
| `Left` / `Right` | Move the input cursor. |
| `Ctrl+R` | Search command history. |
| `Tab` | Start or cycle through filename completions. |
| `Enter` | Execute input, or accept a completion when completion is active. |
| `Esc` | Cancel history search or filename completion. |
| `Ctrl+Shift+C` | Copy the selected terminal output. |
| `Ctrl+Shift+V` | Paste clipboard text at the input cursor. |
| Mouse drag | Select rendered terminal output. |
| Mouse wheel | Scroll terminal output. |

When filename completion is active, repeated `Tab` presses cycle through candidates. The selected candidate is highlighted. Press `Enter` once to accept it into the input line; press `Enter` again to execute the command. Press `Esc` to cancel.

### History search

Command history is shared across tabs and persisted in `~/.terminator_history`. The history store retains up to 10,000 commands; consecutive duplicate commands are skipped. The `history` command displays up to the most recent 1,000 entries.

Press `Ctrl+R` and enter a search term. The most recent exact command match is preferred. If there is no exact match, the search uses the longest common contiguous substring and prints matches only when the score is greater than two characters. Press `Esc` to cancel.

### Clipboard behavior

`Ctrl+Shift+C` copies the currently selected, rendered terminal output to the X11 clipboard. `Ctrl+Shift+V` requests clipboard text and inserts it at the current input cursor. Mouse dragging selects output text.

## Project structure

```text
terminator/
├── main.c          # Application initialization and X11 resource setup
├── gui.c           # Window, rendering, tabs, scrolling, mouse selection, event loop
├── shell.c         # Built-in dispatch and per-tab working-directory handling
├── parser.c        # Expansion, parsing, pipelines, redirection, process/job control
├── builtins.c      # Built-ins, tab output buffer, persistent history
├── hotkeys.c       # Input editing, shortcuts, completion, history search, clipboard
├── multiwatch.c    # Concurrent repeated command execution
├── terminator.h    # Shared structures, constants, and function declarations
└── Makefile        # Build rules
```

## Implementation notes and limitations

- This is an educational shell-like terminal project, not a complete Bash/POSIX shell.
- Command parsing and expansion are implemented in `parser.c`; there is no separate `lexer.c` module in this version.
- External commands are launched with `execvp()`. Terminator does not use `/bin/sh -c` to interpret command strings.
- Command substitution runs the inner command through the project's own parser/executor, captures standard output, and removes trailing newline characters before inserting the result. Background commands inside command substitution are rejected.
- The project uses pipes and child processes but does **not** currently use a pseudo-terminal (PTY). As a result, interactive full-screen programs such as `vim`, `top`, and `less` are not supported as they are in a conventional terminal emulator.
- Shell quoting, expansion, redirection, and job-control behavior are limited to the syntax implemented in this project; full Bash compatibility is not intended.
- `multiWatch` supports one active monitoring session at a time.
- The application requires an X11 display or XWayland.

## License

No license file is included in this project archive. Add a `LICENSE` file if you intend to distribute the project under a specific open-source license.
