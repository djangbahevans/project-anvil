// Records what VS Code's own editing commands do, as tests/core/data/edit_cases.json: Tab, Indent
// Line, Outdent Line (which Shift+Tab runs too), Delete Word Left and Delete Word Right, on a
// corpus of texts and selections in a language without a configuration of its own.
// tests/core/edit_commands_test.cpp replays the cases against core/indent_commands.cpp and
// core/word_delete.cpp.
//   node scripts/generate-edit-cases.mjs [--check]
import path from 'node:path';
import { corpus, load, record, root } from './vscode-editor.mjs';

const output = path.join(root, 'tests/core/data/edit_cases.json');

const { DisposableStore } = await load('base/common/lifecycle.js');
const { Selection } = await load('editor/common/core/selection.js');
const { createCodeEditorServices, instantiateTestCodeEditor } = await load('editor/test/browser/testCodeEditor.js');
const { instantiateTextModel } = await load('editor/test/common/testTextModel.js');
const { ILanguageService } = await load('editor/common/languages/language.js');
const { CoreEditingCommands } = await load('editor/browser/coreCommands.js');
const { TypeOperations } = await load('editor/common/cursor/cursorTypeOperations.js');
const { DeleteWordLeft, DeleteWordRight } = await load('editor/contrib/wordOperations/browser/wordOperations.js');

const { between, chance, pick, selectionsFor } = corpus(20261127);

const indents = ['', '', '', ' ', '  ', '   ', '    ', '      ', '        ', '\t', '\t\t', '\t  ', '  \t', ' \t '];
const bodies = [
    'foo()',
    'bar baz',
    'let x = 1;',
    'a.b->c::d',
    'snake_case camelCase',
    '== != <=',
    'x  y   z',
    'end.  ',
    'é naïve café',
    '😀 emoji 😀',
    '中文 text',
    '"quoted string"',
    '((()))',
    'a-b_c',
    'one\ttab',
    'word',
    '.',
    '',
    '',
    '',
];
const line = () => pick(indents) + pick(bodies) + (chance(0.1) ? pick([' ', '  ', '\t']) : '');

async function run(command, options, lines, selections) {
    const disposables = new DisposableStore();
    try {
        const services = createCodeEditorServices(disposables);
        const languageId = 'plainCases';
        services.invokeFunction((accessor) => {
            disposables.add(accessor.get(ILanguageService).registerLanguage({ id: languageId }));
        });
        const model = disposables.add(
            instantiateTextModel(services, lines.join('\n'), languageId, {
                tabSize: options.tabSize,
                indentSize: options.tabSize,
                insertSpaces: options.insertSpaces,
                detectIndentation: false,
            }),
        );
        const editor = disposables.add(instantiateTestCodeEditor(services, model));
        const viewModel = editor.getViewModel();
        viewModel.setSelections(
            'cases',
            selections.map(([al, ac, pl, pc]) => new Selection(al, ac, pl, pc)),
        );
        const toArray = (s) => [s.selectionStartLineNumber, s.selectionStartColumn, s.positionLineNumber, s.positionColumn];
        const current = viewModel.getSelections().map(toArray);
        if (command === 'tab') {
            CoreEditingCommands.Tab.runCoreEditingCommand(editor, viewModel, null);
        } else if (command === 'outdent') {
            CoreEditingCommands.Outdent.runCoreEditingCommand(editor, viewModel, null);
        } else if (command === 'indent') {
            editor.executeCommands('cases', TypeOperations.indent(viewModel.cursorConfig, model, editor.getSelections()));
        } else {
            const action = command === 'deleteWordLeft' ? new DeleteWordLeft() : new DeleteWordRight();
            services.invokeFunction((accessor) => action.runEditorCommand(accessor, editor, null));
        }
        return {
            selections: current,
            expectedLines: model.getLinesContent(),
            expectedSelections: viewModel.getSelections().map(toArray),
        };
    } finally {
        disposables.dispose();
    }
}

const cases = [];
const commands = ['tab', 'tab', 'indent', 'outdent', 'deleteWordLeft', 'deleteWordLeft', 'deleteWordRight', 'deleteWordRight'];
for (let n = 0; n < 3000; n++) {
    const command = pick(commands);
    const options = { tabSize: pick([2, 4, 4, 8]), insertSpaces: chance(0.7) };
    const lines = Array.from({ length: between(1, 4) }, line);
    // The primary cursor is not always the first in the text.
    const selections = selectionsFor(lines);
    if (selections.length > 1 && chance(0.4)) {
        selections.unshift(...selections.splice(between(1, selections.length - 1), 1));
    }
    cases.push({ command, ...options, lines, ...(await run(command, options, lines, selections)) });
}

record('generate-edit-cases.mjs', output, {}, cases);
