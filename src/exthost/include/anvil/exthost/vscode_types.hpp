#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/selection.hpp"
#include "anvil/core/text.hpp"
#include "anvil/core/uri.hpp"

#include <expected>
#include <nlohmann/json.hpp>

// Conversions between the editor's types and the JSON shapes of the extension host protocol.
// VS Code numbers lines and columns from one where the editor counts from zero; columns are
// UTF-16 code units in both.
namespace anvil::exthost {

// UriComponents.
nlohmann::json uri_to_json(const core::Uri& uri);
std::expected<core::Uri, Error> uri_from_json(const nlohmann::json& json);

// IRange.
nlohmann::json range_to_json(const core::Range& range);
std::expected<core::Range, Error> range_from_json(const nlohmann::json& json);

// ISelection: the anchor is the selection start and the position is the active end. The offsets
// are positions in `text`.
nlohmann::json selection_to_json(const core::Selection& selection, const core::Text& text);
std::expected<core::Selection, Error> selection_from_json(const nlohmann::json& json,
                                                          const core::Text& text);

} // namespace anvil::exthost
