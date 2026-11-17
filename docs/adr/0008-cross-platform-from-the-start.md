# 0008. Cross-platform from the start, Linux first

- Status: Accepted
- Date: 2026-11-17

## Context

Retrofitting portability into a native code base is costly: platform assumptions about paths, process spawning, pseudo-terminals, file watching and line endings spread widely. Linux is the primary development environment, with Windows and macOS to follow.

## Decision

Linux is the primary development and testing platform. All code is written to build and behave correctly on Linux, Windows and macOS from the first commit, using these rules:

- Platform-specific code lives only in the `platform` layer behind interfaces defined in portable headers: pseudo-terminal (forkpty and ConPTY), file watching (inotify, ReadDirectoryChangesW, FSEvents), process spawning, and OS path conventions.
- Outside that layer, use `std::filesystem` and Qt abstractions; no `#ifdef` on operating system names.
- Paths are handled as UTF-8 strings internally and converted at the OS boundary.
- Line endings, case-insensitive file systems and drive letters are handled in the file and workspace layers, not assumed.
- CMake presets exist for each platform, and CI builds and tests all three once a CI system is configured.

## Consequences

- Windows and macOS support requires implementing the `platform` interfaces rather than reworking the application.
- Linux-only conveniences (for example relying on `/proc` or POSIX signals) are confined to the `platform` layer.
- Each new feature touching the operating system carries the added cost of three implementations or a documented stub.
