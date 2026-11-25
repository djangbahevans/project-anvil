#include "anvil/ui/keybindings_reference.hpp"

#include <QString>
#include <algorithm>
#include <set>
#include <vector>

namespace anvil::ui {

std::string keybindings_reference(const CommandTitles& titles, const core::KeybindingTable& table,
                                  core::KeyPlatform platform) {
    struct Line {
        QString sort_key;
        std::string id;
        std::string text;
    };
    std::vector<Line> lines;
    std::set<std::string, std::less<>> seen;
    for (const core::Keybinding& binding : table.bindings()) {
        if (binding.command.empty() || !seen.insert(binding.command).second) {
            continue;
        }
        std::vector<std::string> labels;
        for (const core::Keybinding* bound : table.lookup(binding.command)) {
            std::string label = core::key_chord_label(bound->chord, platform);
            if (!label.empty() && std::ranges::find(labels, label) == labels.end()) {
                labels.push_back(std::move(label));
            }
        }
        if (labels.empty()) {
            continue;
        }
        const auto title = titles.find(binding.command);
        const std::string& name =
            title != titles.end() && !title->second.empty() ? title->second : binding.command;
        std::string text = name + '\t';
        for (std::size_t i = 0; i < labels.size(); ++i) {
            text += (i == 0 ? "" : ", ") + labels[i];
        }
        lines.push_back({QString::fromStdString(name).toCaseFolded(), binding.command, text});
    }
    std::ranges::sort(lines, [](const Line& a, const Line& b) {
        return a.sort_key != b.sort_key ? a.sort_key < b.sort_key : a.id < b.id;
    });

    std::string reference = "Keyboard Shortcuts Reference\n\n";
    for (const Line& line : lines) {
        reference += line.text + '\n';
    }
    return reference;
}

} // namespace anvil::ui
