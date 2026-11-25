#include "anvil/core/language_comments.hpp"

#include "anvil/core/jsonc.hpp"
#include "anvil/core/language_comments_table.hpp"

#include <map>

namespace anvil::core {
namespace {

using json = nlohmann::json;

void read_line(const json& value, ReadComments& out) {
    if (value.is_string()) {
        if (!value.get<std::string>().empty()) {
            out.comments.line = value.get<std::string>();
        }
        return;
    }
    if (value.is_object()) {
        const auto comment = value.find("comment");
        if (comment == value.end() || !comment->is_string()) {
            out.problems.emplace_back("comments.lineComment.comment is not a string");
            return;
        }
        if (comment->get<std::string>().empty()) {
            return;
        }
        out.comments.line = comment->get<std::string>();
        const auto no_indent = value.find("noIndent");
        out.comments.line_no_indent =
            no_indent != value.end() && no_indent->is_boolean() && no_indent->get<bool>();
        return;
    }
    out.problems.emplace_back("comments.lineComment is not a string or an object");
}

void read_block(const json& value, ReadComments& out) {
    const bool pair = value.is_array() && value.size() == 2 && value[0].is_string() &&
                      value[1].is_string() && !value[0].get<std::string>().empty() &&
                      !value[1].get<std::string>().empty();
    if (!pair) {
        out.problems.emplace_back("comments.blockComment is not an array of two non-empty strings");
        return;
    }
    out.comments.block = BlockComment{value[0].get<std::string>(), value[1].get<std::string>()};
}

} // namespace

LanguageComments LanguageComments::overlaid_with(const LanguageComments& overlay) const {
    LanguageComments result = *this;
    if (overlay.line) {
        result.line = overlay.line;
        result.line_no_indent = overlay.line_no_indent;
    }
    if (overlay.block) {
        result.block = overlay.block;
    }
    return result;
}

ReadComments read_language_comments(std::string_view text) {
    ReadComments out;
    const auto parsed = parse_jsonc(text);
    if (!parsed) {
        out.problems.push_back(parsed.error().message);
        return out;
    }
    if (!parsed->is_object()) {
        out.problems.emplace_back("the language configuration is not an object");
        return out;
    }
    const auto comments = parsed->find("comments");
    if (comments == parsed->end()) {
        return out;
    }
    if (!comments->is_object()) {
        out.problems.emplace_back("comments is not an object");
        return out;
    }
    if (const auto line = comments->find("lineComment"); line != comments->end()) {
        read_line(*line, out);
    }
    if (const auto block = comments->find("blockComment"); block != comments->end()) {
        read_block(*block, out);
    }
    return out;
}

const LanguageComments* builtin_language_comments(std::string_view language_id) {
    static const std::map<std::string, LanguageComments, std::less<>> table = [] {
        std::map<std::string, LanguageComments, std::less<>> built;
        for (const LanguageCommentsRow& row : language_comments_table) {
            LanguageComments comments;
            if (!row.line.empty()) {
                comments.line = std::string(row.line);
                comments.line_no_indent = row.line_no_indent;
            }
            if (!row.block_open.empty()) {
                comments.block =
                    BlockComment{std::string(row.block_open), std::string(row.block_close)};
            }
            built.emplace(std::string(row.id), std::move(comments));
        }
        return built;
    }();
    const auto found = table.find(language_id);
    return found == table.end() ? nullptr : &found->second;
}

} // namespace anvil::core
