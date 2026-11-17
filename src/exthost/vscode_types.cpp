#include "anvil/exthost/vscode_types.hpp"

namespace anvil::exthost {
namespace {

using nlohmann::json;

Error malformed(const std::string& what) {
    return Error{ErrorCode::protocol_error, "malformed " + what};
}

// A one-based line or column from the extension host as a zero-based count.
std::expected<std::size_t, Error> to_zero_based(const json& value, const std::string& what) {
    if (!value.is_number_integer() || value.get<long long>() < 1) {
        return std::unexpected(malformed(what));
    }
    return static_cast<std::size_t>(value.get<long long>() - 1);
}

std::expected<core::Position, Error> position_from(const json& object, const char* line_key,
                                                   const char* column_key,
                                                   const std::string& what) {
    if (!object.is_object() || !object.contains(line_key) || !object.contains(column_key)) {
        return std::unexpected(malformed(what));
    }
    auto line = to_zero_based(object[line_key], what);
    auto column = to_zero_based(object[column_key], what);
    if (!line) {
        return std::unexpected(line.error());
    }
    if (!column) {
        return std::unexpected(column.error());
    }
    return core::Position{*line, *column};
}

void put_position(const core::Position& position, const char* line_key, const char* column_key,
                  json& target) {
    target[line_key] = position.line + 1;
    target[column_key] = position.column + 1;
}

} // namespace

json uri_to_json(const core::Uri& uri) {
    json result = {{"$mid", 1}, {"scheme", uri.scheme}, {"path", uri.path}};
    if (!uri.authority.empty()) {
        result["authority"] = uri.authority;
    }
    if (!uri.query.empty()) {
        result["query"] = uri.query;
    }
    if (!uri.fragment.empty()) {
        result["fragment"] = uri.fragment;
    }
    return result;
}

std::expected<core::Uri, Error> uri_from_json(const json& json_uri) {
    if (!json_uri.is_object() || !json_uri.contains("scheme") || !json_uri["scheme"].is_string()) {
        return std::unexpected(malformed("resource identifier"));
    }
    core::Uri uri;
    uri.scheme = json_uri["scheme"].get<std::string>();
    uri.authority = json_uri.value("authority", "");
    uri.path = json_uri.value("path", "");
    uri.query = json_uri.value("query", "");
    uri.fragment = json_uri.value("fragment", "");
    return uri;
}

json range_to_json(const core::Range& range) {
    json result = json::object();
    put_position(range.start, "startLineNumber", "startColumn", result);
    put_position(range.end, "endLineNumber", "endColumn", result);
    return result;
}

std::expected<core::Range, Error> range_from_json(const json& json_range) {
    auto start = position_from(json_range, "startLineNumber", "startColumn", "range");
    auto end = position_from(json_range, "endLineNumber", "endColumn", "range");
    if (!start) {
        return std::unexpected(start.error());
    }
    if (!end) {
        return std::unexpected(end.error());
    }
    return core::Range{*start, *end};
}

json selection_to_json(const core::Selection& selection, const core::Text& text) {
    json result = json::object();
    put_position(text.position_at(selection.anchor), "selectionStartLineNumber",
                 "selectionStartColumn", result);
    put_position(text.position_at(selection.active), "positionLineNumber", "positionColumn",
                 result);
    return result;
}

std::expected<core::Selection, Error> selection_from_json(const json& json_selection,
                                                          const core::Text& text) {
    auto anchor = position_from(json_selection, "selectionStartLineNumber", "selectionStartColumn",
                                "selection");
    auto active =
        position_from(json_selection, "positionLineNumber", "positionColumn", "selection");
    if (!anchor) {
        return std::unexpected(anchor.error());
    }
    if (!active) {
        return std::unexpected(active.error());
    }
    return core::Selection{text.offset_at(*anchor), text.offset_at(*active)};
}

} // namespace anvil::exthost
