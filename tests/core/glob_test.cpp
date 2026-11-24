#include "anvil/core/glob.hpp"

#include <gtest/gtest.h>

using anvil::core::glob_match;

TEST(Glob, LiteralPatternsMatchTheWholePath) {
    EXPECT_TRUE(glob_match("a/b.txt", "a/b.txt"));
    EXPECT_FALSE(glob_match("a/b.txt", "a/b.txt/c"));
    EXPECT_FALSE(glob_match("a/b", "x/a/b"));
    EXPECT_FALSE(glob_match("b.txt", "B.txt"));
}

TEST(Glob, StarStaysInOneSegment) {
    EXPECT_TRUE(glob_match("*.js", "a.js"));
    EXPECT_TRUE(glob_match("a*c", "abc"));
    EXPECT_TRUE(glob_match("a*c", "ac"));
    EXPECT_FALSE(glob_match("*.js", "src/a.js"));
    EXPECT_FALSE(glob_match("a*", "a/b"));
}

TEST(Glob, QuestionMarkIsOneCharacter) {
    EXPECT_TRUE(glob_match("a?c", "abc"));
    EXPECT_FALSE(glob_match("a?c", "ac"));
    EXPECT_FALSE(glob_match("a?c", "a/c"));
}

TEST(Glob, DoubleStarMatchesAnyNumberOfSegmentsIncludingNone) {
    EXPECT_TRUE(glob_match("**/*.js", "a.js"));
    EXPECT_TRUE(glob_match("**/*.js", "x/y/a.js"));
    EXPECT_TRUE(glob_match("**/node_modules/**", "node_modules"));
    EXPECT_TRUE(glob_match("**/node_modules/**", "a/node_modules"));
    EXPECT_TRUE(glob_match("**/node_modules/**", "a/node_modules/b/c.js"));
    EXPECT_FALSE(glob_match("**/node_modules/**", "a/node_modulesx/b"));
    EXPECT_TRUE(glob_match("**", ""));
    EXPECT_TRUE(glob_match("src/**", "src"));
    EXPECT_FALSE(glob_match("src/**", "lib/a"));
}

TEST(Glob, BracesGiveAlternativesThatMayContainSlashesAndNest) {
    EXPECT_TRUE(glob_match("*.{js,ts}", "a.ts"));
    EXPECT_FALSE(glob_match("*.{js,ts}", "a.css"));
    EXPECT_TRUE(glob_match("{a/b,c}/d", "a/b/d"));
    EXPECT_TRUE(glob_match("{a/b,c}/d", "c/d"));
    EXPECT_TRUE(glob_match("x{1,{2,3}}", "x3"));
    EXPECT_FALSE(glob_match("x{1,{2,3}}", "x4"));
}

TEST(Glob, CharacterClasses) {
    EXPECT_TRUE(glob_match("[abc].txt", "b.txt"));
    EXPECT_TRUE(glob_match("[a-c].txt", "b.txt"));
    EXPECT_FALSE(glob_match("[a-c].txt", "d.txt"));
    EXPECT_TRUE(glob_match("[!a-c].txt", "d.txt"));
    EXPECT_FALSE(glob_match("[^a-c].txt", "a.txt"));
}

TEST(Glob, UnclosedBracketsAreOrdinaryCharacters) {
    EXPECT_TRUE(glob_match("a{b", "a{b"));
    EXPECT_TRUE(glob_match("a[b", "a[b"));
}

TEST(Glob, EmptySegmentsAreIgnored) {
    EXPECT_TRUE(glob_match("/a/b", "a/b"));
    EXPECT_TRUE(glob_match("a//b", "a/b"));
    EXPECT_TRUE(glob_match("/home/me/**", "/home/me/x/y"));
}
