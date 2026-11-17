# 0002. C++23 with CMake and vcpkg

- Status: Accepted
- Date: 2026-11-17

## Context

The editor targets low memory use and fast startup and input latency, which rules out a managed or browser-based runtime for the application itself. The code base must build on Linux, Windows and macOS (see [0008](0008-cross-platform-from-the-start.md)) and depend on several third-party libraries (Qt, tree-sitter, libvterm, a JSON library).

C++23 adds `std::expected`, `std::unreachable`, `std::to_underlying`, `std::byteswap` and `std::move_only_function`, among others. Library support for C++23 varies by compiler and standard library version, so the language mode and the set of usable library features are separate questions.

## Decision

- The implementation language standard is C++23 (`CMAKE_CXX_STANDARD 23`, extensions off).
- The minimum compilers are GCC 13 and Clang 18. Minimums for MSVC and Apple Clang are fixed when their CI jobs are added.
- Fallible operations return `std::expected<T, E>` instead of throwing; exceptions are reserved for programmer errors and allocation failure. Qt callbacks and the extension-host RPC layer translate at the boundary.
- A C++23 library facility is used only if it is available on every supported compiler and standard library. Facilities that fall outside the minimums, such as `<print>`, `std::generator` and `std::flat_map`, are not used until the minimums advance.
- The build system is CMake with Ninja as the generator and CMake presets for each supported platform.
- Third-party dependencies other than Qt are declared in a vcpkg manifest (`vcpkg.json`). The top-level CMake file enables the vcpkg toolchain when `VCPKG_ROOT` points to a vcpkg checkout; otherwise the same dependencies resolve from system packages. Qt 6 (minimum 6.4) is provided by the platform installer or package manager.
- Source is organized as one CMake library per architectural layer under `src/<layer>/`, with public headers in `include/anvil/<layer>/`. A layer links only against layers below it, which enforces the dependency rule in [design.md](../design.md).
- The CI matrix covers Linux, Windows and macOS.

## Consequences

- Dependencies are declared in a manifest and reproducible across machines.
- Error handling is explicit in function signatures and composes with `and_then` and `transform`.
- Raising the language standard does not raise the library floor: each C++23 facility is checked against the minimum compilers before use.
- Qt 6 and the vcpkg ports used compile under C++23.
- Clang with libstdc++ reports `__cpp_concepts` as 201907L, which makes libstdc++ 13 hide `<expected>`. The build overrides the macro to 202002L for Clang on Linux (`cmake/AnvilCompileOptions.cmake`). The override is removed once the minimum Clang version reports the correct value.
- Mixing vcpkg and a system-provided Qt requires the preset files to set both prefix paths explicitly.
- A different package manager such as Conan would offer comparable features; vcpkg is chosen for its first-party Qt and tree-sitter ports and its manifest workflow.
