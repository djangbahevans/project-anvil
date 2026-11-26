# 0020. The files of a workspace are listed with ripgrep when they are asked for

- Status: Proposed
- Date: 2026-11-26

## Context

Go to File (#186) needs every file below the workspace folder. So do `workspace.findFiles` and workspace search (#22) and the activation event `workspaceContains:<glob>` (#209). The editor has no such list: the explorer reads and watches only the folders that are open, and `core::FileTree` is lazy.

VS Code keeps no index that a watcher updates. It runs ripgrep with `--files` when Quick Open asks (`src/vs/workbench/services/search/node/ripgrepFileSearch.ts`), keeps the result while the picker is open, and shows at most 512 matches (`anythingQuickAccess.ts`). What it leaves out follows `files.exclude`, `search.exclude` (`**/node_modules`, `**/bower_components` and `**/*.code-search` by default) and `.gitignore` and `.ignore` files (`search.useIgnoreFiles`, on by default).

Times with a warm page cache, on ext4 under WSL2 with 24 cores, on two generated trees of empty files: a flat one of 100,000 files in 1,000 directories, and an ignoring one of 150,001 files, of which `.gitignore` leaves out a `build` folder of 145,000. ripgrep is 15.0.0 from the VS Code checkout, run with `--files --hidden --no-require-git`.

| What | Files | Time |
| --- | --- | --- |
| `rg`, flat tree | 100,000 | 48 ms |
| `platform::list_directory` walk on one thread, flat tree | 100,000 | 210 ms |
| `rg`, ignoring tree | 5,001 | 6 ms |
| `rg --no-ignore`, ignoring tree | 150,001 | 69 ms |
| `platform::list_directory` walk, ignoring tree, nothing left out | 150,001 | 320 ms |
| `core::score_item` over 100,000 paths and the best 512 ordered, one thread | 100,000 | 60 to 96 ms for each query |

Ignore files decide the size of the problem more than the speed of the listing does. ripgrep is already the planned engine of workspace search ([design.md](../design.md), #22).

## Decision

- **No index and no recursive watch.** The files are listed when something asks for them. Go to File lists them when it opens, shows matches while the listing runs, and keeps the list until it closes.
- **ripgrep lists them**, with `--files`, as a child process whose output is read through a pipe (the piped standard streams of #9).
- **The rules are those of VS Code**, from the same settings and defaults: `files.exclude`, `search.exclude`, `search.useIgnoreFiles`, `search.useGlobalIgnoreFiles` (off), `search.useParentIgnoreFiles` (off) and `search.followSymlinks` (on). Hidden files are listed.
- **One library, `search`**, with no Qt, depends on `core` and `platform` only, so that both `exthost` and the layers above it use it.
- **The binary is found by one function:** `ANVIL_RG`, else the one in the VS Code checkout that the extension host uses, else `rg` on `PATH`. Without one, Go to File lists the open documents and says why. How a distributed build gets ripgrep is not decided here.
- **Matching runs off the UI thread.**

## Consequences

- What Go to File, search and extensions see follows one set of rules, which is that of VS Code and of ripgrep.
- Nothing is held and nothing is watched while no search runs. A file made while the picker is open appears the next time.
- The time to the first matches is the time of the disk. A cold cache and slow file systems, such as a Windows drive seen from WSL, are not measured and are slower.
- The editor depends on a program that it does not build. Piped streams are needed on every platform, and on Windows they wait for its process support (#26).
- A walk of the editor's own would need no child process, but would have to implement the semantics of ignore files (negation, nesting, precedence, global files), or list 150,001 files of the ignoring tree where ripgrep lists 5,001. Workspace search would still use ripgrep, with rules that could differ.
- An index kept by a recursive watch would answer at once when the picker opens, but costs a watch for each directory, memory for the whole session and a walk at startup, all for a list that takes less than a tenth of a second to make. It needs the same ignore rules.
