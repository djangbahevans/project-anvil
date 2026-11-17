#include "anvil/core/uri.hpp"

#include <cctype>

namespace anvil::core {
namespace {

bool has_drive_prefix(const std::string& text, std::size_t at) {
    return text.size() >= at + 2 && std::isalpha(static_cast<unsigned char>(text[at])) != 0 &&
           text[at + 1] == ':';
}

} // namespace

Uri Uri::file(const std::filesystem::path& path) {
    std::string generic = path.generic_string();
    if (has_drive_prefix(generic, 0)) {
        generic[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(generic[0])));
        generic.insert(generic.begin(), '/');
    }
    return Uri{"file", "", std::move(generic), "", ""};
}

std::optional<std::filesystem::path> Uri::file_path() const {
    if (scheme != "file") {
        return std::nullopt;
    }
    if (path.size() >= 3 && path[0] == '/' && has_drive_prefix(path, 1)) {
        return std::filesystem::path(path.substr(1));
    }
    return std::filesystem::path(path);
}

std::string Uri::to_string() const {
    std::string result = scheme + ":";
    if (!authority.empty() || scheme == "file") {
        result += "//" + authority;
    }
    result += path;
    if (!query.empty()) {
        result += "?" + query;
    }
    if (!fragment.empty()) {
        result += "#" + fragment;
    }
    return result;
}

} // namespace anvil::core
