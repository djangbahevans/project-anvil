#include "anvil/core/jsonc_edit.hpp"

#include "anvil/core/jsonc.hpp"

#include <algorithm>
#include <vector>

namespace anvil::core {
namespace {

using nlohmann::json;

constexpr std::size_t npos = JsoncMember::npos;

struct Edit {
    std::size_t position;
    std::size_t length;
    std::string text;
};

// Where a key lives: the object that holds the property, and its place among the properties.
struct Location {
    const JsoncNode* object;
    std::size_t index;
};

std::optional<std::size_t> last_named(const JsoncNode& object, std::string_view name) {
    for (std::size_t i = object.members.size(); i > 0; --i) {
        if (object.members[i - 1].name == name) {
            return i - 1;
        }
    }
    return std::nullopt;
}

// The order of `Settings::find`: the key spelled flat, then the shorter prefixes as the name of an
// object that holds the rest.
std::optional<Location> locate(const JsoncNode& object, std::string_view key) {
    if (object.kind != JsoncNode::Kind::object) {
        return std::nullopt;
    }
    if (const auto index = last_named(object, key)) {
        return Location{&object, *index};
    }
    for (std::size_t dot = key.find('.'); dot != std::string_view::npos;
         dot = key.find('.', dot + 1)) {
        if (const auto child = last_named(object, key.substr(0, dot))) {
            if (const auto found = locate(object.members[*child].value, key.substr(dot + 1))) {
                return found;
            }
        }
    }
    return std::nullopt;
}

std::string dump(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool is_eol(char c) {
    return c == '\n' || c == '\r';
}

bool is_blank_text(std::string_view text) {
    return std::ranges::all_of(text, [](char c) { return c == ' ' || c == '\t'; });
}

std::string_view detect_eol(std::string_view text) {
    const std::size_t newline = text.find('\n');
    if (newline != std::string_view::npos && newline > 0 && text[newline - 1] == '\r') {
        return "\r\n";
    }
    return "\n";
}

std::size_t line_start(std::string_view text, std::size_t position) {
    std::size_t i = position;
    while (i > 0 && !is_eol(text[i - 1])) {
        --i;
    }
    return i;
}

std::string_view indentation_of_line(std::string_view text, std::size_t position) {
    const std::size_t begin = line_start(text, position);
    std::size_t end = begin;
    while (end < text.size() && (text[end] == ' ' || text[end] == '\t')) {
        ++end;
    }
    return text.substr(begin, end - begin);
}

// How far the file indents: the first indented line decides, four spaces when none is.
std::string indent_unit(std::string_view text) {
    std::size_t line = 0;
    while (line < text.size()) {
        std::size_t end = line;
        while (end < text.size() && (text[end] == ' ' || text[end] == '\t')) {
            ++end;
        }
        if (end > line && end < text.size() && !is_eol(text[end])) {
            if (text[line] == '\t') {
                return "\t";
            }
            return std::string(std::min<std::size_t>(end - line, 8), ' ');
        }
        while (end < text.size() && !is_eol(text[end])) {
            ++end;
        }
        while (end < text.size() && is_eol(text[end])) {
            ++end;
        }
        line = end;
    }
    return "    ";
}

// The end of the line that `position` is on, when nothing but spaces and comments lies between
// them and `limit`; `npos` when something else follows on the line.
std::size_t end_of_line_after(std::string_view text, std::size_t position, std::size_t limit) {
    std::size_t i = position;
    while (i < limit) {
        const char c = text[i];
        if (c == ' ' || c == '\t') {
            ++i;
        } else if (is_eol(c)) {
            return i;
        } else if (text.substr(i).starts_with("//")) {
            while (i < limit && !is_eol(text[i])) {
                ++i;
            }
        } else if (text.substr(i).starts_with("/*")) {
            const std::size_t close = text.find("*/", i + 2);
            if (close == std::string_view::npos) {
                return npos;
            }
            i = close + 2;
        } else {
            return npos;
        }
    }
    return npos;
}

std::size_t eol_length(std::string_view text, std::size_t position) {
    if (text[position] == '\r' && position + 1 < text.size() && text[position + 1] == '\n') {
        return 2;
    }
    return 1;
}

// Applies edits that do not overlap. Working from the end of the text keeps the positions of the
// others valid, and two insertions at one position end up in the order they were given.
std::string apply(std::string_view text, std::vector<Edit> edits) {
    std::ranges::stable_sort(edits,
                             [](const Edit& a, const Edit& b) { return a.position < b.position; });
    std::string result(text);
    for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit) {
        result.replace(edit->position, edit->length, edit->text);
    }
    return result;
}

std::string member_text(std::string_view key, const json& value) {
    return dump(json(std::string(key))) + ": " + dump(value);
}

std::string add_to_blank(std::string_view text, const std::string& member) {
    const std::string_view eol = detect_eol(text);
    std::string result(text);
    if (!result.empty() && !is_eol(result.back())) {
        result += eol;
    }
    result += "{";
    result += eol;
    result += indent_unit(text);
    result += member;
    result += eol;
    result += "}";
    result += eol;
    return result;
}

std::string add_member(std::string_view text, const JsoncNode& root, const std::string& member) {
    const std::string_view eol = detect_eol(text);
    if (root.members.empty()) {
        const std::size_t close = root.close;
        const bool multi_line =
            text.substr(root.begin, close - root.begin).find('\n') != std::string_view::npos;
        const std::size_t closing_line = line_start(text, close);
        if (multi_line && is_blank_text(text.substr(closing_line, close - closing_line))) {
            const std::string indent =
                std::string(text.substr(closing_line, close - closing_line)) + indent_unit(text);
            return apply(text, {{closing_line, 0, indent + member + std::string(eol)}});
        }
        const std::string indent = indent_unit(text);
        return apply(text, {{close, 0, std::string(eol) + indent + member + std::string(eol)}});
    }

    const JsoncMember& last = root.members.back();
    const bool trailing_comma = last.comma != npos;
    const std::size_t after = trailing_comma ? last.comma + 1 : last.value.end;
    const std::size_t line_end = end_of_line_after(text, after, root.close);
    if (line_end == npos) {
        // The last property and the closing brace share a line: the new property goes in it.
        if (trailing_comma) {
            return apply(text, {{after, 0, " " + member + ","}});
        }
        return apply(text, {{after, 0, ", " + member}});
    }
    const std::string indent(indentation_of_line(text, last.name_begin));
    std::vector<Edit> edits;
    if (!trailing_comma) {
        edits.push_back({last.value.end, 0, ","});
    }
    edits.push_back(
        {line_end, 0, std::string(eol) + indent + member + (trailing_comma ? "," : "")});
    return apply(text, std::move(edits));
}

std::string remove_member(std::string_view text, const JsoncNode& object, std::size_t index) {
    const JsoncMember& member = object.members[index];
    const JsoncMember* previous = index > 0 ? &object.members[index - 1] : nullptr;
    const bool last = index + 1 == object.members.size();
    const std::size_t begin = member.name_begin;
    const std::size_t end = member.comma != npos ? member.comma + 1 : member.value.end;

    const std::size_t line_begin = line_start(text, begin);
    const std::size_t line_end = end_of_line_after(text, end, object.close);
    if (is_blank_text(text.substr(line_begin, begin - line_begin)) && line_end != npos) {
        // The property has its line to itself: the line goes, and with it a comment at its end.
        std::vector<Edit> edits;
        edits.push_back({line_begin, line_end + eol_length(text, line_end) - line_begin, ""});
        if (last && member.comma == npos && previous && previous->comma != npos) {
            edits.push_back({previous->comma, 1, ""});
        }
        return apply(text, std::move(edits));
    }
    if (member.comma != npos) {
        std::size_t stop = end;
        while (stop < text.size() && (text[stop] == ' ' || text[stop] == '\t')) {
            ++stop;
        }
        return apply(text, {{begin, stop - begin, ""}});
    }
    std::vector<Edit> edits;
    std::size_t from = begin;
    if (previous && previous->comma != npos) {
        // The last property goes with the comma before it, and a comment between the two stays.
        edits.push_back({previous->comma, 1, ""});
        while (from > 0 && (text[from - 1] == ' ' || text[from - 1] == '\t')) {
            --from;
        }
    }
    edits.push_back({from, end - from, ""});
    return apply(text, std::move(edits));
}

} // namespace

std::expected<std::string, Error> edit_jsonc_property(std::string_view text, std::string_view key,
                                                      const std::optional<json>& value) {
    if (is_blank_jsonc(text)) {
        if (!value) {
            return std::string(text);
        }
        return add_to_blank(text, member_text(key, *value));
    }
    const auto root = parse_jsonc_tree(text);
    if (!root) {
        return std::unexpected(root.error());
    }
    if (root->kind != JsoncNode::Kind::object) {
        return std::unexpected(Error{ErrorCode::invalid_argument,
                                     "the settings must be a JSON object at the top level"});
    }

    const auto found = locate(*root, key);
    if (!found) {
        if (!value) {
            return std::string(text);
        }
        return add_member(text, *root, member_text(key, *value));
    }
    if (!value) {
        // A key spelled both ways is read from the flat one, so removing only that would show the
        // other: take out every property that holds the key.
        return edit_jsonc_property(remove_member(text, *found->object, found->index), key,
                                   std::nullopt);
    }
    const JsoncNode& old_value = found->object->members[found->index].value;
    return apply(text, {{old_value.begin, old_value.end - old_value.begin, dump(*value)}});
}

} // namespace anvil::core
