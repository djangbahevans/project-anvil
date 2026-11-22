// Records what VS Code's own fuzzy scorer returns for a fixed corpus, as tests/core/data/fuzzy_cases.json.
// tests/core/fuzzy_test.cpp replays the cases against core/fuzzy.cpp, so the port stays faithful.
// The scorer is loaded from the pinned VS Code checkout, so the file changes when that is upgraded.
//   node scripts/generate-fuzzy-cases.mjs [--check]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const vscodeDir = path.join(root, '.deps', 'vscode');
const output = path.join(root, 'tests/core/data/fuzzy_cases.json');

if (process.platform !== 'linux') {
    throw new Error('the scorer reads the platform (path separator, case-sensitive paths): run this on Linux');
}
const scorer = await import(path.join(vscodeDir, 'out/vs/base/common/fuzzyScorer.js'));
const version = JSON.parse(fs.readFileSync(path.join(vscodeDir, 'package.json'), 'utf8')).version;

const accessor = {
    getItemLabel: (item) => item.label,
    getItemDescription: (item) => item.description,
    getItemPath: (item) => item.path,
};

// Case is folded for ASCII only in the port, so the corpus holds no non-ASCII letter that has
// another case. Characters without case (accents on lower-case letters, CJK, emoji) are included.
const commands = [
    'Open File…', 'Open Folder…', 'Save', 'Save As…', 'Save All', 'Close Editor', 'Close All Editors',
    'View: Toggle Terminal', 'View: Toggle Sidebar Visibility', 'Go to Line/Column…', 'Go to File…',
    'Preferences: Open User Settings (JSON)', 'Preferences: Color Theme', 'Format Document',
    'Format Selection', 'Developer: Reload Window', 'Git: Checkout to…', 'Git: Commit All',
    'Find in Files', 'Replace in Files', 'Select All', 'Undo', 'Redo', 'Cut', 'Copy', 'Paste',
    'Move Cursor to Line Start', 'Delete Left', 'Toggle Line Comment', 'Fold All', 'Unfold All',
    'NullPointerException', 'HTTPServer', 'fooBarBaz', 'edit_distance_test.cpp', 'some-dashed-name',
    '🔍 Find Symbol', '検索: ファイル', 'café menu', 'a b c', 'x', 'ede',
];
const descriptions = [undefined, undefined, 'File', 'View', 'Preferences', 'src/ui', 'Developer', 'git'];
const files = [
    '/xyz/some/path/someFile123.txt', '/xyz/others/spath/some/xsp/file123.txt', 'projects/ui/cula/ats/target.mk',
    '/1a111d1/11a1d1/something.txt', 'config/test/t1.js', 'config/test.js', 'parts/quick/arrow-left-dark.svg',
    'parts/quickopen/quickopen.ts', '/src/ui/main_window.cpp', '/src/core/document.cpp', 'abcde/super/duper',
    '/some/path/fileA.txt', '/some/path/other/fileB.txt', 'ASDFasdfasdf', 'asdfasdfasdf',
];

const items = [];
for (const [i, label] of commands.entries()) {
    items.push({ label, description: descriptions[i % descriptions.length] });
}
for (const file of files) {
    const slash = file.lastIndexOf('/');
    items.push({ label: file.slice(slash + 1), description: slash > 0 ? file.slice(0, slash) : '', path: file });
}

const queries = [
    'o', 'op', 'open', 'Open', 'of', 'file', 'fl', 'sav', 'sa', 'sf', 'tt', 'tog', 'vtt', 'gtl', 'goto',
    'json', 'pref', 'ct', 'fd', 'ff', 'fif', 'rif', 'sel', 'sa all', 'undo', 'mcl', 'npe', 'NPE', 'http',
    'fbb', 'fB', 'edt', 'dash', 'find sym', 'ファイル', 'café', 'cafe', '🔍', 'a', 'x', 'de', 'ad', 'ASDF',
    'asdf', 'sF', 'of', 'xyz123', 'xyz some', 'some xyz', 'some xyz file file123', 'path z y', 'xspfile123',
    'tcltarget.mk', 'co/te', 'partsquick', 'src/ui/main', 'src\\ui\\main', 'abcde/super/duper',
    'abcde\\super\\duper', 'doc.cpp', '"open"', '"op en"', '"fl"', '"sF"', 'o*pen', 'open…', 'open#', '#',
    ' open ', 'open file', 'file open', 'e  d', '"', '', '987', 'zzz', '/some/path/fileA.txt',
    '/some/path/other/fileB.txt', 'fileA', 'fileB', 'pathfileA', 'somepath', 'tab\there',
];

function ranges(matches) {
    return (matches ?? []).map((m) => [m.start, m.end]);
}

let seed = 0x9e3779b9;
function random() {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
}
// Cases that match are all kept; a tenth of the cases that do not are kept too, to keep the file small.
const keep = (score) => score !== 0 || random() < 0.1;

const data = { vscode: version, scoreFuzzy: [], scoreItem: [], compare: [], prepareQuery: [] };

for (const query of queries) {
    const prepared = scorer.prepareQuery(query);
    data.prepareQuery.push({
        query,
        normalized: prepared.normalized,
        normalizedLowercase: prepared.normalizedLowercase,
        pathNormalized: prepared.pathNormalized,
        expectContiguousMatch: prepared.expectContiguousMatch,
        containsPathSeparator: prepared.containsPathSeparator,
        pieces: prepared.values
            ? prepared.values.map((p) => ({ normalized: p.normalized, expectContiguousMatch: p.expectContiguousMatch }))
            : null,
    });
}

for (const item of items) {
    for (const query of queries) {
        const prepared = scorer.prepareQuery(query);
        for (const allow of [true, false]) {
            const [score, positions] = scorer.scoreFuzzy(item.label, prepared.normalized, prepared.normalizedLowercase, allow);
            if (keep(score)) {
                data.scoreFuzzy.push([item.label, query, allow, score, positions]);
            }
            const res = scorer.scoreItemFuzzy(item, prepared, allow, accessor, Object.create(null));
            if (keep(res.score)) {
                data.scoreItem.push([
                    item.label, item.description ?? '', item.path ?? '', query, allow, res.score,
                    ranges(res.labelMatch), ranges(res.descriptionMatch),
                ]);
            }
        }
    }
}

// Orderings. The result of a sort depends on its algorithm when a comparator is not a strict weak
// order, so the cases record the sign of the comparison of every pair instead. Items of one group
// have different label and description lengths, so no pair is decided by the collator-based last
// tie-break, which the port does not reproduce.
const lengthOf = (item) => item.label.length + (item.description ?? '').length;
for (const query of queries.filter((q) => q.length > 0)) {
    for (let attempt = 0; attempt < 2; attempt++) {
        const pool = [...items].sort(() => random() - 0.5);
        const group = [];
        const seen = new Set();
        for (const item of pool) {
            if (!seen.has(lengthOf(item))) {
                seen.add(lengthOf(item));
                group.push(item);
            }
            if (group.length === 10) {
                break;
            }
        }
        const prepared = scorer.prepareQuery(query);
        const signs = [];
        for (let a = 0; a < group.length; a++) {
            for (let b = a + 1; b < group.length; b++) {
                const result = scorer.compareItemsByFuzzyScore(group[a], group[b], prepared, true, accessor, Object.create(null));
                signs.push(Math.sign(result));
            }
        }
        data.compare.push({
            query,
            items: group.map((i) => [i.label, i.description ?? '', i.path ?? '']),
            signs,
        });
    }
}

const text = JSON.stringify(data) + '\n';
if (process.argv.includes('--check')) {
    if (!fs.existsSync(output) || fs.readFileSync(output, 'utf8') !== text) {
        console.error(`${path.relative(root, output)} is out of date; run node scripts/generate-fuzzy-cases.mjs`);
        process.exit(1);
    }
} else {
    fs.writeFileSync(output, text);
    console.log(`wrote ${path.relative(root, output)}: ${data.scoreFuzzy.length} scoreFuzzy, ${data.scoreItem.length} scoreItem, ${data.compare.length} compare groups (VS Code ${version})`);
}
