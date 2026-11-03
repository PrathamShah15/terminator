# Terminator — Design and Architecture Document

**Computing Lab (CS69201) · Term Project**  
**Implementation language:** C11 · **Platform:** Linux · **GUI:** Xlib/Xft

---

## Table of Contents

1. [Purpose and Scope](#1-purpose-and-scope)
2. [High-Level Architecture](#2-high-level-architecture)
3. [Module Responsibilities](#3-module-responsibilities)
4. [Core Data Structures](#4-core-data-structures)
5. [X11 GUI and Event Loop](#5-x11-gui-and-event-loop)
6. [Tab Execution Models](#6-tab-execution-models)
7. [IPC and Shared-State Synchronization](#7-ipc-and-shared-state-synchronization)
8. [Command Processing Pipeline](#8-command-processing-pipeline)
9. [Pipelines and I/O Redirection](#9-pipelines-and-io-redirection)
10. [Process Groups, Signals and Job Control](#10-process-groups-signals-and-job-control)
11. [multiWatch Design](#11-multiwatch-design)
12. [History Design](#12-history-design)
13. [Line Editing, Completion and Clipboard](#13-line-editing-completion-and-clipboard)
14. [Resource Limits and Error Handling](#14-resource-limits-and-error-handling)
15. [Design Trade-offs and Known Gaps](#15-design-trade-offs-and-known-gaps)

---

## 1. Purpose and Scope

Terminator is a multi-tab graphical terminal emulator and custom command interpreter implemented in C. The design is centred on demonstrating operating-system interfaces directly rather than embedding an existing interactive shell inside a pseudo-terminal.

The implementation combines:

- An X11 window and event-driven input/output interface.
- Independent tab execution contexts.
- Two tab-worker models: POSIX threads and separate processes.
- Pipe-based communication between workers and the GUI.
- A parser and process launcher for commands, pipelines, substitutions and redirection.
- Foreground/background process supervision and signal delivery.
- Shared persistent command history.
- File-name completion and interactive line editing.

### Scope against the supplied specification

This document describes the submitted source tree and its Task 1–10 feature areas. Two specification details should be distinguished from implemented behaviour:

- The default hotkeys are implemented, but the configurable `~/.terminatorrc` key-binding loader is not present.
- The `source` built-in named in the Task 2 command list is not implemented in the submitted built-in dispatcher.

The script-interpreter requirement shown after Task 10 in the supplied PDF is not described as implemented by this project version.

---

## 2. High-Level Architecture

### 2.1 Component diagram

```text
                           +---------------------------+
                           |           main.c          |
                           | startup, X11 setup, mode  |
                           | selection, shared state   |
                           +-------------+-------------+
                                         |
                                         v
                           +---------------------------+
                           |           gui.c           |
                           | X11 rendering, tabs,      |
                           | poll()-driven event loop  |
                           +-------------+-------------+
                                         |
                  +----------------------+----------------------+
                  |                                             |
          THREAD MODE                                    PROCESS MODE
                  |                                             |
        +---------v----------+                        +---------v----------+
        | Tab worker thread  |                        | Tab worker process |
        | pthread_create()   |                        | fork()             |
        +---------+----------+                        +---------+----------+
                  |                                             |
                  +----------------------+----------------------+
                                         |
                                         v
                           +---------------------------+
                           |          shell.c          |
                           | cwd isolation and         |
                           | command dispatch         |
                           +-------------+-------------+
                                         |
                          +--------------+--------------+
                          |                             |
                 +--------v--------+           +--------v--------+
                 |   builtins.c    |           |    parser.c      |
                 | built-in logic  |           | parser, expansion|
                 | and history     |           | launch, pipes,   |
                 +-----------------+           | redirection, jobs|
                                               +--------+--------+
                                                        |
                                              +---------+---------+
                                              |                   |
                                        +-----v-----+       +-----v-----+
                                        | child #1  | pipe  | child #2  |
                                        | execvp()  |------>| execvp()  |
                                        +-----------+       +-----------+

             hotkeys.c: input editor, completion, history search, clipboard
             multiwatch.c: watched pipelines, output polling, repeat timer
             terminator.h: structures, constants, interfaces
```

### 2.2 Main data-flow paths

**User command path**

```text
X11 KeyPress
    |
    v
hotkey_handle()
    |
    v
Tab input buffer
    |
    | Enter (and multiline check)
    v
tab_send_command()
    |
    +---- thread mode: shell_execute() in worker thread
    |
    +---- process mode: framed command over pipe_to_tab
                                  |
                                  v
                            tab_worker_loop()
                                  |
                                  v
                            shell_execute()
                                  |
                    +-------------+-------------+
                    |                           |
                built-in                  parser_execute_pipeline()
                                                |
                                                v
                                       fork / pipe / execvp
```

**Command output path**

```text
Child process stdout/stderr
          |
          v
     pipe write end
          |
          v
Tab worker output
          |
          v
tab_append() / pipe_from_tab
          |
          v
GUI poll() -> read()
          |
          v
tab_append_raw()
          |
          v
Per-tab output buffer -> gui_redraw()
```

The X11 display connection is owned by the GUI. Worker contexts do not draw directly into the X11 window; they communicate text through pipes.

---

## 3. Module Responsibilities

| File | Main responsibility | Important functions / concepts |
|---|---|---|
| `main.c` | Startup, mode selection, X11 resources, shared-memory/semaphore setup and cleanup | `main()`, `setup_execution_mode()`, `cleanup_execution_mode()` |
| `gui.c` | X11 window, rendering, tab lifecycle, worker loops and event multiplexing | `gui_run()`, `gui_redraw()`, `create_tab()`, `close_tab()`, `tab_worker_loop()` |
| `shell.c` | Per-tab current-directory handling and command dispatch | `shell_execute()` |
| `parser.c` | Command parsing, substitutions, pipeline creation, redirection, child waiting and job control | `parse_words()`, `split_pipeline()`, `parser_execute_pipeline()`, `wait_foreground()` |
| `builtins.c` | Built-in command handling, output buffering and history persistence | `builtin_execute()`, `history_add()`, `history_load()`, `history_save()` |
| `hotkeys.c` | Keyboard editing, multiline detection, history search, filename completion and clipboard operations | `hotkey_handle()`, `command_is_incomplete()`, `filename_completion()` |
| `multiwatch.c` | Concurrent watched command/pipeline execution and rescheduling | `multiwatch_start()`, `multiwatch_poll()`, `multiwatch_stop()` |
| `terminator.h` | Shared structures, compile-time limits and function declarations | `App`, `Tab`, `Job`, `SharedHistory`, `MultiWatch` |
| `Makefile` | Compilation and linking | GCC, C11, Xft, X11, pthread, rt |

The modules are separated so that GUI concerns, shell dispatch, parsing, input editing and monitoring do not need to be implemented in one source file.

---

## 4. Core Data Structures

The central structures are declared in `terminator.h`.

### 4.1 `App`

`App` represents the application-wide state. It owns:

- X11 resources: `Display`, `Window`, `GC`, Xft font/drawing resources and backbuffer Pixmap.
- Window dimensions and colour configuration.
- The fixed-size array of tabs and the active-tab index.
- Job-control entries and the next job identifier.
- `multiWatch` state.
- Execution-mode selection and history synchronization resources.
- Shared history pointers and process-shared per-tab state.
- Reverse-history-search state and filename-completion candidates.
- Clipboard and mouse-selection state.

This structure is passed to the GUI, shell and worker routines so they operate on the same application context (or the inherited/shared portions of it in process mode).

### 4.2 `Tab`

Each `Tab` stores the state needed to preserve a shell session independently:

| Field | Purpose |
|---|---|
| `cwd` | Current working directory for the tab |
| `input` | Editable command line |
| `cursor` | Cursor position in the input buffer |
| `output` / `output_len` | Renderable output text and its current length |
| `multiline` / `continuing` | Accumulated incomplete command state |
| `history_index` | Position used when navigating history |
| `scroll_offset` | Output scroll position |
| `alive` | Tab worker/lifecycle state |
| `tab_id` | Tab identity |
| `thread_id` | Worker thread identifier in thread mode |
| `process_id` | Worker process identifier in process mode |
| `pipe_to_tab` | GUI-to-worker command channel |
| `pipe_from_tab` | Worker-to-GUI output channel |
| `shared` | Pointer to process-shared tab metadata, when available |
| `app_ref` | Application context pointer |

### 4.3 `TabSharedState`

```c
typedef struct {
    char cwd[PATH_MAX];
    pid_t foreground_pgid;
    int busy;
} TabSharedState;
```

This structure contains small per-tab values that must be visible across process boundaries in process mode. The pool is mapped with shared anonymous memory.

### 4.4 `SharedHistory`

```c
typedef struct {
    int count;
    char lines[HISTORY_CAP][INPUT_CAP];
} SharedHistory;
```

The history structure has a fixed capacity of `HISTORY_CAP` command strings. In process mode it is backed by a POSIX shared-memory object; in thread mode the application uses its in-process history representation and mutex-protected access.

### 4.5 `Job`

A `Job` records the process group and output/lifecycle information needed to list, resume and wait for background or stopped work.

| Field | Purpose |
|---|---|
| `pgid` | Process-group identifier for the job |
| `output_fd` | Output descriptor associated with the job |
| `active` | Whether the slot is in use |
| `stopped` | Whether the job is currently stopped |
| `tab_index` | Tab associated with the job |
| `command` | Original command text |

### 4.6 `WatchCommand` and `MultiWatch`

`WatchCommand` tracks one watched command or pipeline: its text, working directory, process group, stage PIDs, output descriptor, running state, output-header state and next scheduled run time.

`MultiWatch` groups the active watched commands and records the tab in which the monitoring session is running.

### 4.7 Compile-time capacities

| Constant | Value | Meaning |
|---|---:|---|
| `MAX_TABS` | 8 | Maximum number of tabs |
| `INPUT_CAP` | 4,096 | Input/command buffer size |
| `OUTPUT_CAP` | 65,536 | Per-tab output buffer capacity |
| `HISTORY_CAP` | 10,000 | Maximum history entries |
| `MAX_COMMANDS` | 32 | Maximum pipeline stages |
| `MAX_ARGS` | 128 | Argument slots per command |
| `MAX_JOBS` | 32 | Job-table slots |
| `MAX_WATCH_COMMANDS` | 16 | Watched command slots |
| `WATCH_OUTPUT_CAP` | 8,192 | Watch output buffer capacity |
| `MAX_COMPLETIONS` | 128 | Completion candidate bound |

These are implementation limits, not claims of unlimited capacity.

---

## 5. X11 GUI and Event Loop

### 5.1 Window and rendering

The GUI is implemented in `gui.c` using Xlib and Xft.

- `XOpenDisplay()` connects the application to the X server.
- The application creates an X11 window and subscribes to keyboard, mouse, expose and resize events.
- `XLookupString()` translates key events into key symbols and character input.
- Xft is used for UTF-8-aware text measurement and drawing.
- The tab bar, prompt, input line, cursor and output are drawn by the application.

The tab bar displays each tab's index and current directory, along with a `+` control. Clicking a tab activates it; clicking its close region closes it. A mouse wheel changes the output scroll position.

### 5.2 Output buffer and wrapping

Each tab stores its output in `Tab.output`. Rendering code converts the buffer into display lines based on the available pixel width and Xft font metrics. This separates the logical output text from the visual wrapped-line layout.

The output buffer is bounded by `OUTPUT_CAP`. When new text would exceed the capacity, older bytes are discarded to make room for recent output. This is a bounded rolling-output strategy rather than an unbounded terminal scrollback store.

### 5.3 Backbuffer rendering

The GUI draws a complete frame into an off-screen Pixmap and copies the finished frame to the window. This reduces visible flicker caused by drawing many interface elements directly to the visible window.

Conceptually:

```text
Application state
      |
      v
Build display lines / calculate positions
      |
      v
Draw background, tabs, output, prompt and cursor
      |
      v
Off-screen Pixmap (backbuffer)
      |
      v
XCopyArea() to visible X11 window
      |
      v
XFlush()
```

The backbuffer is recreated or resized when the window dimensions change.

### 5.4 Event multiplexing with `poll()`

The GUI must respond to both X11 events and output produced by tab workers. It therefore monitors the X11 connection descriptor and active tab output-pipe descriptors together.

Simplified representation of the event-loop setup:

```c
struct pollfd fds[1 + MAX_TABS];

fds[0].fd = ConnectionNumber(app->display);
fds[0].events = POLLIN;

for (int i = 0; i < app->tab_count; ++i) {
    Tab *tab = &app->tabs[i];

    if (tab->alive && tab->pipe_from_tab[0] >= 0) {
        fds[nfds].fd = tab->pipe_from_tab[0];
        fds[nfds].events = POLLIN | POLLHUP;
        ++nfds;
    }
}

poll(fds, nfds, timeout);
```

The actual loop also checks whether Xlib already has queued events before waiting. When a tab output descriptor becomes readable, the GUI reads available bytes, appends them to the corresponding tab's buffer and redraws.

### 5.5 Event handling responsibilities

| X11 event / input | Handling |
|---|---|
| `KeyPress` | Translate key and delegate to `hotkey_handle()` |
| `Expose` | Redraw the window |
| `ConfigureNotify` | Update dimensions and redraw |
| `ButtonPress` | Tab selection, close/add controls, output selection and scrolling |
| `ButtonRelease` | Finish mouse selection |
| `MotionNotify` | Update active text selection |
| `SelectionRequest` | Respond to another X11 client requesting clipboard data |
| `SelectionNotify` | Complete a clipboard paste request |

The GUI loop also polls job and `multiWatch` state so that asynchronous activity can update the display.

---

## 6. Tab Execution Models

Each tab uses a worker context to execute shell commands. The application offers two ways to create that context.

### 6.1 Thread mode

```text
                     One application process
       +------------------------------------------------+
       |                                                |
       |  Main thread                                   |
       |  +------------------------------------------+  |
       |  | X11 event loop and GUI rendering         |  |
       |  +------------------------------------------+  |
       |             | pipe output / command input     |
       |      +------+-------------------+             |
       |      |                          |             |
       |  +---v-----------+          +---v-----------+ |
       |  | Tab 0 worker  |          | Tab 1 worker  | |
       |  | pthread       |          | pthread       | |
       |  +---------------+          +---------------+ |
       |                                                |
       |       Shared process memory                    |
       |       history protected by mutex                |
       +------------------------------------------------+
```

In the default mode:

1. `create_tab()` allocates and initializes the tab state and its pipes.
2. `pthread_create()` starts `tab_thread_worker()`.
3. The worker enters `tab_worker_loop()` and waits for command messages.
4. The GUI and worker threads share the same address space.
5. History access is serialized using `history_mutex`.

The worker threads are detached. This means the application does not join each worker during normal tab operation; tab lifecycle is coordinated through the tab's state and communication descriptors.

### 6.2 Process mode

```text
                Parent process (GUI)
        +----------------------------------+
        | X11 connection and event loop    |
        | tab buffers and rendering        |
        +----------------+-----------------+
                         |
            per-tab command/output pipes
                         |
              +----------+----------+
              |                     |
       +------v------+       +------v------+
       | Tab process |       | Tab process |
       | worker loop |       | worker loop |
       +------+------+       +------+------+
              |                     |
              +----------+----------+
                         |
        +----------------v----------------+
        | Shared history: shm_open/mmap   |
        | Shared tab metadata: mmap       |
        | History lock: POSIX semaphore   |
        +---------------------------------+
```

In process mode:

1. The GUI creates a tab's command and output pipes.
2. `fork()` creates a child worker process.
3. The child closes the pipe ends it does not use and closes its inherited X11 connection descriptor.
4. The child runs `tab_worker_loop()`.
5. The parent retains the opposite pipe ends and monitors worker output in the GUI loop.

The worker process does not own or render the X11 interface.

### 6.3 Mode comparison

| Property | Thread mode | Process mode |
|---|---|---|
| Tab worker | POSIX thread | Child process |
| Creation API | `pthread_create()` | `fork()` |
| Address space | Shared | Separate, except explicitly shared mappings |
| History lock | `pthread_mutex_t` | Named POSIX semaphore |
| Shared history backing | In-process representation | POSIX shared memory object |
| GUI ownership | Main thread | Parent process |
| Worker communication | Per-tab pipes | Per-tab pipes |
| Isolation | Shared-process failure domain | Separate worker process failure domain |

A process worker has a separate address space, but data that must be shared is deliberately placed in shared mappings. Ordinary heap memory is not automatically shared after `fork()`.

---

## 7. IPC and Shared-State Synchronization

### 7.1 Per-tab pipes

Each tab has two unidirectional pipes:

| Pipe | Direction | Purpose |
|---|---|---|
| `pipe_to_tab` | GUI → worker | Send a command for execution |
| `pipe_from_tab` | Worker → GUI | Send command output and status text |

In process mode, the command channel uses a length-prefixed message: the sender writes an integer length followed by the command bytes. The worker reads the length and then reads the complete command payload.

The output channel carries text. The GUI marks its read end non-blocking and uses `poll()` before reading so that one inactive tab does not block updates for other tabs.

### 7.2 Why two pipes?

A pipe is unidirectional. Separating command input from output makes ownership and data flow explicit:

```text
GUI                                  Worker
 |                                     |
 |------ command length -------------->|
 |------ command bytes ---------------->|
 |                                     |
 |<----- output bytes -----------------|
 |<----- output bytes -----------------|
```

This also prevents command messages and output text from being confused with each other.

### 7.3 Shared history in thread mode

Threads share the same process memory. The history mutex protects critical sections that read or modify the shared history state.

The intended critical-section pattern is:

```text
lock history_mutex
    read / append / update history
unlock history_mutex
```

This prevents concurrent tab workers from modifying the same history representation at the same time.

### 7.4 Shared history in process mode

Separate processes do not share ordinary heap variables. Process mode therefore creates:

- A POSIX shared-memory object named `/terminator_history_shm`.
- A mapping of that object into the participating processes using `mmap()` with `MAP_SHARED`.
- A named POSIX semaphore `/terminator_history_sem` to serialize history access.

The history lock is conceptually:

```text
sem_wait(history_sem)
    read / append / update shared history
sem_post(history_sem)
```

The shared mapping provides visibility; the semaphore provides mutual exclusion. Neither mechanism replaces the other.

### 7.5 Shared per-tab metadata

`TabSharedState` values are placed in an anonymous shared mapping (`MAP_SHARED | MAP_ANONYMOUS`). This gives the parent and worker processes a common location for fields such as:

- The tab's current directory.
- The foreground process-group identifier.
- Whether the worker is busy.

The main `Tab` structure also contains process-local GUI and communication state. Only the explicitly shared mapping is intended to provide cross-process visibility.

### 7.6 Working-directory isolation

The process-wide current directory is not a suitable storage location for independent tabs in thread mode: all threads share one process cwd. `shell_execute()` therefore saves the current cwd, changes to the active tab's cwd, runs the command, captures the resulting cwd back into the tab, and restores the saved cwd in thread mode.

This makes a built-in `cd` affect the active tab's logical shell session without leaving the entire application process in that directory. In process mode, the worker process also has its own process cwd, while the tab metadata is updated for the GUI.

---

## 8. Command Processing Pipeline

### 8.1 Dispatch

The GUI collects input in the active tab. When Enter is pressed, the line editor first checks whether the command is incomplete. Complete commands are then sent to the tab worker.

`shell_execute()`:

1. Saves the process cwd.
2. Changes to the tab's cwd.
3. Checks whether the command is handled by the built-in dispatcher.
4. Calls `builtin_execute()` for recognized built-ins; otherwise calls `parser_execute_pipeline()`.
5. Captures the updated cwd in the tab state.
6. Restores the saved process cwd in thread mode.

### 8.2 Built-ins versus external programs

A built-in must execute inside the shell context when it changes shell state. For example, `cd` must update the active tab's cwd; running it only in a short-lived child would not preserve that state.

External commands are launched using `fork()` followed by `execvp()`. The parent or job-control logic then waits for or tracks the child process.

### 8.3 Parsing stages

The parser separates the command into stages and arguments, with associated redirection targets.

At a high level:

```text
Input command line
       |
       v
Expand command substitutions / variables
       |
       v
Split into pipeline stages
       |
       v
Parse words and redirection operators
       |
       v
Create pipes and child processes
       |
       v
Connect descriptors with dup2()
       |
       v
execvp() each external command
       |
       v
Wait, track job, or return for background work
```

`parser.c` includes support for variable expansion, nested command-substitution boundary detection, command tokenization, pipeline splitting and filename glob expansion.

### 8.4 Command substitution

The parser recognizes both `$(...)` and backtick substitution. The substitution boundary finder tracks nesting so that an inner substitution can be evaluated before the surrounding expression is completed.

The captured output is collected through a pipe-backed child execution path and incorporated into the parent command's expanded text. This avoids requiring a separate interactive shell to evaluate the substitution.

### 8.5 Exit status

The shell tracks command completion status and makes the previous status available through `$?`. A command that cannot be executed reports an error and leaves the tab usable.

---

## 9. Pipelines and I/O Redirection

### 9.1 Pipeline construction

For a pipeline with `N` stages:

- The implementation creates `N - 1` inter-stage pipes.
- It launches one child process for each external stage.
- Each non-first stage receives standard input from the previous pipe.
- Each non-last stage sends standard output to the next pipe.
- Unused pipe ends are closed in child and parent contexts.

```text
Command 1          Command 2          Command 3
+--------+         +--------+         +--------+
| stdout |-------->| stdin  |         |        |
|        |         | stdout |-------->| stdin  |
+--------+         +--------+         +--------+
   pipe 0              pipe 1
```

The descriptor wiring for a middle stage is conceptually:

```c
dup2(previous_pipe_read, STDIN_FILENO);
dup2(next_pipe_write, STDOUT_FILENO);
```

The actual parser also handles first/last stages and redirection overrides. Closing unused descriptors is essential: if an unnecessary write end remains open, a downstream reader may never observe EOF.

### 9.2 Input redirection

For:

```bash
sort < input.txt
```

The child opens the input file read-only and duplicates the resulting descriptor onto standard input:

```c
fd = open(path, O_RDONLY);
dup2(fd, STDIN_FILENO);
close(fd);
```

If the input file cannot be opened, the command reports the error rather than proceeding with an invalid input stream.

### 9.3 Output redirection

For:

```bash
ls > output.txt
```

The output file is opened with create/truncate semantics and duplicated onto standard output:

```c
fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
dup2(fd, STDOUT_FILENO);
close(fd);
```

### 9.4 Combined redirection and pipelines

Redirection can be applied together with pipelines. Each child receives the correct standard input/output descriptors before `execvp()` replaces its process image.

The child-side descriptor setup must occur before execution; after `execvp()`, the program sees the redirected descriptors as its standard streams.

---

## 10. Process Groups, Signals and Job Control

### 10.1 Process groups

Pipeline stages belonging to the same job are placed into a process group. This allows the shell to send a signal to the entire pipeline using a negative process-group identifier:

```c
kill(-pgid, SIGINT);
```

The GUI receives keyboard input as X11 events, so Ctrl+C and Ctrl+Z are interpreted by the application and translated into signals for the active job.

### 10.2 Foreground execution

For a foreground command or pipeline:

1. Child processes are created and associated with the job's process group.
2. The tab records the foreground process-group identifier.
3. The shell waits for the relevant children, including stopped-state notifications.
4. When the foreground job exits or stops, the tab's foreground state is cleared or updated.
5. The prompt becomes available again when control returns to the shell.

### 10.3 Ctrl+C

`Ctrl+C` is handled in the X11 key-event path. If a foreground process group is active, Terminator sends `SIGINT` to that group. The GUI itself is not the target of the signal.

### 10.4 Ctrl+Z

`Ctrl+Z` sends `SIGTSTP` to the active foreground process group. The job-control code observes stopped child states, records the job and returns control to the prompt.

### 10.5 Background execution

A command ending in `&` is launched without waiting for it as a foreground job. The job table records the process group and command. Background jobs use `/dev/null` as standard input so that they do not consume interactive command input.

### 10.6 Job-control built-ins

| Built-in | Behaviour |
|---|---|
| `jobs` | Display active tracked jobs and their running/stopped state |
| `bg %n` | Send `SIGCONT` to a stopped job and continue it in the background |
| `fg %n` | Resume/bring a job into the foreground and wait for it |

The parser's job polling and foreground-waiting paths use `waitpid()` with options such as `WNOHANG` and `WUNTRACED` to observe child state changes without blocking the GUI indefinitely.

---

## 11. multiWatch Design

`multiWatch` runs multiple commands or pipelines concurrently and repeatedly. It is not implemented as a single sequential command chain.

### 11.1 State per watched command

Each `WatchCommand` stores:

- Command text and the tab cwd in which it should run.
- Process group and PIDs for pipeline stages.
- Output descriptor.
- Whether a run is active.
- Whether the output header has been printed.
- The next scheduled execution time.

### 11.2 Start and output flow

```text
multiWatch ["cmd A", "cmd B", "cmd C"]
          |
          v
Parse command list
          |
          +----------------+----------------+
          |                |                |
          v                v                v
      Launch A         Launch B         Launch C
      own pipe          own pipe          own pipe
          |                |                |
          +----------------+----------------+
                           |
                           v
                    poll(all read ends)
                           |
                           v
                 append arriving output
                           |
                           v
                 detect process completion
                           |
                           v
              schedule next run (default ~2 s)
```

Each watched command has its own pipe and child process group. `multiwatch_poll()` checks the read descriptors and process states, appending output as it arrives.

### 11.3 Output format

Each output block is emitted with the command text and a Unix timestamp, followed by separators and the captured output:

```text
"date +%T", <unix_timestamp>:
----------------------------------------------------
<command output>
----------------------------------------------------
```

### 11.4 Stop behaviour

When Ctrl+C is pressed during an active `multiWatch` session, the application stops the watched process groups and clears the monitoring state so that the tab can continue accepting commands.

---

## 12. History Design

### 12.1 Persistence

History is stored in:

```text
~/.terminator_history
```

The file is loaded when the application starts and updated as commands are added/saved. The in-memory/shared history is bounded by `HISTORY_CAP` (10,000 entries).

### 12.2 Shared access

History is intended to be visible across tabs:

- Thread mode uses a mutex around shared history operations.
- Process mode uses shared memory for the history structure and a named semaphore for mutual exclusion.

This allows one tab's command to become available to history navigation/search in another tab.

### 12.3 `history` command

The `history` built-in displays up to the most recent 1,000 commands. This display limit is separate from the 10,000-entry storage capacity.

### 12.4 Duplicate suppression

Consecutive duplicate command lines are skipped when adding history entries. This reduces repeated adjacent entries without removing non-consecutive repetitions.

### 12.5 Ctrl+R search

The reverse-search interface prompts for a search term. The search algorithm:

1. Scans from the newest history entry towards older entries for an exact match.
2. If there is no exact match, compares the term with history commands using longest common contiguous substring length.
3. Accepts a best substring match only when its length is greater than two characters.
4. Otherwise reports that no match was found.

This is a custom search behaviour; it is not the incremental reverse-search interface of GNU Readline/Bash.

---

## 13. Line Editing, Completion and Clipboard

### 13.1 Input editor

The input line and cursor position are stored in the active `Tab`. The key handler modifies the buffer and cursor directly, then requests a redraw.

| Key | Editor operation |
|---|---|
| Left / Right | Move cursor within the line |
| Ctrl+A / Ctrl+E | Move to beginning / end |
| Ctrl+U / Ctrl+K | Delete to beginning / end |
| Ctrl+W | Delete previous word |
| Up / Down | Navigate command history |
| Ctrl+L | Clear visible output |
| Ctrl+D | Close tab / EOF behaviour on empty input |

### 13.2 Multiline detection

Before submitting a line, `command_is_incomplete()` checks for incomplete constructs such as:

- An unescaped trailing backslash.
- An unmatched single or double quote.
- An unclosed command-substitution parenthesis.

If the command is incomplete, the editor appends the next line to the multiline buffer and displays the continuation prompt (`> `).

### 13.3 Filename completion

`filename_completion()` identifies the token immediately before the cursor. It scans the active tab's current directory with `opendir()` and `readdir()` and stores matching names as completion candidates.

The completion algorithm is:

```text
Typed token
    |
    v
Scan current directory
    |
    +---- no matches ------> leave line unchanged
    |
    +---- one match -------> replace token with that name
    |
    +---- several matches -> compute longest common prefix
                                  |
                                  +-- prefix grew -> insert prefix
                                  |
                                  +-- still ambiguous -> show numbered list
                                                             |
                                                             v
                                                    number selects candidate
```

The candidate list is bounded by `MAX_COMPLETIONS`. The line replacement preserves text after the token, so completion can be performed when the cursor is not at the end of the full input line.

### 13.4 Clipboard

The GUI implements X11 selection handling for copy and paste.

- Copying selected output stores the selected text and claims ownership of the relevant X11 selection atoms.
- When another X11 client requests the selection, Terminator responds to the `SelectionRequest`.
- Pasting requests clipboard data and handles the corresponding `SelectionNotify`.
- `Ctrl+Shift+C` and `Ctrl+Shift+V` are the application shortcuts.

This is an X11 selection implementation, not a terminal emulator's PTY-based clipboard mechanism.

---

## 14. Resource Limits and Error Handling

### 14.1 Fixed-size limits

The project uses fixed-size arrays for predictable bounds and simpler ownership. Important limits are listed in Section 4.7.

When extending the project, changes to these limits should be reviewed alongside all buffers and loops that depend on them.

### 14.2 Error handling principles

- System-call failures are checked where resources are created or commands are launched.
- Missing input files are reported before command execution.
- Unknown external commands produce a command-not-found diagnostic.
- Invalid or unavailable resources should not prevent the user from continuing to use an otherwise active tab.
- Pipe ends and child processes should be closed/reaped along their corresponding lifecycle paths.

### 14.3 Resource lifecycle

| Resource | Created by | Released by |
|---|---|---|
| X11 display/window and drawing resources | Application startup / GUI setup | Application cleanup |
| Per-tab pipes | `create_tab()` | Tab close / application cleanup |
| Tab worker thread | `pthread_create()` | Worker exits; detached thread resources are reclaimed |
| Tab worker process | `fork()` | Worker exits; parent reaps during lifecycle handling |
| Pipeline pipes | Parser execution | Parent and children close unused ends |
| Shared-memory mapping | Execution-mode setup | Execution-mode cleanup |
| POSIX semaphore | Process-mode setup | Execution-mode cleanup |
| Completion candidates | Filename completion | Completion reset / hotkey cleanup |
| Clipboard buffer | Copy operation | Clipboard/hotkey cleanup |

---

## 15. Design Trade-offs and Known Gaps

### 15.1 Why pipes instead of a PTY?

The project focuses on process creation, pipes, descriptor duplication and explicit signal routing. Pipes make these mechanisms visible and relatively straightforward to reason about.

The trade-off is that a pipe is not a terminal device. Programs that expect terminal ioctls, raw input mode, terminal-size queries, job-control terminal ownership or full-screen cursor control may not work as they would in a conventional terminal emulator.

### 15.2 Why two tab execution modes?

Thread mode demonstrates shared-address-space concurrency and mutex synchronization. Process mode demonstrates process isolation, explicit IPC, shared memory and process-shared synchronization. Implementing both modes provides a direct comparison of two operating-system concurrency models.

### 15.3 Why a shared history store?

History is a cross-tab feature. A command entered in one tab should be available to other tabs. The synchronization mechanism differs by execution model, but the user-visible purpose remains the same.

### 15.4 Why a bounded output buffer?

The GUI stores output in a fixed-capacity buffer so that long-running commands cannot grow a tab's in-memory output indefinitely. The trade-off is that older output is discarded when the buffer reaches capacity.
