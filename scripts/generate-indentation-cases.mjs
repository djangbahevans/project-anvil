// Records what VS Code guesses about the indentation of a text, as
// tests/core/data/indentation_cases.json: its own `guessIndentation` on a corpus of texts that are
// indented with spaces, tabs or both, with aligned continuation lines among them.
// tests/core/indentation_test.cpp replays the cases against core/indentation.cpp.
//   node scripts/generate-indentation-cases.mjs [--check]
import path from 'node:path';
import { corpus, load, record, root } from './vscode-editor.mjs';

const output = path.join(root, 'tests/core/data/indentation_cases.json');
const { guessIndentation } = await load('editor/common/model/indentationGuesser.js');
const { between, chance, pick } = corpus(20261128);

const bodies = [
    'foo()',
    'bar();',
    'let x = 1,',
    'const a = b + c,',
    'd = e;',
    '- item',
    'key: value',
    '}',
    'é, naïve,',
    '😀😀 x,',
    'héllo wörld',
    'x',
    ',',
];

function text() {
    const unit = pick([2, 2, 3, 4, 4, 4, 5, 6, 7, 8, 9]);
    const style = pick(['spaces', 'spaces', 'spaces', 'tabs', 'tabs', 'mixed', 'random']);
    const indentation = (level) => {
        if (style === 'tabs') return '\t'.repeat(level);
        if (style === 'mixed') return chance(0.5) ? '\t'.repeat(level) : ' '.repeat(level * unit);
        if (style === 'random') return pick(['', ' ', '  ', '\t ', ' \t', '    ', '\t\t  ']);
        return ' '.repeat(level * unit);
    };
    let level = 0;
    const lines = [];
    for (let n = between(1, 14); n > 0; n--) {
        level = Math.max(0, level + pick([-2, -1, 0, 0, 1, 1]));
        const previous = lines.at(-1) ?? '';
        if (chance(0.15) && previous.endsWith(',')) {
            // Aligned under a word of the line above.
            const spaces = previous.indexOf(' ', between(0, previous.length - 1));
            lines.push(' '.repeat(spaces < 0 ? between(1, 8) : spaces + 1) + pick(bodies));
        } else if (chance(0.1)) {
            lines.push(pick(['', '', ' ', '    ', '\t']));
        } else {
            lines.push(indentation(level) + pick(bodies) + (chance(0.1) ? ' ' : ''));
        }
    }
    return lines;
}

const buffer = (lines) => ({
    getLineCount: () => lines.length,
    getLineLength: (line) => lines[line - 1].length,
    getLineContent: (line) => lines[line - 1],
    getLineCharCode: (line, index) => lines[line - 1].charCodeAt(index),
});

const cases = [];
const add = (lines, tabSize, insertSpaces) => {
    const guess = guessIndentation(buffer(lines), tabSize, insertSpaces);
    cases.push({
        lines,
        tabSize,
        insertSpaces,
        expectedTabSize: guess.tabSize,
        expectedInsertSpaces: guess.insertSpaces,
    });
};
// An aligned line counts when it is indented by the default tab size and spaces are the default.
for (const insertSpaces of [true, false]) {
    for (const tabSize of [2, 4, 6]) {
        add(['- item,', '  - nested,', '- item,', '  - nested,'], tabSize, insertSpaces);
        add(['é x,', '    y', 'a b,', '  c'], tabSize, insertSpaces);
    }
}
for (let n = 0; n < 2000; n++) {
    add(text(), pick([2, 4, 4, 8]), chance(0.6));
}

record('generate-indentation-cases.mjs', output, {}, cases);
