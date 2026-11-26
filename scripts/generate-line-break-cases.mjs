// Records where VS Code breaks lines into rows when it wraps them (its monospace line breaks
// computer, with the default break characters and `wrappingIndent: "same"`) as
// tests/core/data/line_break_cases.json. tests/core/line_breaks_test.cpp checks that Anvil breaks
// the same lines at the same places. Offsets are UTF-16 code units, where each row after the first
// begins.
//   node scripts/generate-line-break-cases.mjs [--check]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const vscodeDir = path.join(root, '.deps', 'vscode');
const output = path.join(root, 'tests/core/data/line_break_cases.json');

const load = (file) => import(path.join(vscodeDir, 'out/vs/editor/common', file));
const { MonospaceLineBreaksComputerFactory } = await load('viewModel/monospaceLineBreaksComputer.js');
const { EditorOptions, WrappingIndent } = await load('config/editorOptions.js');
const version = JSON.parse(fs.readFileSync(path.join(vscodeDir, 'package.json'), 'utf8')).version;

const before = EditorOptions.wordWrapBreakBeforeCharacters.defaultValue;
const after = EditorOptions.wordWrapBreakAfterCharacters.defaultValue;
const factory = new MonospaceLineBreaksComputerFactory(before, after);

const breaksOf = (line, tabSize, column, fullWidth) => {
    const context = { getLineContent: () => line, getLineInjectedText: () => null };
    const fontInfo = { typicalHalfwidthCharacterWidth: 1, typicalFullwidthCharacterWidth: fullWidth, fontSize: 1 };
    const computer = factory.createLineBreaksComputer(context, fontInfo, tabSize, column, WrappingIndent.Same, 'normal', false, false);
    computer.addRequest(1, null);
    const data = computer.finalize()[0];
    return data === null
        ? { starts: [], indent: 0 }
        : { starts: data.breakOffsets.slice(0, -1), indent: data.wrappedTextIndentLength };
};

const cases = [];
const add = (line, tabSize, column, fullWidth) => {
    cases.push({ line, tabSize, column, fullWidth, ...breaksOf(line, tabSize, column, fullWidth) });
};

// Every break character between two words, so that a missing one shows.
for (const character of new Set([...before, ...after])) {
    add(`aaaa${character}bbbbbb`, 4, 6, 2);
    add(`aaaaa${character}${character}bbbbbb`, 4, 8, 2);
}

for (const line of ['', 'a', '😀', 'ab', '  ', '\t\t\t\t\t\t', '    indented text that goes on and on and on',
    '\tconst value = compute(first, second); // a note', 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    '        ', '          x', 'これは日本語の長い文章です。改行の位置を確かめます。', '中文（括号）和「引号」的换行。',
    'https://example.com/a/very/long/path?with=query&and=more#anchor']) {
    for (const column of [1, 2, 3, 5, 8, 13, 20, 40]) {
        add(line, 4, column, 2);
        add(line, 8, column, 1.6666666666666667);
    }
}

const pieces = [
    'a', 'b', 'c', 'd', 'e', 'f', ' ', ' ', ' ', ' ', '\t', '.', ',', ';', ':', '(', ')', '[', ']', '{', '}',
    '-', '/', '\\', '|', '?', '&', '+', '"', '\'', '_', '=',
    '中', '文', 'あ', 'ア', 'ゃ', '㐀', '、', '。', '（', '）', '「', '」', '！', '￥', '£', '“', '”', '…', '′',
    '한', 'Ａ', 'é', 'ß', 'д', '😀', '𠀀', '\u0001', '\u007f', '́', ' ', '　',
];
let seed = 1;
const random = (n) => {
    seed = (seed * 48271) % 2147483647;
    return seed % n;
};
for (let i = 0; i < 1200; i++) {
    // A few pieces per line, so that runs of one kind are common.
    const alphabet = Array.from({ length: 2 + random(8) }, () => pieces[random(pieces.length)]);
    const indent = ['', '', '  ', '\t', '\t  ', '        '][random(6)];
    const line = indent + Array.from({ length: random(60) }, () => alphabet[random(alphabet.length)]).join('');
    add(line, [1, 2, 4, 8][random(4)], [1, 2, 3, 4, 6, 8, 11, 16, 24][random(9)], [1, 1.6666666666666667, 2, 2][random(4)]);
}

const text = `{\n "vscode": ${JSON.stringify(version)},\n "cases": [\n${cases.map((c) => '  ' + JSON.stringify(c)).join(',\n')}\n ]\n}\n`;
if (process.argv.includes('--check')) {
    if (fs.readFileSync(output, 'utf8') !== text) {
        console.error(`${output} is out of date`);
        process.exit(1);
    }
} else {
    fs.writeFileSync(output, text);
}
