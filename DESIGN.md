# Terminator — Design Document

## 1. Project overview

Terminator is a graphical terminal emulator and shell-like command environment implemented in C using Xlib and Xft. It provides a tabbed interface, editable command input, terminal-style output rendering, command parsing, process execution, pipelines, redirection, job control, persistent history, filename completion, clipboard support, and repeated command monitoring through `multiWatch`.

The project implements its own command parsing and execution path. External programs are launched with `fork()` and `execvp()`; command strings are not delegated to `/bin/sh -c`.

The implementation is organized into seven C source modules and one shared header:

- `main.c`
- `gui.c`
- `shell.c`
- `parser.c`
- `builtins.c`
- `hotkeys.c`
- `multiwatch.c`
- `terminator.h`

## 2. Architecture at a glance

```text
                         +----------------------+
                         |       main.c         |
                         | Init app, X11, Xft   |
                         +----------+-----------+
                                    |
                                    v
                         +----------------------+
                         |       gui.c          |
                         | X11 event loop       |
                         | Render tabs/output   |
                         +----------+-----------+
                                    |
                      Keyboard / Enter / events
                                    |
                                    v
                         +----------------------+
                         |      hotkeys.c       |
                         | Input editing        |
                         | History/completion   |
                         | Clipboard shortcuts |
                         +----------+-----------+
                                    |
                                    v
                         +----------------------+
                         |       shell.c        |
                         | Tab cwd + dispatch   |
                         +----------+-----------+
                                    |
                       +------------+------------+
                       |                         |
                       v                         v
             +------------------+      +---------------------+
             |   builtins.c     |      |      parser.c       |
             | Built-in commands|      | Parse and expand    |
             | History/output   |      | Execute pipelines   |
             +------------------+      | Jobs and redirection|
                                       +----------+----------+
                                                  |
                                                  v
                                       +---------------------+
                                       | Child processes     |
                                       | fork / execvp       |
                                       | pipes / waitpid     |
                                       +---------------------+

                         +----------------------+
                         |     multiwatch.c     |
                         | Repeated pipelines   |
                         | Polling and output   |
                         +----------------------+
```

## 3. Module responsibilities

### 3.1 `main.c` — initialization and shutdown

`main.c` creates and initializes the application state.

Responsibilities include:

- Initialize the shared `App` structure.
- Load persistent command history.
- Open the X11 display with `XOpenDisplay()`.
- Create the application window and set its title.
- Configure window dimensions, colors, and event masks.
- Open the Xft font and allocate text colors.
- Create the graphics context and initial backbuffer resources.
- Initialize clipboard-related X11 atoms.
- Initialize the first tab and its working directory.
- Start the GUI loop through `gui_run()`.
- Save history and release allocated resources during shutdown.

The application requests the `DejaVu Sans Mono` font. The project requires an X11-compatible display; on a Wayland desktop this is provided through XWayland.

### 3.2 `gui.c` — window, rendering, and event loop

`gui.c` owns the visible terminal interface.

Responsibilities:

- Draw the tab bar, active-tab appearance, close controls, and new-tab control.
- Render the prompt, command input, cursor, and output.
- Wrap output lines to fit the available window width.
- Render the visible portion of the output according to the scroll offset.
- Handle window resizing and redraw requests.
- Maintain the off-screen Pixmap backbuffer.
- Handle mouse-wheel scrolling.
- Map mouse coordinates to output positions for text selection.
- Serve X11 clipboard selection requests.
- Process X11 events while foreground commands are running.
- Poll background jobs and `multiWatch` activity.

The prompt is rendered in the form:

```text
pratham@terminator:/current/working/directory$
```

The prompt's path is taken from the active tab's saved working directory.

#### Backbuffer rendering

Instead of repeatedly drawing partial updates directly to the visible window, the GUI renders a complete frame into an off-screen X11 Pixmap. The completed Pixmap is copied to the window. This reduces flicker and helps keep redraw behavior stable.

The renderer calculates text widths using Xft font metrics. Long output lines are wrapped into display lines based on the available content width. The scroll offset selects which wrapped lines are visible.

#### Mouse selection

The GUI maps pointer coordinates to a wrapped display line and character column. Mouse dragging updates the selection anchor and endpoint. `gui_copy_selection()` reconstructs the selected text from the displayed output and places it in the application's clipboard text.

### 3.3 `shell.c` — dispatch and per-tab working directory

`shell_execute()` ensures a command runs in the working directory belonging to its tab.

Its sequence is:

1. Save the process's current working directory.
2. Change to the active tab's stored directory, if available.
3. Check whether the command is a built-in.
4. Dispatch built-ins to `builtin_execute()`; otherwise call `parser_execute_pipeline()`.
5. Read the resulting current directory and store it in the tab.
6. Restore the process's original working directory.

This allows multiple tabs to maintain different working directories even though all tabs belong to the same application process.

### 3.4 `builtins.c` — built-ins, output, and history

This module provides the tab output-buffer helpers, persistent history, and built-in command implementation.

Important functions:

- `tab_append()` appends text to a tab's output buffer.
- `tab_clear()` clears a tab's output.
- `history_load()` reads the saved history file.
- `history_add()` adds a command to in-memory history and persists it.
- `history_save()` writes retained history to disk.
- `history_free()` releases history memory.
- `builtin_execute()` handles the built-ins implemented in this module.

Built-ins handled here include `cd`, `clear`, `exit`, `history`, and `multiWatch`. Job-control commands (`jobs`, `fg`, and `bg`) are handled by `job_control_builtin()` in `parser.c`.

The history file is:

```text
~/.terminator_history
```

The history capacity is `HISTORY_CAP` (10,000 entries). Consecutive duplicate commands are skipped. The `history` command displays up to the most recent 1,000 entries.

### 3.5 `hotkeys.c` — input editing, completion, search, and shortcuts

This module handles keyboard-driven interaction with the active tab.

Responsibilities:

- Insert typed text at the current input cursor.
- Move the cursor and delete input ranges.
- Handle command submission and multiline continuation.
- Navigate command history with the Up and Down keys.
- Implement `Ctrl+R` history search.
- Implement filename/path completion.
- Handle clipboard copy and paste shortcuts.
- Create, switch, and close tabs through keyboard actions.
- Clear transient completion and history-search state.

#### Multiline input

Before submitting input, the handler checks whether the command appears incomplete. It tracks quote state, nested command-substitution parentheses, and a trailing backslash continuation. Incomplete input is accumulated in the tab's multiline buffer until the command is complete.

#### Filename completion

Completion identifies the word immediately before the input cursor and searches the active tab's working directory or the directory component included in that word. Up to `MAX_COMPLETIONS` candidates are stored.

- First `Tab`: discover and display candidates.
- Repeated `Tab`: cycle through candidates.
- Current candidate: highlighted in the completion display.
- `Enter`: accept the selected candidate into the input line.
- A subsequent `Enter`: execute the command.
- `Esc`: cancel completion.

The implementation preserves text after the cursor when replacing the completion range.

#### Searchable history

`Ctrl+R` activates a search prompt. The search first looks for the most recent exact command match. If there is no exact match, it scores history entries using the longest common contiguous substring. Matches are printed when the score exceeds two characters. `Esc` cancels the search.

#### Clipboard

`Ctrl+Shift+C` copies selected terminal output. `Ctrl+Shift+V` requests the X11 `CLIPBOARD` selection and inserts received text at the input cursor. The application uses X11 selection atoms and a property to transfer clipboard data.

### 3.6 `parser.c` — parser, expansion, pipelines, and jobs

`parser.c` is the command-processing core. It parses the command line, performs supported expansions, constructs argument vectors, creates pipes, launches child processes, collects output, and tracks background or stopped jobs.

The parser uses a command representation containing:

- An argument vector (`argv`).
- An argument count (`argc`).
- An optional input filename.
- An optional output filename.

The limits are defined in `terminator.h`, including `MAX_COMMANDS` (32), `MAX_ARGS` (128), and `MAX_JOBS` (32).

#### Environment-variable expansion

The parser supports:

```bash
$HOME
${HOME}
```

It obtains values using `getenv()`. An unset variable expands to an empty string. Expansion is not intended to reproduce every Bash expansion rule.

#### Command substitution

The parser recognizes `$(...)`. The matching-parenthesis scanner accounts for nested parentheses, nested command substitutions, quotes, and backslash escapes.

Command substitution is implemented using the project's own parser/executor:

1. Create a pipe to capture the inner command's standard output.
2. Fork a child process.
3. Redirect the child's standard output to the pipe.
4. Execute the inner command through the internal pipeline executor in capture mode.
5. Read the captured bytes.
6. Remove trailing newline characters.
7. Insert the resulting text into the outer command before parsing/execution continues.

This does not invoke `/bin/sh -c`. Background commands inside command substitution are rejected. The feature is a limited shell-like implementation, not full subshell compatibility.

#### Word parsing and wildcard expansion

The word parser handles whitespace-separated arguments, single and double quotes, backslash escapes, variable expansion, and command substitution. Filename patterns are expanded using `glob()`.

#### Pipeline construction

The pipeline splitter separates commands at unquoted `|` characters. A pipeline is limited to `MAX_COMMANDS` stages.

For each stage, the executor creates the required pipe connections:

- The first stage reads from the original standard input unless input redirection is specified.
- A middle stage reads from the previous stage's pipe and writes to the next stage's pipe.
- The last stage writes to the pipeline output or the configured output destination.

Child processes use `dup2()` to connect the relevant file descriptors, close unused descriptors, and execute their argument vectors with `execvp()`.

#### Input and output redirection

The parser supports:

- `< filename` — open a file for reading and connect it to standard input.
- `> filename` — create or truncate a file and connect it to standard output.

The file descriptors are installed with `dup2()`. The implementation supports the redirection operators above; it should not be assumed to support every shell redirection form.

#### Foreground execution

Foreground commands are launched as a process group. The parent collects output through a pipe and waits for the process group. While waiting, the application continues polling X11 events so that the window can still process redraws and user interactions.

The active foreground process-group identifier and owning tab index are stored in `App`, allowing interrupt and suspend shortcuts to target the correct foreground job.

#### Background execution and job control

A command ending in `&` is launched as a background job. Background jobs are tracked in the `App.jobs` array. Each job stores its process-group ID, output file descriptor, active/stopped state, owning tab index, and command text.

The job table is bounded by `MAX_JOBS`.

- `jobs` lists tracked jobs.
- `bg` sends `SIGCONT` to a stopped job's process group.
- `fg` resumes a job and waits for it in the foreground.
- `Ctrl+C` sends an interrupt to the active foreground process group.
- `Ctrl+Z` suspends the active foreground process group.

Background commands without explicit input redirection receive `/dev/null` as standard input so they do not consume input intended for the application.

### 3.7 `multiwatch.c` — repeated command monitoring

`multiwatch.c` implements the `multiWatch` built-in's repeated execution mechanism.

The input is a list of quoted command strings, for example:

```text
multiWatch ["date +%T", "ls | wc -l"]
```

The module:

1. Parses the command list.
2. Stores each command and the tab's working directory.
3. Splits each command into pipeline stages.
4. Launches each watched command as a pipeline.
5. Captures each command's output.
6. Polls child processes and output descriptors.
7. Schedules the next run for two seconds after the previous run completes.

The commands in a session are launched concurrently. The application supports one active `multiWatch` session, with up to `MAX_WATCH_COMMANDS` commands. `multiwatch_stop()` terminates the active monitoring session.

## 4. Shared data structures

The shared definitions are in `terminator.h`.

### 4.1 `Tab`

Each `Tab` stores the state of one terminal tab:

| Field | Purpose |
|---|---|
| `cwd` | Tab's current working directory. |
| `input` | Editable command input buffer. |
| `cursor` | Current input cursor position. |
| `output` | Accumulated terminal output. |
| `output_len` | Number of bytes currently used in output. |
| `multiline` | Accumulated incomplete command. |
| `continuing` | Whether multiline input is active. |
| `history_index` | Position used for history navigation. |
| `scroll_offset` | Output scroll position. |
| `alive` | Whether the tab is active. |

The tab array is bounded by `MAX_TABS` (8). Input and output capacities are `INPUT_CAP` (4,096) and `OUTPUT_CAP` (65,536).

### 4.2 `Job`

A `Job` contains:

- `pgid`: process-group ID.
- `output_fd`: descriptor used to collect background output.
- `active`: whether the job slot is in use.
- `stopped`: whether the job is suspended.
- `tab_index`: tab associated with the job.
- `command`: original command text.

### 4.3 `WatchCommand` and `MultiWatch`

`WatchCommand` stores the command string, working directory, process-group ID, child PIDs, process count, output descriptor, active/running flags, header state, and next scheduled run time.

`MultiWatch` stores whether monitoring is active, the owning tab index, the number of watched commands, and the fixed array of watch-command records.

### 4.4 `App`

`App` contains application-wide state:

- X11 display, window, graphics context, Xft font, Xft draw object, and backbuffer.
- Xft colors and X11 color values.
- Window dimensions and tab array.
- Active-tab and tab-count fields.
- Shared persistent-history storage.
- Foreground process-group and owning-tab information.
- Background job table and next job ID.
- History-search state.
- Filename-completion candidates and selection range.
- Clipboard text and X11 clipboard atoms.
- Mouse-selection anchor and endpoint.
- `multiWatch` state and application running/exit flags.

## 5. Main execution flows

### 5.1 Application startup

```text
main()
  |
  +--> initialize App
  |
  +--> history_load()
  |
  +--> XOpenDisplay()
  |
  +--> create window / GC / Xft resources
  |
  +--> initialize clipboard atoms
  |
  +--> initialize first Tab and cwd
  |
  +--> gui_run()
  |
  +--> save history and release resources
```

### 5.2 Command submission

```text
Enter
 |
 v
hotkey_handle()
 |
 +--> Is multiline input incomplete?
 |       |
 |       +--> Yes: append to Tab.multiline
 |       |
 |       +--> No: assemble complete command
 |
 v
append command/prompt to visible output
 |
 v
shell_execute()
 |
 +--> built-in? ---- Yes ---> builtin_execute()
 |       |
 |       No
 |       |
 |       v
 |   parser_execute_pipeline()
 |
 v
append command output to owning Tab
 |
 v
gui_redraw()
```

### 5.3 External pipeline execution

```text
Command line
    |
    v
Expand command substitutions
    |
    v
Parse words and redirections
    |
    v
Split pipeline into stages
    |
    v
Create pipes and process group
    |
    v
Fork each stage
    |
    +--> dup2() input/output descriptors
    |
    +--> execvp()
    |
    v
Parent collects output and waits / tracks job
    |
    v
Append output to tab buffer
```

## 6. Important constants and capacity limits

| Constant | Value | Meaning |
|---|---:|---|
| `MAX_TABS` | 8 | Maximum tab slots. |
| `INPUT_CAP` | 4,096 | Input and command buffer capacity. |
| `OUTPUT_CAP` | 65,536 | Per-tab output buffer capacity. |
| `HISTORY_CAP` | 10,000 | Maximum in-memory history entries. |
| `MAX_COMMANDS` | 32 | Maximum pipeline stages / command slots. |
| `MAX_ARGS` | 128 | Argument-vector capacity per command. |
| `MAX_JOBS` | 32 | Background/stopped job slots. |
| `MAX_WATCH_COMMANDS` | 16 | Commands in one multiWatch session. |
| `WATCH_OUTPUT_CAP` | 8,192 | Watch-output capacity constant. |
| `MAX_COMPLETIONS` | 128 | Filename completion candidates. |

## 7. Design decisions

### Own parser and executor

Command interpretation is performed by the project rather than delegated to Bash. This keeps parsing, expansion, pipelines, redirection, and process creation visible in the codebase and demonstrates the underlying operating-system interfaces.

### Separate GUI and execution modules

The GUI and shell logic are separated so that rendering and X11 event handling do not need to be mixed into parsing and process creation. Shared state and function declarations are kept in `terminator.h`.

### Per-tab current directory

Each tab stores its own `cwd`. `shell_execute()` temporarily changes the process working directory to the tab's directory, runs the command, saves any updated directory, and restores the previous process directory.

### Pipes rather than a PTY

The project uses pipes to transfer command output and input between processes. It does not create a pseudo-terminal. This keeps the process and IPC implementation explicit, but limits compatibility with interactive terminal applications.

### Event processing during foreground waits

The foreground wait path continues processing GUI events while the command executes. This prevents the window from appearing frozen and allows tab interaction and redraw handling during long-running foreground commands.

### Persistent history

History is stored in the user's home directory so it survives application restarts. The in-memory limit prevents unbounded growth during a session.

## 8. Testing checklist

The following commands and interactions exercise the main implemented features.

### Basic command execution

```bash
pwd
ls -la
echo hello
```

### Environment variables and command substitution

```bash
echo "$HOME"
echo "${HOME}"
echo "$(pwd)"
echo "Files in $(basename $(pwd)): $(ls | wc -l)"
```

### Pipelines and redirection

```bash
ls | wc -l
cat < input.txt
echo hello > output.txt
cat input.txt | grep error | wc -l
```

### Background jobs and job control

```bash
sleep 20 &
jobs
```

Then test `fg` and `bg` with a stopped or running job, and test `Ctrl+C` / `Ctrl+Z` on foreground commands.

### multiWatch

```text
multiWatch ["date +%T", "ls | wc -l"]
```

Verify that commands run concurrently and repeat two seconds after their previous execution completes. Press `Ctrl+C` to stop the session.

### History and completion

- Run several commands, restart Terminator, and verify that history remains.
- Use Up/Down to navigate history.
- Use `Ctrl+R` to search history.
- Type a partial filename and press `Tab` repeatedly to cycle through completions.
- Use `Enter` to accept a candidate and then execute.

### GUI and tabs

- Create tabs with `Ctrl+T` and the `+` control.
- Switch with `Alt+1` through `Alt+8`.
- Change directories in separate tabs and verify that each tab retains its own directory.
- Resize the window and verify wrapping/redraw.
- Scroll output with the mouse wheel.
- Select output with the mouse and test `Ctrl+Shift+C` / `Ctrl+Shift+V`.

## 9. Known limitations

- Terminator is not a complete Bash or POSIX shell implementation.
- The application does not currently use a PTY. Interactive full-screen programs such as `vim`, `top`, and `less` are therefore not supported as they are in a conventional terminal.
- The parser supports only the syntax implemented in `parser.c`; shell expansion and quoting are not guaranteed to match Bash in every case.
- Background commands inside command substitution are rejected.
- Only one `multiWatch` session can be active at a time.
- The application requires X11 or XWayland.
- The source archive does not include a license file.

## 10. Build configuration

The Makefile uses:

```make
CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -g -D_GNU_SOURCE
CFLAGS += $(shell pkg-config --cflags xft)
LDLIBS = -lX11 $(shell pkg-config --libs xft)
```

Source files:

```text
main.c gui.c shell.c parser.c builtins.c hotkeys.c multiwatch.c
```

Build and run:

```bash
make
./terminator
```

Clean:

```bash
make clean
```
