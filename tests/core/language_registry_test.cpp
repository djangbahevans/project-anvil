#include "anvil/core/language_registry.hpp"

#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

using namespace anvil::core;
using nlohmann::json;

namespace {

LanguageContribution language(std::string id) {
    return {.id = std::move(id),
            .aliases = std::nullopt,
            .extensions = {},
            .filenames = {},
            .filename_patterns = {},
            .first_line = "",
            .mime_types = {}};
}

LanguageContribution with_extensions(std::string id, std::vector<std::string> extensions) {
    LanguageContribution contribution = language(std::move(id));
    contribution.extensions = std::move(extensions);
    return contribution;
}

LanguageContribution with_filenames(std::string id, std::vector<std::string> filenames) {
    LanguageContribution contribution = language(std::move(id));
    contribution.filenames = std::move(filenames);
    return contribution;
}

LanguageContribution with_patterns(std::string id, std::vector<std::string> patterns) {
    LanguageContribution contribution = language(std::move(id));
    contribution.filename_patterns = std::move(patterns);
    return contribution;
}

LanguageContribution with_first_line(std::string id, std::string first_line) {
    LanguageContribution contribution = language(std::move(id));
    contribution.first_line = std::move(first_line);
    return contribution;
}

LanguageRegistry registry_of(std::initializer_list<LanguageContribution> contributions) {
    LanguageRegistry registry;
    for (const LanguageContribution& contribution : contributions) {
        const auto problem = registry.add(contribution);
        EXPECT_FALSE(problem.has_value()) << contribution.id << ": " << problem.value_or("");
    }
    return registry;
}

struct PathCase {
    std::string path;
    std::string first_line;
    std::string expected;
};

void expect_languages(const LanguageRegistry& registry, const std::vector<PathCase>& cases) {
    for (const PathCase& c : cases) {
        EXPECT_EQ(registry.language_for(c.path, c.first_line), c.expected)
            << c.path << " | " << c.first_line;
    }
}

} // namespace

TEST(LanguageRegistryTest, NothingThatMatchesIsPlainText) {
    const auto registry = registry_of({with_extensions("typescript", {".ts"})});
    expect_languages(registry, {{"/w/a.py", "", "plaintext"},
                                {"/w/ts", "", "plaintext"},
                                {"/w/a.ts.bak", "", "plaintext"},
                                {"", "", "plaintext"},
                                {"/", "", "plaintext"}});
    EXPECT_EQ(LanguageRegistry().language_for("/w/a.ts"), "plaintext");
}

TEST(LanguageRegistryTest, AnExactFileNameBeatsAPatternAndAPatternBeatsAnExtension) {
    const auto registry = registry_of({
        with_extensions("json", {".json"}),
        with_patterns("snippets", {"snippets*.json"}),
        with_filenames("jsonc", {"tsconfig.json"}),
    });
    expect_languages(registry, {{"/w/tsconfig.json", "", "jsonc"},
                                {"/w/snippets-x.json", "", "snippets"},
                                {"/w/other.json", "", "json"},
                                {"/w/tsconfig.json.bak", "", "plaintext"}});
}

TEST(LanguageRegistryTest, AnExactFileNameBeatsALongerPatternRegisteredLater) {
    const auto registry =
        registry_of({with_filenames("a", {"Makefile"}), with_patterns("b", {"Makefile*"}),
                     with_extensions("c", {"file"})});
    EXPECT_EQ(registry.language_for("/w/Makefile"), "a");
    EXPECT_EQ(registry.language_for("/w/Makefile.x"), "b");
    EXPECT_EQ(registry.language_for("/w/Mfile"), "c");
}

TEST(LanguageRegistryTest, TheLongestExtensionWinsWhateverTheOrder) {
    const auto short_first =
        registry_of({with_extensions("javascript", {".ts"}), with_extensions("dts", {".d.ts"})});
    const auto long_first =
        registry_of({with_extensions("dts", {".d.ts"}), with_extensions("javascript", {".ts"})});
    for (const LanguageRegistry* registry : {&short_first, &long_first}) {
        expect_languages(*registry, {{"/w/a.d.ts", "", "dts"},
                                     {"/w/a.ts", "", "javascript"},
                                     {"/w/.d.ts", "", "dts"},
                                     {"/w/a.x.d.ts", "", "dts"},
                                     {"/w/d.ts", "", "javascript"}});
    }
}

TEST(LanguageRegistryTest, TheLongestPatternWinsWhateverTheOrder) {
    const auto short_first =
        registry_of({with_patterns("a", {"*.json"}), with_patterns("b", {"tsconfig*.json"})});
    const auto long_first =
        registry_of({with_patterns("b", {"tsconfig*.json"}), with_patterns("a", {"*.json"})});
    for (const LanguageRegistry* registry : {&short_first, &long_first}) {
        expect_languages(*registry, {{"/w/tsconfig.app.json", "", "b"}, {"/w/x.json", "", "a"}});
    }
}

TEST(LanguageRegistryTest, TheLastRegisteredWinsAmongEqualRules) {
    const auto registry = registry_of({
        with_extensions("a", {".x"}),
        with_filenames("a", {"name"}),
        with_patterns("a", {"p*"}),
        with_extensions("b", {".x"}),
        with_filenames("b", {"name"}),
        with_patterns("b", {"p?"}),
    });
    expect_languages(
        registry,
        {{"/w/f.x", "", "b"}, {"/w/name", "", "b"}, {"/w/pq", "", "b"}, {"/w/pqr", "", "a"}});

    const auto reversed = registry_of({with_extensions("b", {".x"}), with_extensions("a", {".x"})});
    EXPECT_EQ(reversed.language_for("/w/f.x"), "a");
}

TEST(LanguageRegistryTest, TheLastRegisteredWinsForOneLanguageOverAnother) {
    auto registry =
        registry_of({with_extensions("json", {".json"}), with_extensions("jsonc", {".json"})});
    EXPECT_EQ(registry.language_for("/w/a.json"), "jsonc");
    EXPECT_TRUE(registry.add(with_extensions("json", {".json"})) == std::nullopt);
    EXPECT_EQ(registry.language_for("/w/a.json"), "json");
}

TEST(LanguageRegistryTest, MatchingIgnoresCaseInThePathAndInTheRules) {
    const auto registry =
        registry_of({with_extensions("c", {".C"}), with_filenames("make", {"Makefile"}),
                     with_patterns("docker", {"Dockerfile.*"})});
    expect_languages(registry, {{"/w/A.c", "", "c"},
                                {"/w/a.C", "", "c"},
                                {"/W/MAKEFILE", "", "make"},
                                {"/w/makefile", "", "make"},
                                {"/w/DOCKERFILE.dev", "", "docker"},
                                {"/w/dockerfile.DEV", "", "docker"}});
}

TEST(LanguageRegistryTest, ADotFileIsAFileNameAndAnExtension) {
    const auto registry =
        registry_of({with_filenames("ignore", {".gitignore"}), with_extensions("ts", {".ts"}),
                     with_extensions("env", {".env"})});
    expect_languages(registry, {{"/w/.gitignore", "", "ignore"},
                                {"/w/.ts", "", "ts"},
                                {"/w/.env", "", "env"},
                                {"/w/x.gitignore", "", "plaintext"},
                                {"/w/.env.local", "", "plaintext"}});
}

TEST(LanguageRegistryTest, OnlyTheLastSegmentOfThePathIsTheFileName) {
    const auto registry =
        registry_of({with_extensions("ts", {".ts"}), with_filenames("make", {"makefile"})});
    expect_languages(registry, {{"/w/a.ts/readme", "", "plaintext"},
                                {"/w/makefile/x", "", "plaintext"},
                                {"/w/a.ts/", "", "ts"},
                                {"/w/makefile//", "", "make"},
                                {"a.ts", "", "ts"},
                                {"makefile", "", "make"}});
}

TEST(LanguageRegistryTest, APatternWithASlashMatchesTheWholePathAndOtherPatternsTheFileName) {
    const auto registry =
        registry_of({with_patterns("git", {"**/.git/config"}), with_patterns("rule", {"*.mdc"}),
                     with_patterns("snippets", {"**/User/snippets/*.json"})});
    expect_languages(registry, {{"/home/u/.git/config", "", "git"},
                                {"/.git/config", "", "git"},
                                {"/home/u/.git/config.bak", "", "plaintext"},
                                {"/home/u/config", "", "plaintext"},
                                {"/w/dir/a.mdc", "", "rule"},
                                {"/w/a.mdc/b", "", "plaintext"},
                                {"/w/User/snippets/a.json", "", "snippets"},
                                {"/w/User/snippets/x/a.json", "", "plaintext"},
                                {"/W/USER/Snippets/A.JSON", "", "snippets"}});
}

TEST(LanguageRegistryTest, ThePatternDialectIsTheGlobsOfVsCode) {
    const auto registry =
        registry_of({with_patterns("compose", {"*docker*compose*.yml", "compose.*.y{a,}ml"})});
    expect_languages(registry, {{"/w/my-docker-compose.override.yml", "", "compose"},
                                {"/w/compose.prod.yaml", "", "compose"},
                                {"/w/compose.prod.yml", "", "compose"},
                                {"/w/compose.yml", "", "plaintext"}});
}

TEST(LanguageRegistryTest, TheUserAssociationsComeBeforeEveryRegisteredRule) {
    auto registry =
        registry_of({with_filenames("make", {"Makefile"}), with_extensions("ts", {".ts"}),
                     with_extensions("python", {".py"}), with_extensions("json", {".json"})});
    registry.set_user_associations({{"*.ts", "python"}, {"Makefile", "json"}});
    expect_languages(
        registry,
        {{"/w/a.ts", "", "python"}, {"/w/Makefile", "", "json"}, {"/w/a.py", "", "python"}});
}

TEST(LanguageRegistryTest, AUserAssociationCanBeSetBeforeTheLanguageIsRegistered) {
    LanguageRegistry registry;
    registry.set_user_associations({{"*.foo", "foo"}});
    EXPECT_EQ(registry.language_for("/w/a.foo"), "plaintext");
    EXPECT_FALSE(registry.add(with_extensions("foo", {".bar"})).has_value());
    EXPECT_EQ(registry.language_for("/w/a.foo"), "foo");
}

TEST(LanguageRegistryTest, TheLongestUserPatternWinsAndTheLastAmongEqualOnes) {
    auto registry = registry_of({language("a"), language("b"), language("c")});
    registry.set_user_associations({{"*.ts", "a"}, {"**/src/*.ts", "b"}, {"*.ts", "c"}});
    expect_languages(registry, {{"/w/src/x.ts", "", "b"}, {"/w/x.ts", "", "c"}});
    registry.set_user_associations({{"*.ts", "a"}, {"*.ts", "b"}});
    EXPECT_EQ(registry.language_for("/w/x.ts"), "b");
}

TEST(LanguageRegistryTest, UserPatternsIgnoreCaseAndMatchThePathWhenTheyHaveASlash) {
    auto registry = registry_of({language("a"), language("b")});
    registry.set_user_associations({{"*.SH", "a"}, {"Build/**", "b"}});
    expect_languages(registry, {{"/w/x.sh", "", "a"},
                                {"/w/X.Sh", "", "a"},
                                {"/w/Build/x/y", "", "plaintext"},
                                {"Build/x/y", "", "b"},
                                {"/w/build/x.sh", "", "a"}});
}

TEST(LanguageRegistryTest, AUserAssociationOfAnUnknownLanguageIsPlainTextAndStopsTheSearch) {
    auto registry = registry_of({with_extensions("ts", {".ts"}), with_first_line("shell", "#!")});
    registry.set_user_associations({{"*.ts", "nosuchlanguage"}});
    EXPECT_EQ(registry.language_for("/w/a.ts", "#!"), "plaintext");
}

TEST(LanguageRegistryTest, SettingTheUserAssociationsReplacesTheEarlierOnes) {
    auto registry = registry_of({language("a"), language("b")});
    registry.set_user_associations({{"*.x", "a"}});
    registry.set_user_associations({{"*.y", "b"}});
    expect_languages(registry, {{"/w/f.x", "", "plaintext"}, {"/w/f.y", "", "b"}});
    registry.set_user_associations({});
    EXPECT_EQ(registry.language_for("/w/f.y"), "plaintext");
}

TEST(LanguageRegistryTest, TheFirstLineDecidesOnlyWhenThePathDecidesNothing) {
    const auto registry =
        registry_of({with_extensions("ts", {".ts"}), with_first_line("python", R"(#!.*\bpython)")});
    expect_languages(registry, {{"/w/script", "#!/usr/bin/env python3", "python"},
                                {"/w/a.ts", "#!/usr/bin/env python3", "ts"},
                                {"/w/script", "#!/usr/bin/env ruby", "plaintext"},
                                {"/w/script", "", "plaintext"}});
}

TEST(LanguageRegistryTest, TheFirstLineIsNotReadWhenThePathIsEmpty) {
    const auto registry = registry_of({with_first_line("python", R"(#!.*\bpython)")});
    EXPECT_EQ(registry.language_for("", "#!/usr/bin/env python3"), "plaintext");
}

TEST(LanguageRegistryTest, TheFirstLineExpressionIsAnchoredAtTheStartButNotItsAlternatives) {
    const auto registry = registry_of({with_first_line("xml", R"(<\?xml|<svg)")});
    expect_languages(registry, {{"/w/f", "<?xml version", "xml"},
                                {"/w/f", " <?xml", "plaintext"},
                                {"/w/f", "<svg>", "xml"},
                                {"/w/f", "text <svg>", "xml"}});
    const auto anchored = registry_of({with_first_line("cloud", "^#cloud-config")});
    expect_languages(anchored,
                     {{"/w/f", "#cloud-config", "cloud"}, {"/w/f", " #cloud-config", "plaintext"}});
}

TEST(LanguageRegistryTest, TheLastRegisteredFirstLineWins) {
    const auto registry = registry_of({with_first_line("a", "#!"), with_first_line("b", "#!/bin")});
    EXPECT_EQ(registry.language_for("/w/f", "#!/bin/sh"), "b");
    EXPECT_EQ(registry.language_for("/w/f", "#!/usr/bin/x"), "a");
}

TEST(LanguageRegistryTest, AByteOrderMarkInFrontOfTheFirstLineIsIgnored) {
    const auto registry =
        registry_of({with_first_line("shell", "#!"), with_first_line("blank", R"(\s*$)")});
    expect_languages(registry, {{"/w/f", "\xEF\xBB\xBF#!/bin/sh", "shell"},
                                {"/w/f", "\xEF\xBB\xBF", "plaintext"},
                                {"/w/f", "\xEF\xBB\xBF\xEF\xBB\xBF#!", "plaintext"}});
}

TEST(LanguageRegistryTest, AFirstLineThatDoesNotCompileIsReportedAndTheRestIsRegistered) {
    LanguageRegistry registry;
    LanguageContribution broken = with_first_line("broken", "(unclosed");
    broken.extensions = {".brk"};
    const auto problem = registry.add(broken);
    ASSERT_TRUE(problem.has_value());
    EXPECT_NE(problem->find("broken"), std::string::npos) << *problem;
    EXPECT_EQ(registry.language_for("/w/a.brk", "(unclosed"), "broken");
    EXPECT_EQ(registry.language_for("/w/a", "(unclosed"), "plaintext");
    ASSERT_NE(registry.language("broken"), nullptr);
}

TEST(LanguageRegistryTest, AFirstLineThatMatchesTheEmptyStringIsReportedAndNotUsed) {
    LanguageRegistry registry;
    for (const char* endless : {"a*", "(?:)", "x?", "(a|b*)", "\\s*", ".*"}) {
        const auto problem = registry.add(with_first_line("loop", endless));
        EXPECT_TRUE(problem.has_value()) << endless;
    }
    EXPECT_EQ(registry.language_for("/w/f", "aaa"), "plaintext");
    EXPECT_EQ(registry.language_for("/w/f", "anything"), "plaintext");
}

TEST(LanguageRegistryTest, TheBlankLineExpressionsThatVsCodeAllowsAreUsed) {
    for (const char* allowed : {"^", "^$", "^\\s*$"}) {
        LanguageRegistry registry;
        EXPECT_FALSE(registry.add(with_first_line("blank", allowed)).has_value()) << allowed;
    }
    LanguageRegistry registry;
    EXPECT_FALSE(registry.add(with_first_line("blank", R"(\s*$)")).has_value());
    EXPECT_EQ(registry.language_for("/w/f", "   "), "blank");
    EXPECT_EQ(registry.language_for("/w/f", " x "), "plaintext");
}

TEST(LanguageRegistryTest, TheFirstLineIsMatchedAsUtf8Characters) {
    const auto registry = registry_of({with_first_line("accent", "é.$")});
    expect_languages(
        registry,
        {{"/w/f", "é!", "accent"}, {"/w/f", "é", "plaintext"}, {"/w/f", "e!", "plaintext"}});
}

TEST(LanguageRegistryTest, AFirstLineThatIsNotUtf8DoesNotMatch) {
    const auto registry = registry_of({with_first_line("any", "#")});
    EXPECT_EQ(registry.language_for("/w/f", "#\xFF"), "plaintext");
}

TEST(LanguageRegistryTest, AContributionsWithTheSameIdAreMergedIntoOneLanguage) {
    LanguageRegistry registry;
    LanguageContribution first = with_extensions("json", {".json", ".map"});
    first.aliases = std::vector<std::string>{"JSON", "json"};
    first.mime_types = {"application/json"};
    LanguageContribution second = with_filenames("json", {"composer.lock"});
    second.extensions = {".map", ".webmanifest"};
    registry.add(first);
    registry.add(language("yaml"));
    registry.add(second);

    EXPECT_EQ(registry.language_ids(), (std::vector<std::string>{"json", "yaml"}));
    const Language* json = registry.language("json");
    ASSERT_NE(json, nullptr);
    EXPECT_EQ(json->name, "JSON");
    EXPECT_EQ(json->aliases, (std::vector<std::string>{"json", "JSON"}));
    EXPECT_EQ(json->mime_types, (std::vector<std::string>{"application/json", "text/x-json"}));
    EXPECT_EQ(json->extensions, (std::vector<std::string>{".json", ".map", ".webmanifest"}));
    EXPECT_EQ(json->filenames, (std::vector<std::string>{"composer.lock"}));
    EXPECT_EQ(registry.language("nosuch"), nullptr);
}

TEST(LanguageRegistryTest, TheNameIsTheFirstAliasOrTheIdOrNothing) {
    LanguageRegistry registry;
    const auto named = [&](std::string id, std::optional<std::vector<std::string>> aliases) {
        LanguageContribution contribution = language(std::move(id));
        contribution.aliases = std::move(aliases);
        registry.add(contribution);
    };
    named("a", std::vector<std::string>{"Alpha", "a"});
    named("b", std::nullopt);
    named("c", std::vector<std::string>{});
    named("d", std::vector<std::string>{"", "Delta"});
    // A later contribution renames the language when it lists aliases, keeps the name when it lists
    // none, and does not remove it when its list is empty.
    named("a", std::nullopt);
    named("b", std::vector<std::string>{"Bravo"});
    named("c", std::vector<std::string>{"Charlie"});
    named("c", std::vector<std::string>{});
    named("e", std::nullopt);
    named("e", std::vector<std::string>{});

    EXPECT_EQ(registry.language("a")->name, "Alpha");
    EXPECT_EQ(registry.language("b")->name, "Bravo");
    EXPECT_EQ(registry.language("c")->name, "Charlie");
    EXPECT_EQ(registry.language("d")->name, "d");
    EXPECT_EQ(registry.language("e")->name, "e");

    LanguageRegistry unnamed;
    LanguageContribution contribution = language("hidden");
    contribution.aliases = std::vector<std::string>{};
    unnamed.add(contribution);
    EXPECT_EQ(unnamed.language("hidden")->name, std::nullopt);
    EXPECT_EQ(unnamed.language("hidden")->aliases, (std::vector<std::string>{"hidden"}));
}

TEST(BuiltinLanguagesTest, TheTableHoldsPlainTextFirstAndVsCodesLanguages) {
    const auto registry = LanguageRegistry::with_builtin_languages();
    ASSERT_FALSE(registry.language_ids().empty());
    EXPECT_EQ(registry.language_ids().front(), "plaintext");
    EXPECT_GE(registry.language_ids().size(), 70U);
    ASSERT_NE(registry.language("plaintext"), nullptr);
    EXPECT_EQ(registry.language("plaintext")->name, "Plain Text");
    ASSERT_NE(registry.language("typescript"), nullptr);
    EXPECT_EQ(registry.language("typescript")->name, "TypeScript");
}

TEST(BuiltinLanguagesTest, FilesAreGivenTheLanguageOfVsCode) {
    const auto registry = LanguageRegistry::with_builtin_languages();
    expect_languages(registry, {{"/w/a.txt", "", "plaintext"},
                                {"/w/a.ts", "", "typescript"},
                                {"/w/a.d.ts", "", "typescript"},
                                {"/w/a.cpp", "", "cpp"},
                                {"/w/Makefile", "", "makefile"},
                                {"/w/Dockerfile", "", "dockerfile"},
                                {"/w/Dockerfile.dev", "", "dockerfile"},
                                {"/w/.gitignore", "", "ignore"},
                                {"/w/tsconfig.json", "", "jsonc"},
                                {"/w/a.json", "", "json"},
                                {"/w/a.md", "", "markdown"},
                                {"/w/script", "#!/usr/bin/env python3", "python"},
                                {"/w/script", "#!/bin/bash", "shellscript"},
                                {"/w/unknown", "", "plaintext"}});
}

TEST(BuiltinLanguagesTest, ARegistryCanBeCopiedAndExtended) {
    auto base = LanguageRegistry::with_builtin_languages();
    LanguageRegistry copy = base;
    EXPECT_FALSE(copy.add(with_extensions("anvil", {".anvil"})).has_value());
    EXPECT_EQ(copy.language_for("/w/a.anvil"), "anvil");
    EXPECT_EQ(base.language_for("/w/a.anvil"), "plaintext");
}

namespace {

std::vector<std::string> unique_in_order(const json& values) {
    std::vector<std::string> unique;
    for (const json& value : values) {
        if (std::ranges::find(unique, value.get<std::string>()) == unique.end()) {
            unique.push_back(value);
        }
    }
    return unique;
}

json recorded_language_cases() {
    std::ifstream file(std::string(ANVIL_CORE_TEST_DATA_DIR) + "/language_cases.json");
    EXPECT_TRUE(file.is_open());
    return json::parse(file);
}

} // namespace

TEST(LanguageReplayTest, TheLanguagesAreTheOnesVsCodeRegistersFromTheSameContributions) {
    const json recorded = recorded_language_cases();
    const auto registry = LanguageRegistry::with_builtin_languages();

    std::vector<std::string> ids;
    for (const json& entry : recorded["languages"]) {
        ids.push_back(entry["id"]);
    }
    ASSERT_EQ(registry.language_ids(), ids);

    for (const json& entry : recorded["languages"]) {
        const std::string id = entry["id"];
        const Language* language = registry.language(id);
        ASSERT_NE(language, nullptr) << id;
        EXPECT_EQ(language->name.has_value(), !entry["name"].is_null()) << id;
        if (language->name && !entry["name"].is_null()) {
            EXPECT_EQ(*language->name, entry["name"].get<std::string>()) << id;
        }
        EXPECT_EQ(language->mime_types, unique_in_order(entry["mime_types"])) << id;
        EXPECT_EQ(language->filenames, unique_in_order(entry["filenames"])) << id;
        std::vector<std::string> extensions = language->extensions;
        std::ranges::sort(extensions);
        std::vector<std::string> expected = unique_in_order(entry["extensions"]);
        std::ranges::sort(expected);
        EXPECT_EQ(extensions, expected) << id;
    }
}

TEST(LanguageReplayTest, EveryCaseRecordedFromVsCodeGivesTheSameLanguage) {
    const json recorded = recorded_language_cases();
    const auto registry = LanguageRegistry::with_builtin_languages();
    ASSERT_GE(recorded["cases"].size(), 500U);

    std::set<std::string> seen;
    for (const json& c : recorded["cases"]) {
        const std::string path = c["path"];
        const std::string first_line = c["first_line"];
        const std::string expected = c["language"];
        EXPECT_EQ(registry.language_for(path, first_line), expected) << path << " | " << first_line;
        seen.insert(expected);
    }
    EXPECT_GE(seen.size(), 60U);
}

TEST(LanguageReplayTest, EveryCaseWithUserAssociationsGivesTheSameLanguage) {
    const json recorded = recorded_language_cases();
    auto registry = LanguageRegistry::with_builtin_languages();
    ASSERT_GE(recorded["user_cases"].size(), 10U);

    for (const json& set : recorded["user_cases"]) {
        std::vector<FileAssociation> associations;
        for (const json& association : set["associations"]) {
            associations.push_back({association["pattern"], association["language"]});
        }
        registry.set_user_associations(associations);
        for (const json& c : set["cases"]) {
            const std::string path = c["path"];
            const std::string first_line = c["first_line"];
            EXPECT_EQ(registry.language_for(path, first_line), c["language"].get<std::string>())
                << set["associations"].dump() << " " << path << " | " << first_line;
        }
    }
}
