#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// Persistent rope: an AVL-balanced binary tree whose leaves hold chunks of UTF-8 text. Nodes are
// immutable once built, so trees share structure and a copy of the root pointer is a snapshot.
// Every node caches the byte, UTF-16 and newline counts of its subtree, which makes offset, line
// and UTF-16 column conversions logarithmic in the number of chunks.
//
// All offsets are byte offsets on UTF-8 character boundaries. An empty tree is a null pointer.
namespace anvil::core::rope {

struct Summary {
    std::size_t bytes = 0;
    std::size_t utf16 = 0;
    std::size_t newlines = 0;

    bool operator==(const Summary&) const = default;
};

Summary summarize(std::string_view text);

struct Node;
using NodePtr = std::shared_ptr<const Node>;

struct Node {
    Summary summary;
    // 0 for a leaf.
    int height = 0;
    // Leaves only.
    std::string text;
    // Inner nodes only.
    NodePtr left;
    NodePtr right;

    bool is_leaf() const {
        return left == nullptr;
    }
};

inline constexpr std::size_t max_chunk_bytes = 2048;

inline std::size_t byte_size(const NodePtr& node) {
    return node ? node->summary.bytes : 0;
}

// Builds a balanced tree from valid UTF-8 text, splitting it into chunks of similar size.
NodePtr build(std::string_view text);

NodePtr concat(const NodePtr& left, const NodePtr& right);

// Splits at `offset` (at most the size of the tree) into the text before and after it.
std::pair<NodePtr, NodePtr> split(const NodePtr& node, std::size_t offset);

// Removes the first or last chunk, returning the remaining tree and that chunk's text. The tree
// must not be empty.
std::pair<NodePtr, std::string> pop_first_leaf(const NodePtr& node);
std::pair<NodePtr, std::string> pop_last_leaf(const NodePtr& node);

// Number of line breaks before `offset`, which is the zero-based line the offset is on.
std::size_t line_at_offset(const NodePtr& node, std::size_t offset);

// Offset of the first byte of `line`; `line` must not exceed the number of line breaks.
std::size_t offset_of_line(const NodePtr& node, std::size_t line);

std::size_t utf16_at_offset(const NodePtr& node, std::size_t offset);

// Offset of the character containing UTF-16 unit `utf16`, so a position inside a surrogate pair
// maps to the start of the pair. Positions past the end map to the size of the tree.
std::size_t offset_at_utf16(const NodePtr& node, std::size_t utf16);

bool is_char_boundary(const NodePtr& node, std::size_t offset);

// Appends the bytes of [offset, offset + length) to `out`, clamped to the end of the tree.
void append_range(const NodePtr& node, std::size_t offset, std::size_t length, std::string& out);

// The bytes from `offset` to the end of the leaf chunk that holds it, which stay valid as long as
// the tree does. Empty when `offset` is at or past the end of the tree.
std::string_view chunk_at(const NodePtr& node, std::size_t offset);

// Number of leaf chunks.
std::size_t chunk_count(const NodePtr& node);

// A description of the first violated structural invariant, or std::nullopt when the tree is
// well-formed: summaries match the text, the tree is balanced, and chunks are non-empty and no
// larger than max_chunk_bytes.
std::optional<std::string> check_invariants(const NodePtr& node);

} // namespace anvil::core::rope
