// Records what VS Code's own comment commands do, as tests/core/data/comment_cases.json:
// `LineCommentCommand` (Toggle Line Comment, Add Line Comment, Remove Line Comment) and
// `BlockCommentCommand` (Toggle Block Comment) run through the editor's cursor machinery on a
// corpus of texts and selections, with the same handling of several selections as the editor
// actions. tests/core/comment_commands_test.cpp replays the cases against core/comment_commands.cpp.
//   node scripts/generate-comment-cases.mjs [--check]
import fs from 'node:fs';
import { register } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const vscodeDir = path.join(root, '.deps', 'vscode');
const output = path.join(root, 'tests/core/data/comment_cases.json');

// The compiled editor loads css and wants a DOM; the cursor machinery needs neither.
register(
    'data:text/javascript,' +
        encodeURIComponent(`
export async function load(url, context, next) {
    if (url.endsWith('.css')) return { format: 'module', source: 'export default {};', shortCircuit: true };
    return next(url, context);
}`),
);
const element = () =>
    new Proxy(
        { style: {}, dataset: {}, children: [], classList: { add() {}, remove() {}, toggle() {}, contains: () => false } },
        {
            get: (target, key) => (key in target ? target[key] : typeof key === 'symbol' ? undefined : () => undefined),
            set: (target, key, value) => {
                target[key] = value;
                return true;
            },
        },
    );
globalThis.window = globalThis;
globalThis.document = {
    createElement: element,
    createElementNS: element,
    createDocumentFragment: element,
    addEventListener() {},
    head: element(),
    body: element(),
    documentElement: element(),
    location: { href: '' },
    querySelector: () => null,
    querySelectorAll: () => [],
    queryCommandSupported: () => true,
};
globalThis.addEventListener ??= () => {};
globalThis.matchMedia = () => ({ matches: false, addEventListener() {}, addListener() {} });
globalThis.location = { href: 'file:///', hash: '', search: '', protocol: 'file:', host: '', hostname: '', pathname: '/', origin: 'file://' };
globalThis.customElements = { get() {}, define() {}, whenDefined: () => Promise.resolve() };
for (const name of ['HTMLElement', 'MouseEvent', 'UIEvent']) {
    globalThis[name] ??= class {};
}

const load = (file) => import(path.join(vscodeDir, 'out/vs', file));
const { DisposableStore } = await load('base/common/lifecycle.js');
const { Range } = await load('editor/common/core/range.js');
const { Selection } = await load('editor/common/core/selection.js');
const { createCodeEditorServices, instantiateTestCodeEditor } = await load('editor/test/browser/testCodeEditor.js');
const { instantiateTextModel } = await load('editor/test/common/testTextModel.js');
const { ILanguageConfigurationService } = await load('editor/common/languages/languageConfigurationRegistry.js');
const { ILanguageService } = await load('editor/common/languages/language.js');
const { LineCommentCommand } = await load('editor/contrib/comment/browser/lineCommentCommand.js');
const { BlockCommentCommand } = await load('editor/contrib/comment/browser/blockCommentCommand.js');

const version = JSON.parse(fs.readFileSync(path.join(vscodeDir, 'package.json'), 'utf8')).version;

// The comment configurations of the corpus. The names are the ones the test knows.
const configurations = {
    slash: { lineComment: '//', blockComment: ['/*', '*/'] },
    hash_no_indent: { lineComment: { comment: '#', noIndent: true } },
    html: { blockComment: ['<!--', '-->'] },
    rem: { lineComment: 'rem' },
    python: { lineComment: '#', blockComment: ['"""', '"""'] },
    none: {},
};
const types = { toggle: 0, add: 1, remove: 2 };

// A small seeded generator, so that the file is the same on every run.
function random(seed) {
    let state = seed >>> 0;
    return () => {
        state = (state + 0x6d2b79f5) >>> 0;
        let t = state;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}
const next = random(20260101);
const pick = (list) => list[Math.floor(next() * list.length)];
const chance = (p) => next() < p;
const between = (low, high) => low + Math.floor(next() * (high - low + 1));

const indents = ['', '', '  ', '    ', '\t', '\t\t', '\t  ', '  \t', '        '];
const bodies = [
    'foo()',
    'bar baz',
    'let x = 1;',
    'é text',
    '😀 emoji',
    'x // y',
    'a /* b */ c',
    '/* whole */',
    '// commented',
    '//tight',
    '// two  spaces',
    '# hash',
    '#tight',
    'REM upper',
    'rem lower',
    'rem',
    '<!-- html -->',
    '<!--x-->',
    '"""doc"""',
    '/*',
    '*/',
    '',
    '',
    '   ',
];
const line = () => pick(indents) + pick(bodies) + (chance(0.1) ? ' ' : '');

// The columns (1-based, in UTF-16 units) where a selection can start or end: not inside a
// surrogate pair.
function columns(text) {
    const result = [];
    for (let i = 0; i <= text.length; i++) {
        const code = text.charCodeAt(i);
        if (code >= 0xdc00 && code <= 0xdfff && i > 0) {
            continue;
        }
        result.push(i + 1);
    }
    return result;
}

// Selections in order of position, with at least one position between neighbors so that the editor
// does not merge them, as [anchor line, anchor column, active line, active column].
function selectionsFor(lines) {
    const positions = [];
    lines.forEach((text, index) => {
        for (const column of columns(text)) {
            positions.push([index + 1, column]);
        }
    });
    const count = Math.min(between(1, 3), Math.floor(positions.length / 3));
    const chosen = new Set();
    while (chosen.size < Math.max(1, count) * 2 && chosen.size < positions.length) {
        chosen.add(between(0, positions.length - 1));
    }
    const ordered = [...chosen].sort((a, b) => a - b);
    const selections = [];
    for (let i = 0; i + 1 < ordered.length || (i < ordered.length && selections.length === 0); i += 2) {
        const start = positions[ordered[i]];
        const end = positions[ordered[i + 1] ?? ordered[i]];
        const caret = chance(0.35);
        const [from, to] = caret ? [start, start] : [start, end];
        selections.push(chance(0.25) ? [to[0], to[1], from[0], from[1]] : [from[0], from[1], to[0], to[1]]);
    }
    return selections;
}

async function run(config, command, options, lines, selections) {
    const disposables = new DisposableStore();
    try {
        const services = createCodeEditorServices(disposables);
        const languageId = 'commentMode';
        services.invokeFunction((accessor) => {
            disposables.add(accessor.get(ILanguageService).registerLanguage({ id: languageId }));
            disposables.add(
                accessor.get(ILanguageConfigurationService).register(languageId, { comments: configurations[config] }),
            );
        });
        const model = disposables.add(instantiateTextModel(services, lines.join('\n'), languageId));
        const editor = disposables.add(instantiateTestCodeEditor(services, model));
        const viewModel = editor.getViewModel();
        viewModel.setSelections(
            'cases',
            selections.map(([al, ac, pl, pc]) => new Selection(al, ac, pl, pc)),
        );
        const current = viewModel.getSelections();
        const commands = [];
        if (command === 'block') {
            for (const selection of current) {
                commands.push(new BlockCommentCommand(selection, options.insertSpace, services.invokeFunction((a) => a.get(ILanguageConfigurationService))));
            }
        } else {
            // What `CommentLineAction.run` does with several selections.
            const entries = current.map((selection, index) => ({ selection, index, ignoreFirstLine: false }));
            entries.sort((a, b) => Range.compareRangesUsingStarts(a.selection, b.selection));
            let previous = entries[0];
            for (let i = 1; i < entries.length; i++) {
                const here = entries[i];
                if (previous.selection.endLineNumber === here.selection.startLineNumber) {
                    if (previous.index < here.index) {
                        here.ignoreFirstLine = true;
                    } else {
                        previous.ignoreFirstLine = true;
                        previous = here;
                    }
                }
            }
            for (const entry of entries) {
                commands.push(
                    new LineCommentCommand(
                        services.invokeFunction((a) => a.get(ILanguageConfigurationService)),
                        entry.selection,
                        options.indentSize,
                        types[command],
                        options.insertSpace,
                        options.ignoreEmptyLines,
                        entry.ignoreFirstLine,
                    ),
                );
            }
        }
        viewModel.executeCommands(commands, 'cases');
        const toArray = (s) => [s.selectionStartLineNumber, s.selectionStartColumn, s.positionLineNumber, s.positionColumn];
        return {
            selections: current.map(toArray),
            expectedLines: model.getLinesContent(),
            expectedSelections: viewModel.getSelections().map(toArray),
        };
    } finally {
        disposables.dispose();
    }
}

const cases = [];
const names = Object.keys(configurations);
const commands = ['toggle', 'toggle', 'add', 'remove', 'block'];
// The first cases of the stream, and the cases of it that need a rule that the others rarely meet:
// a reversed selection that the edits empty, a selection pushed past the end of its line, cursors
// that merge, a block comment over a blank line.
const kept = 3000;
const rare = new Set([4115, 7198, 9132, 10972, 11077, 11528]);
for (let n = 0; n < 12000; n++) {
    const config = pick(names);
    const command = pick(commands);
    const options = {
        insertSpace: chance(0.75),
        ignoreEmptyLines: chance(0.75),
        indentSize: pick([2, 4, 4, 8]),
    };
    const lines = Array.from({ length: between(1, 5) }, line);
    // Whole-line comments are the common case: sometimes start from a commented text.
    if (command !== 'add' && chance(0.4)) {
        const token = configurations[config].lineComment;
        const text = typeof token === 'string' ? token : token?.comment;
        if (text) {
            for (let i = 0; i < lines.length; i++) {
                if (chance(0.7) && lines[i].trim() !== '') {
                    const body = lines[i].replace(/^\s*/, '');
                    const indent = lines[i].slice(0, lines[i].length - body.length);
                    lines[i] = (typeof token === 'string' ? indent : '') + text + (options.insertSpace ? ' ' : '') + body;
                }
            }
        }
    }
    // The primary cursor, which VS Code lists first, decides which of two cursors that share a line
    // comments it.
    const selections = selectionsFor(lines);
    if (selections.length > 1 && chance(0.4)) {
        selections.unshift(...selections.splice(between(1, selections.length - 1), 1));
    }
    if (n < kept || rare.has(n)) {
        const result = await run(config, command, options, lines, selections);
        cases.push({ config, command, ...options, lines, ...result });
    }
}

const generated = JSON.stringify({ vscode: version, configurations, cases }, null, 0)
    .replace(/\{"vscode"/, '{\n"vscode"')
    .replace(/,"configurations":/, ',\n"configurations":')
    .replace(/,"cases":\[/, ',\n"cases":[\n')
    .replace(/\},\{"config"/g, '},\n{"config"')
    .concat('\n');

if (process.argv.includes('--check')) {
    const current = fs.existsSync(output) ? fs.readFileSync(output, 'utf8') : '';
    if (current !== generated) {
        console.error(`${path.relative(root, output)} is out of date; run node scripts/generate-comment-cases.mjs`);
        process.exit(1);
    }
    console.log(`comment cases up to date (${cases.length} cases, VS Code ${version})`);
} else {
    fs.writeFileSync(output, generated);
    console.log(`wrote ${path.relative(root, output)} (${cases.length} cases, VS Code ${version})`);
}
