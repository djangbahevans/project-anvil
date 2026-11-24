#pragma once

#include <string_view>

namespace anvil::core {

// Whether `path`, written with forward slashes, matches the glob pattern, in the dialect of
// VS Code's file watchers and search patterns:
//   `*`        any characters except `/`
//   `?`        one character except `/`
//   `**`       as a whole segment, any number of segments, including none
//   `[abc]`    one of the characters; `[a-z]` a range; `[!abc]` or `[^abc]` anything else
//   `{a,b}`    either alternative, which may contain `/`; alternatives nest
// Matching is case sensitive and covers the whole path. Empty segments, such as the one before a
// leading `/`, are ignored on both sides. A `{` without its `}` and a `[` without its `]` are
// ordinary characters.
bool glob_match(std::string_view pattern, std::string_view path);

} // namespace anvil::core
