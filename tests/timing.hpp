#pragma once

namespace anvil::test {

// Sanitizers slow code down several times over, so a time limit in a test applies to plain builds
// only. The behavior that a limit stands for is checked by other assertions in every build.
#if defined(__SANITIZE_ADDRESS__)
inline constexpr bool timing_is_meaningful = false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
inline constexpr bool timing_is_meaningful = false;
#else
inline constexpr bool timing_is_meaningful = true;
#endif
#else
inline constexpr bool timing_is_meaningful = true;
#endif

} // namespace anvil::test
