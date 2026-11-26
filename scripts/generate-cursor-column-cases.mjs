// Records how VS Code counts visible columns (`CursorColumns`) and moves a caret up and down
// (`MoveOperations.vertical`) as tests/core/data/cursor_column_cases.json.
// tests/core/visible_column_test.cpp checks that Anvil does the same. Columns are zero-based
// UTF-16 code units, and leftover columns are what VS Code keeps for the next move.
//   node scripts/generate-cursor-column-cases.mjs [--check]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const vscodeDir = path.join(root, '.deps', 'vscode');
const output = path.join(root, 'tests/core/data/cursor_column_cases.json');

const { CursorColumns } = await import(path.join(vscodeDir, 'out/vs/editor/common/core/cursorColumns.js'));
const { MoveOperations } = await import(path.join(vscodeDir, 'out/vs/editor/common/cursor/cursorMoveOperations.js'));
const version = JSON.parse(fs.readFileSync(path.join(vscodeDir, 'package.json'), 'utf8')).version;

const pieces = [
    'a', 'b', ' ', ' ', '\t', '\t', '.',
    'é', 'é', '́', // precomposed, combining, a lone combining mark
    '中', '한', 'ア', '、', 'Ａ', '￥', 'ｱ', // wide, and a half-width katakana
    '가', // decomposed Hangul
    '⺀', '⹿', '힯', '豈', '～', '￦', // the ends of the wide ranges
    '😀', '👩‍🚀', '👍🏽', '🇬🇭', '☂', '☂️', '⭐', '𠀀', '𝑥',
    'नि', 'กำ', '‍',
];

let seed = 1;
const random = (n) => {
    seed = (seed * 48271) % 2147483647;
    return seed % n;
};
const randomLine = (length) => Array.from({ length }, () => pieces[random(pieces.length)]).join('');

const boundaries = (line) => {
    const result = [0];
    for (const character of line) {
        result.push(result.at(-1) + character.length);
    }
    return result;
};

const columnCases = [];
const addLine = (line, tabSize) => {
    const columns = boundaries(line);
    const visible = columns.map((column) => CursorColumns.visibleColumnFromColumn(line, column + 1, tabSize));
    const last = CursorColumns.visibleColumnFromColumn(line, line.length + 1, tabSize);
    const targets = [];
    for (let target = 0; target <= last + 2; target++) {
        targets.push(CursorColumns.columnFromVisibleColumn(line, target, tabSize) - 1);
    }
    columnCases.push({ line, tabSize, columns, visible, targets });
};
for (const tabSize of [1, 2, 4, 8]) {
    addLine('', tabSize);
    addLine('\tab\t\tc', tabSize);
    addLine('a\t中\t😀\té\tz', tabSize);
}
for (const piece of pieces) {
    addLine(`x${piece}y${piece}${piece}\tz`, 4);
}
for (let i = 0; i < 300; i++) {
    addLine(randomLine(random(12)), [1, 2, 3, 4, 8][random(5)]);
}

const moveCases = [];
for (let i = 0; i < 400; i++) {
    const tabSize = [2, 4, 8][random(3)];
    const lines = Array.from({ length: 1 + random(5) }, () => randomLine(random(10)));
    const model = {
        getLineCount: () => lines.length,
        getLineContent: (n) => lines[n - 1],
        getLineMinColumn: () => 1,
        getLineMaxColumn: (n) => lines[n - 1].length + 1,
        normalizePosition: (position) => position,
    };
    const config = {
        tabSize,
        columnFromVisibleColumn: (_, n, visible) => CursorColumns.columnFromVisibleColumn(lines[n - 1], visible, tabSize),
    };
    // The arrow keys move on at the first and last line; adding a cursor does not.
    const arrows = i % 2 === 0;
    let line = random(lines.length);
    const starts = boundaries(lines[line]);
    let column = starts[random(starts.length)];
    let leftover = 0;
    const start = [line, column];
    const steps = [];
    const direction = random(2) === 0 ? 1 : -1;
    for (let step = 0; step < 8; step++) {
        const down = arrows ? random(2) === 0 : direction > 0;
        const move = down ? MoveOperations.down : MoveOperations.up;
        const to = move.call(MoveOperations, config, model, line + 1, column + 1, leftover, 1, arrows);
        line = to.lineNumber - 1;
        column = to.column - 1;
        leftover = to.leftoverVisibleColumns;
        steps.push([down ? 1 : 0, line, column]);
    }
    moveCases.push({ lines, tabSize, arrows, start, steps });
}

const rows = (cases) => '[\n' + cases.map((c) => '  ' + JSON.stringify(c)).join(',\n') + '\n ]';
const text = `{\n "vscode": ${JSON.stringify(version)},\n "columns": ${rows(columnCases)},\n "moves": ${rows(moveCases)}\n}\n`;
if (process.argv.includes('--check')) {
    if (fs.readFileSync(output, 'utf8') !== text) {
        console.error(`${output} is out of date`);
        process.exit(1);
    }
} else {
    fs.writeFileSync(output, text);
}
