# Terminator — A Custom X11 Terminal Emulator and Shell

<p align="center">
  <strong>Computing Lab (CS69201) · Term Project</strong><br>
  A multi-tab terminal emulator and command interpreter written in C for Linux
</p>

---

## Table of Contents

- [1. Project Overview](#1-project-overview)
- [2. Feature Matrix](#2-feature-matrix)
- [3. Architecture at a Glance](#3-architecture-at-a-glance)
- [4. Execution Modes](#4-execution-modes)
- [5. Requirements and Dependencies](#5-requirements-and-dependencies)
- [6. Build and Run](#6-build-and-run)
- [7. User Guide](#7-user-guide)
- [8. Keyboard Shortcuts](#8-keyboard-shortcuts)
- [9. Source Code Layout](#9-source-code-layout)
- [10. Implementation Notes and Limits](#10-implementation-notes-and-limits)
- [11. Troubleshooting](#11-troubleshooting)
- [12. Concepts Demonstrated](#12-concepts-demonstrated)

---

## 1. Project Overview

**Terminator** is a graphical terminal emulator and a custom command interpreter implemented in **C** for Linux. The graphical interface is built with **Xlib/X11** and **Xft**. It provides multiple independent tabs, an editable command line, command output rendering, file-name completion, command history, pipelines, redirection, signal delivery, and background-job management.

The shell does not launch `/bin/bash` as its interactive command interpreter. It parses commands in the application and launches external programs with the POSIX process APIs. The project is designed to demonstrate operating-system concepts directly, including process creation, pipes, file-descriptor duplication, synchronization, shared memory, polling, and signal handling.

### Project at a glance

| Property | Details |
|---|---|
| Project | Terminator — A Custom Terminal |
| Course | Computing Lab (CS69201) |
| Language | C (C11) |
| Platform | Linux with an X11 display server |
| GUI | Xlib and Xft |
| Build system | GNU Make |
| Execution models | POSIX thread mode and process mode |
| Maximum tabs | 8 (`MAX_TABS`) |
| Command history capacity | 10,000 entries (`HISTORY_CAP`) |
| Main executable | `./terminator` |

### What Terminator demonstrates

| Area | Main concepts and APIs |
|---|---|
| GUI and event handling | X11 events, `XLookupString()`, Xft text rendering, Pixmap backbuffer |
| Process creation | `fork()`, `execvp()`, `waitpid()` |
| IPC | `pipe()`, `read()`, `write()` |
| Redirection | `open()`, `dup2()`, `close()` |
| Pipelines | `pipe()`, process groups, descriptor lifecycle |
| Concurrency | `pthread_create()`, `pthread_mutex_t`, `fork()` |
| Process-shared state | `shm_open()`, `mmap()`, POSIX semaphore |
| Event multiplexing | `poll()` |
| Job control | `setpgid()`, `kill()`, `SIGINT`, `SIGTSTP`, `SIGCONT` |
| Filesystem interaction | `opendir()`, `readdir()`, `getcwd()`, `chdir()` |

---

## 2. Feature Matrix

The following matrix maps the implemented feature areas to the project specification's task numbering.

| Specification task | Feature area | Implementation in this repository |
|---:|---|---|
| 1 | X11 GUI and tabs | X11 window, tab bar, per-tab buffers, mouse/keyboard tab controls, output polling and scrolling |
| 2 | Built-in and external commands | External command execution, `cd`, `exit`, `history`, `clear`, `jobs`, `fg`, `bg`, and `multiWatch` dispatch |
| 3 | Multiline input and quoting | Continuation prompt, trailing-backslash continuation, multiline quote tracking, nested `$()` and backtick substitution |
| 4 | I/O redirection | Input `<`, output `>`, and combined redirection using `open()` and `dup2()` |
| 5 | Pipes | Multi-stage pipelines using `N - 1` pipes for `N` stages |
| 6 | `multiWatch` | Concurrent command/pipeline monitoring, output collection with `poll()`, timed re-execution and Ctrl+C stop |
| 7 | Hotkeys | Line editing, history navigation, reverse search, clipboard shortcuts and tab controls |
| 8 | Signals and job control | Foreground process-group signal delivery, background execution, `jobs`, `fg`, and `bg` |
| 9 | Searchable history | Persistent shared history file, 10,000-entry capacity, `history`, and Ctrl+R search |
| 10 | File-name completion | Current-directory lookup, longest common prefix, numbered candidate selection |

**Specification alignment note:** The submitted source implements the interactive/default hotkey set, but does not contain a `~/.terminatorrc` key-remapping loader. The `source` built-in mentioned in the specification's Task 2 list is also not present in the submitted dispatcher. These are therefore not advertised as implemented here. The separate script-interpreter requirement appearing after Task 10 in the supplied specification is not claimed as part of this Task 1–10 feature set.

---

## 3. Architecture at a Glance

```text
                         +------------------------------+
                         |            main.c            |
                         | Startup, resources, mode     |
                         | selection, shared history    |
                         +---------------+--------------+
                                         |
                                         v
                         +------------------------------+
                         |             gui.c            |
                         | X11 window, drawing, tabs,   |
                         | poll()-based event loop      |
                         +---------------+--------------+
                                         |
                    +--------------------+--------------------+
                    |                                         |
             THREAD MODE                               PROCESS MODE
                    |                                         |
          +---------v----------+                    +---------v----------+
          | Tab worker thread  |                    | Tab worker process |
          | pthread_create()   |                    | fork()             |
          +---------+----------+                    +---------+----------+
                    |                                         |
                    +--------------------+--------------------+
                                         |
                                         v
                         +------------------------------+
                         |            shell.c          |
                         | Per-tab cwd and dispatch    |
                         +---------------+--------------+
                                         |
                      +------------------+------------------+
                      |                                     |
              +-------v--------+                    +-------v--------+
              |   builtins.c   |                    |    parser.c     |
              | Built-in shell |                    | Parse, expand,  |
              | commands       |                    | fork, pipe, job |
              +----------------+                    | control         |
                                                    +-------+--------+
                                                            |
                                             +--------------+-------------+
                                             |                            |
                                      +------v------+              +------v------+
                                      | Child stage | -- pipe ---> | Child stage |
                                      | execvp()    |              | execvp()    |
                                      +-------------+              +-------------+

          hotkeys.c: line editor, completion, history search, clipboard
          multiwatch.c: concurrent watched pipelines and repeat scheduling
          terminator.h: shared structures, limits and function declarations
```

The GUI process owns the X11 connection. Worker contexts execute shell commands and send output back to the GUI through per-tab pipes. The GUI reads those pipes and updates each tab's output buffer.

---

## 4. Execution Modes

Each tab has its own working directory and command-execution context. Start Terminator in either of the following modes.

### 4.1 Thread mode (default)

```bash
./terminator
```

| Component | Behaviour |
|---|---|
| GUI | Runs on the main application thread |
| Tab worker | A detached POSIX thread is created for each tab |
| Memory | Threads share the process address space |
| History synchronization | `pthread_mutex_t` protects shared history operations |
| Tab output | Worker output is forwarded through the tab's output pipe |

Thread mode is the default and is suitable for normal use. Since tabs share memory, shared data must be synchronized when accessed concurrently.

### 4.2 Process mode

```bash
./terminator useProcess=True
```

| Component | Behaviour |
|---|---|
| GUI | Remains in the parent process |
| Tab worker | Each tab is created using `fork()` |
| Memory | Each worker has a separate address space |
| GUI/worker communication | Two pipes per tab: command input and output |
| Shared history | POSIX shared memory mapped with `mmap(..., MAP_SHARED, ...)` |
| History synchronization | Named POSIX semaphore |
| Per-tab shared state | Anonymous shared mapping for `TabSharedState` |

The parent sends framed command messages to a tab worker. The worker executes the command and writes output to its output pipe. The GUI monitors the output pipes alongside the X11 connection.

### 4.3 Behaviour shared by both modes

- A tab maintains its own current working directory.
- History is shared across tabs.
- Commands execute as child processes.
- The GUI remains responsible for X11 interaction and rendering.
- Signals generated by the GUI are directed to the foreground process group associated with the relevant tab.

---

## 5. Requirements and Dependencies

### 5.1 Supported environment

| Requirement | Details |
|---|---|
| Operating system | Linux |
| Display | X11 server (or an X11-compatible environment such as WSLg) |
| Compiler | GCC with C11 support |
| Build tool | GNU Make |
| Package metadata | `pkg-config` |
| GUI libraries | Xlib (`libX11`) and Xft (`libXft`) |
| Font stack | Fontconfig and FreeType dependencies used by Xft |
| Recommended font | DejaVu Sans Mono |

A graphical session with a working X11 display is required to launch the application. Building the executable does not require opening the GUI.

### 5.2 Install dependencies

**Ubuntu / Debian**

```bash
sudo apt update
sudo apt install -y build-essential make pkg-config \
    libx11-dev libxft-dev fontconfig
```

**Arch Linux**

```bash
sudo pacman -S --needed gcc make pkgconf libx11 libxft fontconfig
```

**Fedora**

```bash
sudo dnf install -y gcc make pkgconf-pkg-config \
    libX11-devel libXft-devel fontconfig
```

If the recommended font is not installed, install the DejaVu font package available for your distribution. Xft can use another available font if the preferred face is unavailable.

---

## 6. Build and Run

### 6.1 Clone or extract the project

Open a terminal in the directory containing `Makefile`, `main.c`, and `terminator.h`.

```bash
cd terminator
```

### 6.2 Build

```bash
make
```

To perform a clean rebuild:

```bash
make clean
make
```

The Makefile compiles the C source files and links the X11, Xft, pthread, and real-time libraries. On success, the executable is created at:

```text
./terminator
```

### 6.3 Launch

**Default thread mode**

```bash
./terminator
```

**Process mode**

```bash
./terminator useProcess=True
```

Run the executable from a graphical Linux session. If the X server is unavailable or `DISPLAY` is not set correctly, X11 window creation will fail.

### 6.4 Clean build artifacts

```bash
make clean
```

This removes object files and the generated `terminator` executable. It does not remove the persistent command-history file in the home directory.

---

## 7. User Guide

### 7.1 Tabs

| Action | How |
|---|---|
| Create a tab | Click `+` or press `Ctrl+T` |
| Switch tabs | Click a tab header or press `Alt+1` … `Alt+8` |
| Close a tab | Click its close control or press `Ctrl+D` on an empty input line |
| Keep another tab active | Select its tab header; each tab retains its own shell working directory and output |

The implementation defines `MAX_TABS` as 8. The tab bar displays the tab index and its working directory.

### 7.2 Commands

External commands are launched as child processes. Common built-ins are handled by Terminator itself.

| Command | Purpose | Example |
|---|---|---|
| `cd` | Change the active tab's working directory | `cd ~/Downloads` |
| `exit` | Close the active tab / exit when it is the only tab | `exit` |
| `history` | Display recent command history | `history` |
| `clear` | Clear the active tab's output | `clear` |
| `jobs` | List tracked background and stopped jobs | `jobs` |
| `fg` | Bring a job to the foreground | `fg %1` |
| `bg` | Resume a stopped job in the background | `bg %1` |
| `multiWatch` | Run multiple monitored commands concurrently | `multiWatch ["date +%T", "ls | wc -l"]` |

The shell also supports the `$?` last-exit-status expansion for command results.

### 7.3 Pipelines and redirection

Commands can be connected with pipes:

```bash
ls -l | sort | head -5
```

Input can be redirected from a file:

```bash
sort < names.txt
```

Output can be redirected to a file:

```bash
ls > listing.txt
```

Input and output redirection can be combined:

```bash
sort < names.txt > sorted.txt
```

### 7.4 Multiline commands and substitution

When a command is incomplete at Enter, the interface switches to the continuation prompt:

```text
user@terminator> echo "first line \
> second line"
first line second line
```

The parser supports command substitution using `$(...)` and backticks, including nested substitutions:

```bash
echo "Current directory: $(basename "$(pwd)")"
```

Single-quoted text is treated literally; double-quoted text permits variable and command expansion.

### 7.5 Concurrent monitoring with `multiWatch`

Example:

```bash
multiWatch ["date +%T", "ls | wc -l"]
```

Each listed command/pipeline is launched independently. Output is collected as it becomes available and displayed in timestamped blocks. Each command is scheduled to run again after it completes, with a default repeat interval of approximately two seconds. Press `Ctrl+C` to stop the monitoring session and return to the prompt.

### 7.6 Background jobs

Append `&` to run a command in the background:

```bash
sleep 100 &
```

Inspect and manage jobs:

```bash
jobs
bg %1
fg %1
```

`Ctrl+C` sends an interrupt to the active foreground job. `Ctrl+Z` requests that the foreground job stop and returns control to the prompt.

### 7.7 History

- Commands are persisted in `~/.terminator_history`.
- The history store is bounded by `HISTORY_CAP` (10,000 entries).
- `history` displays up to the latest 1,000 entries.
- Consecutive duplicate commands are suppressed.
- `Ctrl+R` opens a search prompt. An exact match is preferred; otherwise the implementation searches for the best longest common contiguous substring match and requires a match longer than two characters.

### 7.8 File-name completion

Press `Tab` while editing a command to complete the token immediately before the cursor using entries from the active tab's current directory.

- One match: insert the matching name.
- Multiple matches: extend to their longest common prefix.
- Remaining ambiguity: display numbered candidates; press the corresponding number to select one.
- No match: leave the input unchanged.

The rest of the command line is preserved when the token is replaced.

---

## 8. Keyboard Shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl+T` | Create a new tab |
| `Alt+1` … `Alt+8` | Switch to a tab |
| `Ctrl+D` | Close the tab on an empty line / EOF behaviour |
| `Ctrl+C` | Interrupt the foreground process group |
| `Ctrl+Z` | Suspend the foreground process group |
| `Ctrl+A` | Move cursor to start of input |
| `Ctrl+E` | Move cursor to end of input |
| `←` / `→` | Move cursor one position |
| `↑` / `↓` | Browse command history |
| `Ctrl+U` | Delete from cursor to start of input |
| `Ctrl+K` | Delete from cursor to end of input |
| `Ctrl+W` | Delete the previous word |
| `Ctrl+L` | Clear the active tab's output |
| `Ctrl+R` | Search command history |
| `Tab` | Complete the current filename token |
| `Ctrl+Shift+C` | Copy selected output through X11 clipboard selection |
| `Ctrl+Shift+V` | Request and paste clipboard text |

---

## 9. Source Code Layout

```text
terminator/
├── main.c          # Startup, execution-mode setup, shared resources, cleanup
├── gui.c           # X11 window, rendering, tab lifecycle, event loop and workers
├── shell.c         # Per-tab working-directory handling and command dispatch
├── parser.c        # Tokenization, expansion, pipelines, redirection and job control
├── builtins.c      # Built-in commands, output buffers and persistent history
├── hotkeys.c       # Input editing, multiline handling, history search, completion
├── multiwatch.c    # Concurrent multiWatch execution and polling
├── terminator.h    # Application structures, limits and function declarations
├── Makefile        # Build rules and linker flags
├── README.md       # User-facing project documentation
└── DESIGN.md       # Internal architecture and design decisions
```

### Important structures

| Structure | Role |
|---|---|
| `App` | Owns global application state, X11 resources, tabs, jobs, history and execution-mode resources |
| `Tab` | Holds a tab's cwd, input, cursor, output, history-navigation state and worker communication |
| `TabSharedState` | Holds process-shared per-tab state such as cwd, foreground process group and busy flag |
| `SharedHistory` | Fixed-capacity shared history representation |
| `Job` | Tracks a background/stopped job and its process group/output state |
| `WatchCommand` | Tracks one monitored command/pipeline and its scheduling/output state |
| `MultiWatch` | Holds the active collection of watched commands |

---

## 10. Implementation Notes and Limits

The following are important when evaluating the current source tree:

1. **No PTY / terminal emulation layer.** Commands communicate with the GUI through pipes. This is sufficient for ordinary command output, but programs that require a real terminal device, raw-mode input, terminal ioctl operations, or full-screen TUI behaviour (for example, interactive `vim`, `top`, or `less`) are outside the current design.
2. **Hotkey remapping is not included.** The default editing and control shortcuts are implemented in the event handler, but the configuration-file-based remapping mechanism described in the specification is not present in this source tree.
3. **`source` is not implemented as a built-in.** It is therefore not listed as a supported command in this README.
4. **The standalone script interpreter is not included in the Task 1–10 feature set documented here.** Do not assume that `./script.sh` is interpreted by Terminator itself.
5. **Fixed resource limits.** The header defines limits including 8 tabs, 32 pipeline commands, 128 arguments per command, 32 jobs, and 16 watched commands. These are compile-time bounds rather than dynamically unlimited capacities.
6. **X11 is required for the GUI.** A headless shell-only mode is not provided.
7. **Shell compatibility is intentionally scoped.** Terminator implements the command syntax and behaviours covered by this project; it is not a drop-in replacement for Bash and does not claim complete POSIX-shell compatibility.

---

## 11. Troubleshooting

| Problem | Likely cause | Suggested action |
|---|---|---|
| `pkg-config` cannot find `xft` | Xft development package is missing | Install `libxft-dev` / `libXft-devel` / `libxft` for your distribution |
| `XOpenDisplay` fails or the window does not appear | No active X11 display or incorrect `DISPLAY` | Launch from an X11 session and check `echo "$DISPLAY"` |
| `make` cannot find X11 headers | Xlib development package is missing | Install the X11 development package |
| Font looks different | Preferred font is unavailable | Install DejaVu Sans Mono or use an installed monospace font |
| A full-screen program does not work correctly | No PTY/terminal emulation support | Use ordinary non-interactive commands; PTY support is outside this implementation |
| A command is not found | Executable is not in `PATH` or command spelling is incorrect | Check the command name and `echo "$PATH"` |

---

## 12. Concepts Demonstrated

This project brings together several systems-programming topics in one application:

- **Concurrency:** tab workers implemented as threads or processes.
- **Synchronization:** mutex and semaphore protection for shared history.
- **Shared memory:** `shm_open()` and `mmap()` in process mode.
- **IPC:** command and output pipes between the GUI and tab workers.
- **Process pipelines:** connecting child processes with `pipe()` and `dup2()`.
- **Job control:** process groups and explicit signal delivery.
- **Event-driven programming:** monitoring X11 and worker descriptors with `poll()`.
- **Filesystem APIs:** directory traversal for completion and per-tab cwd management.

For implementation details, data-flow explanations, synchronization choices, and process-lifecycle diagrams, see **[DESIGN.md](DESIGN.md)**.
