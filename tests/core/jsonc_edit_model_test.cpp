#include "anvil/core/jsonc.hpp"
#include "anvil/core/jsonc_edit.hpp"
#include "anvil/core/settings.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <map>
#include <random>
#include <string>
#include <vector>

// Edits of random settings files, checked against what the generator knows about the text it wrote.
// The generator records where every value and every property is, so the checks do not depend on the
// parser that the edit uses.

namespace {

using anvil::core::edit_jsonc_property;
using anvil::core::parse_jsonc;
using anvil::core::Settings;
using nlohmann::json;

struct Leaf {
    std::string key;
    json value;
    std::size_t value_begin = 0;
    std::size_t value_end = 0;
    // What removing the property takes out of the text, as the generator wrote it.
    std::size_t removed_begin = 0;
    std::size_t removed_end = 0;
    // Whether it has a comma of its own, and whether a property comes before it in its object.
    bool has_comma = false;
    bool has_previous = false;
};

struct Document {
    std::string text;
    std::vector<Leaf> leaves;
};

class Generator {
public:
    explicit Generator(unsigned seed) : rng_(seed) {}

    Document make() {
        Document doc;
        eol_ = chance(25) ? "\r\n" : "\n";
        unit_ = chance(20) ? "\t" : chance(50) ? "  " : "    ";
        std::string& out = doc.text;
        if (chance(10)) {
            out += "\xEF\xBB\xBF";
        }
        for (int i = pick(0, 2); i > 0; --i) {
            out += comment() + eol_;
        }
        object(doc, out, "", 0, chance(85));
        if (chance(70)) {
            out += eol_;
        }
        if (chance(15)) {
            out += comment() + eol_;
        }
        return doc;
    }

    std::mt19937& rng() {
        return rng_;
    }
    bool chance(int percent) {
        return pick(1, 100) <= percent;
    }
    int pick(int low, int high) {
        return std::uniform_int_distribution<int>(low, high)(rng_);
    }

    json scalar() {
        switch (pick(0, 6)) {
        case 0:
            return true;
        case 1:
            return false;
        case 2:
            return pick(-1000, 100000);
        case 3:
            return nullptr;
        case 4:
            return 1.5 * pick(1, 9);
        case 5: {
            static const std::vector<std::string> words = {
                "Dark 2026", "x", "", "quote \" and \\ slash", "tab\tnew\nline", "é日本", "a,b}"};
            return words[static_cast<std::size_t>(pick(0, static_cast<int>(words.size()) - 1))];
        }
        default:
            return "monospace";
        }
    }

private:
    std::string comment() {
        static const std::vector<std::string> comments = {"// a note", "/* block */",
                                                          "// key: \"x\", y", "/* a, b } */"};
        return comments[static_cast<std::size_t>(pick(0, static_cast<int>(comments.size()) - 1))];
    }

    std::string name() {
        static const std::vector<std::string> names = {
            "editor",  "files",    "window", "workbench", "fontSize",
            "tabSize", "autoSave", "theme",  "x",         "y"};
        return names[static_cast<std::size_t>(pick(0, static_cast<int>(names.size()) - 1))];
    }

    // Writes an object at the end of `out`. `prefix` is the dotted key of the object, empty for the
    // root. A property is a leaf, or a nested object; a leaf may be spelled with a dotted name.
    void object(Document& doc, std::string& out, const std::string& prefix, int depth,
                bool multi_line) {
        out += "{";
        const int count = depth == 0 ? pick(0, 6) : pick(1, 3);
        std::vector<std::string> used;
        bool has_previous = false;
        for (int i = 0; i < count; ++i) {
            std::string member_name = name();
            if (chance(30)) {
                member_name += "." + name();
            }
            if (std::ranges::find(used, member_name) != used.end()) {
                continue;
            }
            // Two properties whose names overlap as keys would make the key ambiguous.
            bool overlapping = false;
            for (const std::string& other : used) {
                overlapping = overlapping || other.starts_with(member_name + ".") ||
                              member_name.starts_with(other + ".");
            }
            if (overlapping) {
                continue;
            }
            used.push_back(member_name);
            const bool last = i + 1 == count;
            const std::string key = prefix.empty() ? member_name : prefix + "." + member_name;
            const bool nested = depth < 2 && chance(25);
            const bool trailing_comma = last && chance(25);
            const bool comma = !last || trailing_comma;

            if (multi_line) {
                out += eol_;
                if (chance(30)) {
                    out += indent(depth + 1) + comment() + eol_;
                }
                out += indent(depth + 1);
            } else {
                out += has_previous ? (chance(20) ? " /* c */ " : " ") : (chance(50) ? " " : "");
            }
            const std::size_t member_begin = out.size();
            out += "\"" + member_name + "\":" + (chance(80) ? " " : "");
            if (nested) {
                // The leaves of a nested object are recorded by the call; the object itself has
                // no leaf, so this property is never the one that is edited.
                object(doc, out, key, depth + 1, multi_line && chance(80));
                has_previous = true;
                if (comma) {
                    out += ",";
                }
                continue;
            }
            Leaf leaf;
            leaf.key = key;
            leaf.value = scalar();
            leaf.value_begin = out.size();
            out += leaf.value.dump();
            leaf.value_end = out.size();
            leaf.removed_begin = member_begin;
            leaf.has_comma = comma;
            leaf.has_previous = has_previous;
            if (comma) {
                out += ",";
            }
            leaf.removed_end = out.size();
            if (multi_line && chance(35)) {
                out += chance(50) ? " // trailing" : " /* trailing */";
                leaf.removed_end = out.size();
            }
            doc.leaves.push_back(leaf);
            has_previous = true;
        }
        if (multi_line) {
            out += eol_ + indent(depth);
        } else if (has_previous && chance(50)) {
            out += " ";
        }
        out += "}";
    }

    std::string indent(int depth) const {
        std::string result;
        for (int i = 0; i < depth; ++i) {
            result += unit_;
        }
        return result;
    }

    std::mt19937 rng_;
    std::string eol_ = "\n";
    std::string unit_ = "    ";
};

// The characters of the text that are not whitespace, counted.
std::map<char, int> glyphs(std::string_view text) {
    std::map<char, int> count;
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            ++count[c];
        }
    }
    return count;
}

// `whole` minus `part`; nothing counts below zero, and `ok` says whether `part` fitted in `whole`.
std::map<char, int> difference(std::map<char, int> whole, const std::map<char, int>& part,
                               bool& ok) {
    ok = true;
    for (const auto& [c, n] : part) {
        whole[c] -= n;
        if (whole[c] < 0) {
            ok = false;
        }
    }
    std::erase_if(whole, [](const auto& entry) { return entry.second == 0; });
    return whole;
}

bool is_subsequence(std::string_view small, std::string_view big) {
    std::size_t i = 0;
    for (const char c : big) {
        if (i < small.size() && small[i] == c) {
            ++i;
        }
    }
    return i == small.size();
}

std::string member_text(const std::string& key, const json& value) {
    return json(key).dump() + ": " + value.dump();
}

json reference_set(json root, const std::string& key, const json& value) {
    root[key] = value;
    return root;
}

void erase_key(json& object, const std::string& key) {
    if (const auto found = object.find(key); found != object.end()) {
        object.erase(found);
        return;
    }
    for (std::size_t dot = key.find('.'); dot != std::string::npos; dot = key.find('.', dot + 1)) {
        if (const auto child = object.find(key.substr(0, dot));
            child != object.end() && child->is_object()) {
            json before = *child;
            erase_key(*child, key.substr(dot + 1));
            if (*child != before) {
                return;
            }
        }
    }
}

constexpr int documents = 1500;

TEST(EditJsoncModel, ReplacingAValueChangesThoseBytesAndNothingElse) {
    Generator gen(1);
    int edits = 0;
    for (int n = 0; n < documents; ++n) {
        const Document doc = gen.make();
        ASSERT_TRUE(parse_jsonc(doc.text).has_value()) << doc.text;
        for (const Leaf& leaf : doc.leaves) {
            const json value = gen.scalar();
            const auto result = edit_jsonc_property(doc.text, leaf.key, value);
            ASSERT_TRUE(result.has_value()) << doc.text;
            const std::string expected = doc.text.substr(0, leaf.value_begin) + value.dump() +
                                         doc.text.substr(leaf.value_end);
            ASSERT_EQ(*result, expected) << "key " << leaf.key << " in\n" << doc.text;
            const auto settings = Settings::parse(*result);
            ASSERT_TRUE(settings.has_value());
            ASSERT_NE(settings->find(leaf.key), nullptr);
            EXPECT_EQ(*settings->find(leaf.key), value);
            ++edits;
        }
    }
    EXPECT_GT(edits, 2000);
}

TEST(EditJsoncModel, AddingAKeyOnlyAddsItAndItsCommas) {
    Generator gen(2);
    int edits = 0;
    for (int n = 0; n < documents; ++n) {
        const Document doc = gen.make();
        const auto old_json = parse_jsonc(doc.text);
        ASSERT_TRUE(old_json.has_value());
        const std::string key = "new." + std::to_string(n) + ".key";
        const json value = gen.scalar();

        const auto result = edit_jsonc_property(doc.text, key, value);
        ASSERT_TRUE(result.has_value()) << doc.text;
        const auto parsed = parse_jsonc(*result);
        ASSERT_TRUE(parsed.has_value()) << *result;
        ASSERT_EQ(*parsed, reference_set(*old_json, key, value)) << *result;

        // Everything of the old text is still there, in order, and what came in is the property
        // and at most two commas.
        ASSERT_TRUE(is_subsequence(doc.text, *result)) << doc.text << "\n=>\n" << *result;
        bool ok = false;
        const auto added = difference(glyphs(*result), glyphs(doc.text), ok);
        ASSERT_TRUE(ok);
        auto expected_added = glyphs(member_text(key, value));
        bool fits = false;
        auto commas = difference(added, expected_added, fits);
        ASSERT_TRUE(fits) << *result;
        for (const auto& [c, count] : commas) {
            ASSERT_EQ(c, ',') << *result;
            ASSERT_LE(count, 2) << *result;
        }
        ++edits;
    }
    EXPECT_EQ(edits, documents);
}

TEST(EditJsoncModel, RemovingAKeyTakesOutThePropertyAndItsCommaAndNothingElse) {
    Generator gen(3);
    int edits = 0;
    for (int n = 0; n < documents; ++n) {
        const Document doc = gen.make();
        const auto old_json = parse_jsonc(doc.text);
        ASSERT_TRUE(old_json.has_value());
        for (const Leaf& leaf : doc.leaves) {
            const auto result = edit_jsonc_property(doc.text, leaf.key, std::nullopt);
            ASSERT_TRUE(result.has_value()) << doc.text;
            const auto parsed = parse_jsonc(*result);
            ASSERT_TRUE(parsed.has_value()) << "key " << leaf.key << "\n"
                                            << doc.text << "=>\n"
                                            << *result;
            json expected = *old_json;
            erase_key(expected, leaf.key);
            ASSERT_EQ(*parsed, expected) << "key " << leaf.key << "\n"
                                         << doc.text << "=>\n"
                                         << *result;

            ASSERT_TRUE(is_subsequence(*result, doc.text)) << doc.text << "\n=>\n" << *result;
            bool ok = false;
            const auto removed = difference(glyphs(doc.text), glyphs(*result), ok);
            ASSERT_TRUE(ok);
            auto expected_removed =
                glyphs(doc.text.substr(leaf.removed_begin, leaf.removed_end - leaf.removed_begin));
            if (!leaf.has_comma && leaf.has_previous) {
                // The comma of the property before it goes too.
                ++expected_removed[','];
            }
            ASSERT_EQ(removed, expected_removed) << "key " << leaf.key << "\n"
                                                 << doc.text << "=>\n"
                                                 << *result;
            ++edits;
        }
    }
    EXPECT_GT(edits, 2000);
}

TEST(EditJsoncModel, EditingTwiceWithTheSameValueChangesNothingTheSecondTime) {
    Generator gen(4);
    for (int n = 0; n < documents; ++n) {
        const Document doc = gen.make();
        const std::string key = n % 2 == 0 && !doc.leaves.empty() ? doc.leaves.front().key
                                                                  : "added." + std::to_string(n);
        const json value = gen.scalar();
        const auto once = edit_jsonc_property(doc.text, key, value);
        ASSERT_TRUE(once.has_value());
        const auto twice = edit_jsonc_property(*once, key, value);
        ASSERT_TRUE(twice.has_value());
        ASSERT_EQ(*twice, *once) << doc.text;
    }
}

TEST(EditJsoncModel, AddingThenRemovingGivesBackTheSettings) {
    Generator gen(5);
    for (int n = 0; n < documents; ++n) {
        const Document doc = gen.make();
        const auto old_json = parse_jsonc(doc.text);
        ASSERT_TRUE(old_json.has_value());
        const std::string key = "tmp." + std::to_string(n);
        const auto added = edit_jsonc_property(doc.text, key, gen.scalar());
        ASSERT_TRUE(added.has_value());
        const auto removed = edit_jsonc_property(*added, key, std::nullopt);
        ASSERT_TRUE(removed.has_value());
        const auto back = parse_jsonc(*removed);
        ASSERT_TRUE(back.has_value()) << *removed;
        ASSERT_EQ(*back, *old_json) << doc.text << "=>\n" << *added << "=>\n" << *removed;
    }
}

} // namespace
