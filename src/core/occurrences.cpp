#include "anvil/core/editing.hpp"

#include <algorithm>

namespace anvil::core {
namespace {

bool overlaps(const Selection& selection, const Match& match) {
    if (selection.empty()) {
        return match.begin <= selection.start() && selection.start() <= match.end;
    }
    return match.begin < selection.end() && selection.start() < match.end;
}

bool overlaps_any(const std::vector<Selection>& selections, const Match& match) {
    return std::ranges::any_of(
        selections, [&](const Selection& selection) { return overlaps(selection, match); });
}

} // namespace

std::optional<OccurrenceQuery> EditController::occurrence_query(bool match_case,
                                                                bool whole_word) const {
    const Text& text = document_.text();
    const SelectionSet& set = document_.selections();
    const Selection primary = set.primary();
    const bool lone_caret = set.all().size() == 1 && primary.empty();
    if (lone_caret) {
        match_case = true;
        whole_word = true;
    }

    OccurrenceQuery result;
    result.query.match_case = match_case;
    result.query.whole_word = whole_word;
    if (primary.empty()) {
        const std::optional<Selection> word = word_at(text, primary.start());
        if (!word) {
            return std::nullopt;
        }
        result.query.pattern = text.slice(word->start(), word->end() - word->start());
        result.first = word;
    } else {
        result.query.pattern = text.slice(primary.start(), primary.end() - primary.start());
    }
    return result;
}

bool EditController::selections_share_text(bool match_case) const {
    const Text& text = document_.text();
    const auto& selections = document_.selections().all();
    const Selection& first = selections.front();
    if (selections.size() == 1) {
        return true;
    }
    auto search = Search::create({.pattern = text.slice(first.start(), first.end() - first.start()),
                                  .match_case = match_case});
    if (!search) {
        return false;
    }
    for (std::size_t i = 1; i < selections.size(); ++i) {
        const Selection& selection = selections[i];
        const auto found = search->find_all(text, selection.start(), selection.end(), 1);
        if (!found || found->matches.size() != 1 || found->matches[0].begin != selection.start() ||
            found->matches[0].end != selection.end()) {
            return false;
        }
    }
    return true;
}

EditOutcome EditController::expand_carets_to_words() {
    const Text& text = document_.text();
    std::vector<Selection> selections = document_.selections().all();
    for (Selection& selection : selections) {
        if (selection.empty()) {
            if (const auto word = word_at(text, selection.start())) {
                selection = *word;
            }
        }
    }
    return adopt(SelectionSet::from(std::move(selections), document_.selections().primary_index()));
}

EditOutcome EditController::add_selection(Selection selection) {
    std::vector<Selection> selections = document_.selections().all();
    selections.push_back(selection);
    return adopt(SelectionSet::from(std::move(selections), selections.size() - 1));
}

EditOutcome EditController::add_next_occurrence(const Search& search) {
    const Text& text = document_.text();
    const auto& selections = document_.selections().all();
    const std::size_t origin = document_.selections().primary().end();

    // The first match that begins in [from, before) and overlaps no selection.
    const auto scan = [&](std::size_t from, std::size_t before) -> std::optional<Match> {
        while (from < before) {
            const auto hit = search.find_next(text, from);
            if (!hit || !*hit || (*hit)->match.begin >= before) {
                return std::nullopt;
            }
            const Match match = (*hit)->match;
            if (!overlaps_any(selections, match)) {
                return match;
            }
            from = std::max(match.end, match.begin + 1);
        }
        return std::nullopt;
    };
    std::optional<Match> next = scan(origin, text.size() + 1);
    if (!next) {
        next = scan(0, origin);
    }
    if (!next) {
        return {};
    }
    return add_selection(Selection{next->begin, next->end});
}

EditOutcome EditController::add_previous_occurrence(const Search& search) {
    const Text& text = document_.text();
    const auto& selections = document_.selections().all();
    const std::size_t origin = document_.selections().primary().start();

    // The last match that begins in [lower, before) and overlaps no selection.
    const auto scan = [&](std::size_t before, std::size_t lower) -> std::optional<Match> {
        while (before > lower) {
            const auto hit = search.find_previous(text, before);
            if (!hit || !*hit || (*hit)->match.begin < lower) {
                return std::nullopt;
            }
            const Match match = (*hit)->match;
            if (!overlaps_any(selections, match)) {
                return match;
            }
            before = match.begin;
        }
        return std::nullopt;
    };
    std::optional<Match> previous = scan(origin, 0);
    if (!previous) {
        previous = scan(text.size() + 1, origin);
    }
    if (!previous) {
        return {};
    }
    return add_selection(Selection{previous->begin, previous->end});
}

EditController::AllOccurrences EditController::select_all_occurrences(const Search& search,
                                                                      std::size_t limit) {
    const Text& text = document_.text();
    const auto found = search.find_all(text, 0, text.size(), limit);
    if (!found || found->matches.empty()) {
        return {};
    }
    const Selection primary = document_.selections().primary();
    std::vector<Selection> selections;
    selections.reserve(found->matches.size());
    std::optional<std::size_t> meeting;
    for (const Match& match : found->matches) {
        if (!meeting && match.begin <= primary.end() && primary.start() <= match.end) {
            meeting = selections.size();
        }
        selections.push_back({match.begin, match.end});
    }
    AllOccurrences result;
    result.truncated = found->truncated;
    result.outcome = adopt(SelectionSet::from(std::move(selections), meeting.value_or(0)));
    return result;
}

} // namespace anvil::core
