#include "rope.hpp"

#include <algorithm>
#include <format>
#include <vector>

namespace anvil::core::rope {
namespace {

bool is_continuation(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0u) == 0x80u;
}

std::size_t char_length(char lead) {
    const auto byte = static_cast<unsigned char>(lead);
    if (byte < 0x80) {
        return 1;
    }
    if (byte >= 0xF0) {
        return 4;
    }
    return byte >= 0xE0 ? 3 : 2;
}

Summary operator+(const Summary& a, const Summary& b) {
    return {a.bytes + b.bytes, a.utf16 + b.utf16, a.newlines + b.newlines};
}

int height_of(const NodePtr& node) {
    return node ? node->height : -1;
}

NodePtr make_leaf(std::string text) {
    auto node = std::make_shared<Node>();
    node->summary = summarize(text);
    node->text = std::move(text);
    return node;
}

NodePtr make_inner(NodePtr left, NodePtr right) {
    auto node = std::make_shared<Node>();
    node->summary = left->summary + right->summary;
    node->height = std::max(left->height, right->height) + 1;
    node->left = std::move(left);
    node->right = std::move(right);
    return node;
}

// Joins two AVL trees whose heights differ by at most two into an AVL tree, with one single or
// double rotation when they differ by two.
NodePtr balanced(NodePtr left, NodePtr right) {
    const int left_height = left->height;
    const int right_height = right->height;
    if (left_height > right_height + 1) {
        if (height_of(left->left) >= height_of(left->right)) {
            return make_inner(left->left, make_inner(left->right, std::move(right)));
        }
        const NodePtr& middle = left->right;
        return make_inner(make_inner(left->left, middle->left),
                          make_inner(middle->right, std::move(right)));
    }
    if (right_height > left_height + 1) {
        if (height_of(right->right) >= height_of(right->left)) {
            return make_inner(make_inner(std::move(left), right->left), right->right);
        }
        const NodePtr& middle = right->left;
        return make_inner(make_inner(std::move(left), middle->left),
                          make_inner(middle->right, right->right));
    }
    return make_inner(std::move(left), std::move(right));
}

NodePtr build_balanced(const std::vector<NodePtr>& leaves, std::size_t begin, std::size_t end) {
    if (end - begin == 1) {
        return leaves[begin];
    }
    const std::size_t middle = begin + (end - begin) / 2;
    return make_inner(build_balanced(leaves, begin, middle), build_balanced(leaves, middle, end));
}

} // namespace

Summary summarize(std::string_view text) {
    Summary summary;
    summary.bytes = text.size();
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte & 0xC0u) != 0x80u) {
            summary.utf16 += byte >= 0xF0 ? 2 : 1;
        }
        if (c == '\n') {
            ++summary.newlines;
        }
    }
    return summary;
}

NodePtr build(std::string_view text) {
    if (text.empty()) {
        return nullptr;
    }
    // Leave room for the few bytes a chunk can grow when its end moves back to a character
    // boundary.
    constexpr std::size_t target = max_chunk_bytes - 4;
    const std::size_t chunk_count = (text.size() + target - 1) / target;

    std::vector<NodePtr> leaves;
    leaves.reserve(chunk_count);
    std::size_t begin = 0;
    for (std::size_t i = 1; i <= chunk_count; ++i) {
        std::size_t end = i == chunk_count ? text.size() : text.size() * i / chunk_count;
        while (end > begin && end < text.size() && is_continuation(text[end])) {
            --end;
        }
        leaves.push_back(make_leaf(std::string(text.substr(begin, end - begin))));
        begin = end;
    }
    return build_balanced(leaves, 0, leaves.size());
}

NodePtr concat(const NodePtr& left, const NodePtr& right) {
    if (!left) {
        return right;
    }
    if (!right) {
        return left;
    }
    if (left->height > right->height + 1) {
        return balanced(left->left, concat(left->right, right));
    }
    if (right->height > left->height + 1) {
        return balanced(concat(left, right->left), right->right);
    }
    return make_inner(left, right);
}

std::pair<NodePtr, NodePtr> split(const NodePtr& node, std::size_t offset) {
    if (!node || offset == 0) {
        return {nullptr, node};
    }
    if (offset >= node->summary.bytes) {
        return {node, nullptr};
    }
    if (node->is_leaf()) {
        return {make_leaf(node->text.substr(0, offset)), make_leaf(node->text.substr(offset))};
    }
    const std::size_t left_bytes = node->left->summary.bytes;
    if (offset < left_bytes) {
        auto [first, second] = split(node->left, offset);
        return {std::move(first), concat(second, node->right)};
    }
    if (offset == left_bytes) {
        return {node->left, node->right};
    }
    auto [first, second] = split(node->right, offset - left_bytes);
    return {concat(node->left, first), std::move(second)};
}

std::pair<NodePtr, std::string> pop_first_leaf(const NodePtr& node) {
    if (node->is_leaf()) {
        return {nullptr, node->text};
    }
    auto [rest, text] = pop_first_leaf(node->left);
    return {rest ? concat(rest, node->right) : node->right, std::move(text)};
}

std::pair<NodePtr, std::string> pop_last_leaf(const NodePtr& node) {
    if (node->is_leaf()) {
        return {nullptr, node->text};
    }
    auto [rest, text] = pop_last_leaf(node->right);
    return {rest ? concat(node->left, rest) : node->left, std::move(text)};
}

std::size_t line_at_offset(const NodePtr& node, std::size_t offset) {
    std::size_t lines = 0;
    const Node* current = node.get();
    while (current != nullptr && !current->is_leaf()) {
        if (offset < current->left->summary.bytes) {
            current = current->left.get();
        } else {
            lines += current->left->summary.newlines;
            offset -= current->left->summary.bytes;
            current = current->right.get();
        }
    }
    if (current != nullptr) {
        const std::size_t limit = std::min(offset, current->text.size());
        lines += static_cast<std::size_t>(
            std::count(current->text.begin(),
                       current->text.begin() + static_cast<std::ptrdiff_t>(limit), '\n'));
    }
    return lines;
}

std::size_t offset_of_line(const NodePtr& node, std::size_t line) {
    if (line == 0 || !node) {
        return 0;
    }
    std::size_t base = 0;
    std::size_t remaining = line;
    const Node* current = node.get();
    while (!current->is_leaf()) {
        if (remaining <= current->left->summary.newlines) {
            current = current->left.get();
        } else {
            remaining -= current->left->summary.newlines;
            base += current->left->summary.bytes;
            current = current->right.get();
        }
    }
    std::size_t position = 0;
    while (remaining > 0 && position < current->text.size()) {
        if (current->text[position++] == '\n') {
            --remaining;
        }
    }
    return base + position;
}

std::size_t utf16_at_offset(const NodePtr& node, std::size_t offset) {
    std::size_t units = 0;
    const Node* current = node.get();
    while (current != nullptr && !current->is_leaf()) {
        if (offset < current->left->summary.bytes) {
            current = current->left.get();
        } else {
            units += current->left->summary.utf16;
            offset -= current->left->summary.bytes;
            current = current->right.get();
        }
    }
    if (current != nullptr) {
        units += summarize(std::string_view(current->text).substr(0, offset)).utf16;
    }
    return units;
}

std::size_t offset_at_utf16(const NodePtr& node, std::size_t utf16) {
    if (!node) {
        return 0;
    }
    std::size_t base = 0;
    const Node* current = node.get();
    while (!current->is_leaf()) {
        if (utf16 < current->left->summary.utf16) {
            current = current->left.get();
        } else {
            utf16 -= current->left->summary.utf16;
            base += current->left->summary.bytes;
            current = current->right.get();
        }
    }
    std::size_t position = 0;
    std::size_t units = 0;
    while (position < current->text.size() && units < utf16) {
        const std::size_t length = char_length(current->text[position]);
        const std::size_t width = length == 4 ? 2 : 1;
        if (units + width > utf16) {
            break;
        }
        units += width;
        position += length;
    }
    return base + position;
}

bool is_char_boundary(const NodePtr& node, std::size_t offset) {
    if (offset == 0 || offset >= byte_size(node)) {
        return offset <= byte_size(node);
    }
    const Node* current = node.get();
    while (!current->is_leaf()) {
        if (offset < current->left->summary.bytes) {
            current = current->left.get();
        } else {
            offset -= current->left->summary.bytes;
            current = current->right.get();
        }
    }
    return !is_continuation(current->text[offset]);
}

void append_range(const NodePtr& node, std::size_t offset, std::size_t length, std::string& out) {
    if (!node || length == 0 || offset >= node->summary.bytes) {
        return;
    }
    length = std::min(length, node->summary.bytes - offset);
    if (node->is_leaf()) {
        out.append(node->text, offset, length);
        return;
    }
    const std::size_t left_bytes = node->left->summary.bytes;
    if (offset < left_bytes) {
        const std::size_t from_left = std::min(length, left_bytes - offset);
        append_range(node->left, offset, from_left, out);
        append_range(node->right, 0, length - from_left, out);
    } else {
        append_range(node->right, offset - left_bytes, length, out);
    }
}

std::string_view chunk_at(const NodePtr& node, std::size_t offset) {
    if (!node || offset >= node->summary.bytes) {
        return {};
    }
    const Node* current = node.get();
    while (!current->is_leaf()) {
        const std::size_t left_bytes = current->left->summary.bytes;
        if (offset < left_bytes) {
            current = current->left.get();
        } else {
            offset -= left_bytes;
            current = current->right.get();
        }
    }
    return std::string_view(current->text).substr(offset);
}

std::size_t chunk_count(const NodePtr& node) {
    if (!node) {
        return 0;
    }
    return node->is_leaf() ? 1 : chunk_count(node->left) + chunk_count(node->right);
}

namespace {

std::optional<std::string> check_node(const Node& node) {
    if (node.is_leaf()) {
        if (node.height != 0) {
            return "leaf with nonzero height";
        }
        if (node.text.empty()) {
            return "empty leaf";
        }
        if (node.text.size() > max_chunk_bytes) {
            return std::format("leaf of {} bytes exceeds the {} byte limit", node.text.size(),
                               max_chunk_bytes);
        }
        if (is_continuation(node.text.front())) {
            return "leaf starts inside a character";
        }
        if (!(node.summary == summarize(node.text))) {
            return "leaf summary does not match its text";
        }
        return std::nullopt;
    }
    if (!node.right || !node.text.empty()) {
        return "malformed inner node";
    }
    if (auto error = check_node(*node.left)) {
        return error;
    }
    if (auto error = check_node(*node.right)) {
        return error;
    }
    const int difference = node.left->height - node.right->height;
    if (difference < -1 || difference > 1) {
        return std::format("unbalanced node: child heights {} and {}", node.left->height,
                           node.right->height);
    }
    if (node.height != std::max(node.left->height, node.right->height) + 1) {
        return "inner node height is wrong";
    }
    if (!(node.summary == node.left->summary + node.right->summary)) {
        return "inner summary does not match its children";
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> check_invariants(const NodePtr& node) {
    return node ? check_node(*node) : std::nullopt;
}

} // namespace anvil::core::rope
