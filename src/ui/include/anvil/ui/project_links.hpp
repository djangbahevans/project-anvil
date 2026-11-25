#pragma once

#include <string_view>

namespace anvil::ui {

// The pages of the project that the Help menu opens in the system's browser.
inline constexpr std::string_view documentation_url =
    "https://github.com/djangbahevans/project-anvil#readme";
inline constexpr std::string_view new_issue_url =
    "https://github.com/djangbahevans/project-anvil/issues/new";
inline constexpr std::string_view license_url =
    "https://github.com/djangbahevans/project-anvil/blob/main/LICENSE";

} // namespace anvil::ui
