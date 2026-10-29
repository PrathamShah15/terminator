# Terminator (C + Xlib)

A beginner-oriented starting implementation for the CS69201 Terminator project. Source files are intentionally formatted with short lines and one responsibility per file.

## Build on Arch Linux / Hyprland

Install the X11 development library and a compiler:

```sh
sudo pacman -S --needed gcc make libx11 xorg-xwayland
```

From this directory:

```sh
make
./terminator
```

If Xlib reports that it cannot open the display, check that your Hyprland session has XWayland available and that `DISPLAY` is set.

## Files

- `main.c` — setup and cleanup of the X11 application.
- `gui.c` — drawing, text wrapping, and X11 event loop.
- `shell.c` — chooses built-ins or the command parser.
- `builtins.c` — built-ins (`cd`, `exit`, `history`, `clear`) and output/history buffers.
- `parser.c` — parses simple pipelines, quotes, wildcard patterns, and `<` / `>` redirection. Executes commands directly using `fork`, `execvp`, `pipe`, `dup2`, `open`, and `waitpid`.
- `hotkeys.c` — input editing, history navigation, tab switching and closing. Ctrl+T opens a tab and Alt+1 through Alt+8 switches tabs.
- Task 8 job control is implemented in `parser.c` and `builtins.c`: process groups, Ctrl+C, Ctrl+Z, background commands (`&`), and the `jobs`, `fg`, and `bg` built-ins.
- `terminator.h` — shared structures and declarations.
- `Makefile` — build rules.

## Current scope and limitations

This is a clean starter, not a complete Bash replacement. It does not use a PTY, `forkpty`, or execute a command through Bash or another shell. The project explicitly prohibits those shortcuts. Pipelines and redirection are handled by this program's parser.

Supported starter behavior: external commands, `cd`, `exit`, `history`, `clear`, `|`, `<`, `>`, basic quotes, simple multiline continuation for open quotes/trailing backslashes, wildcard expansion with `glob()`, basic command history and common editing keys.

Not yet implemented fully: Bash-compatible variable/command substitution (including nested `$()` and backticks), clipboard selection copy/paste, process-mode tabs, asynchronous command output while the GUI remains responsive, and robust job/signal control. These should be implemented as separate milestones rather than treated as finished requirements.


## Hotkey behavior

- `Ctrl+W`: delete the word immediately before the cursor (and whitespace before it).
- `Ctrl+D`: send EOF behavior. On an empty command line, close the current shell tab; if text remains and the cursor is before the end, delete the character under the cursor.
- `Ctrl+T`: open a new tab.


## Task 8 — Signals and job control

- `Ctrl+C` sends `SIGINT` to the foreground command's process group.
- `Ctrl+Z` sends `SIGTSTP` to the foreground command's process group and returns to the prompt.
- Add `&` at the end of a command to start it in the background, for example `sleep 30 &`.
- `jobs` lists known jobs; `fg %1` resumes job 1 in the foreground; `bg %1` resumes it in the background.
- Pipelines are placed in a process group so signals target all processes in that pipeline.

This is a beginner-oriented implementation of the required job-control behavior, not a complete POSIX shell. Test `sleep 30`, Ctrl+Z, `jobs`, `bg %1`, `fg %1`, and Ctrl+C in the X11 window.
