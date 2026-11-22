// Records the style that VS Code's TextMate tokenizer (vscode-textmate) gives to scope stacks under
// a set of theme rules, as tests/core/data/token_theme_cases.json. tests/core/token_theme_test.cpp
// replays the cases against core/token_theme.cpp, so the port stays faithful.
// A grammar that names each nested scope after the text between angle brackets makes a token with
// any scope stack, so the real tokenizer decides the style: "<a.b><c>@</></>" is the token @ in
// the scopes source.t, a.b and c, the text being the marker character @. The rules and the
// checkout's vscode-textmate decide the rest.
//   node scripts/generate-token-theme-cases.mjs [--check]
import fs from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const modules = path.join(root, '.deps', 'vscode', 'node_modules');
const output = path.join(root, 'tests/core/data/token_theme_cases.json');
const require = createRequire(import.meta.url);
const tm = require(path.join(modules, 'vscode-textmate/release/main.js'));
const onig = require(path.join(modules, 'vscode-oniguruma/release/main.js'));
const version = JSON.parse(fs.readFileSync(path.join(modules, 'vscode-textmate/package.json'), 'utf8')).version;
await onig.loadWASM(fs.readFileSync(path.join(modules, 'vscode-oniguruma/release/onig.wasm')).buffer);

const grammar = {
    scopeName: 'source.t',
    patterns: [{ include: '#nest' }],
    repository: { nest: { begin: '<([^>]*)>', end: '</>', contentName: '$1', patterns: [{ include: '#nest' }] } },
};

// JSON with comments and trailing commas, as the theme files are written.
function parseJsonc(text) {
    let out = '';
    for (let i = 0; i < text.length; i++) {
        const c = text[i];
        if (c === '"') {
            let j = i + 1;
            while (text[j] !== '"') {
                j += text[j] === '\\' ? 2 : 1;
            }
            out += text.slice(i, j + 1);
            i = j;
        } else if (c === '/' && text[i + 1] === '/') {
            while (i < text.length && text[i] !== '\n') {
                i++;
            }
            out += '\n';
        } else if (c === '/' && text[i + 1] === '*') {
            i = text.indexOf('*/', i + 2) + 1;
        } else {
            out += c;
        }
    }
    return JSON.parse(out.replace(/,(\s*[}\]])/g, '$1'));
}

const themes = path.join(root, '.deps', 'vscode', 'extensions', 'theme-defaults', 'themes');
function themeFile(name) {
    const file = parseJsonc(fs.readFileSync(path.join(themes, name), 'utf8'));
    const included = file.include ? themeFile(path.basename(file.include)) : { colors: {}, rules: [] };
    return { colors: { ...included.colors, ...(file.colors ?? {}) }, rules: [...included.rules, ...(file.tokenColors ?? [])] };
}
// What VS Code gives the tokenizer: a rule for the editor's colors, then the theme's own rules.
function realRules(name) {
    const theme = themeFile(name);
    const settings = {};
    if (theme.colors['editor.foreground']) settings.foreground = theme.colors['editor.foreground'];
    if (theme.colors['editor.background']) settings.background = theme.colors['editor.background'];
    return [{ settings }, ...theme.rules];
}

let seed = 0x1badc0de;
function random() {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
}
const pick = (list) => list[Math.floor(random() * list.length)];

const atoms = ['meta', 'meta.function', 'meta.function.call', 'entity', 'entity.name', 'entity.name.function',
    'entity.name.type', 'string', 'string.quoted', 'string.quoted.double', 'comment', 'comment.line', 'keyword',
    'keyword.control', 'keyword.operator', 'support', 'support.function', 'variable', 'variable.parameter',
    'punctuation', 'punctuation.definition', 'punctuation.definition.string', 'source.js', 'source.t',
    'text.html', 'markup.bold', 'constant.numeric', 'storage.type', 'invalid', 'a', 'ab', 'a.b'];
const colors = ['#ff0000', '#00FF00', '#0000ff80', '#abc', '#ABCD', '#123456', '#fedcba98', '#ffffff', '#000000'];
const fontStyles = ['', 'italic', 'bold', 'italic bold', 'underline', 'strikethrough', 'italic underline strikethrough', ' bold ', 'bold  italic', 'bogus'];

function randomSettings() {
    const settings = {};
    if (random() < 0.7) settings.foreground = pick(colors);
    if (random() < 0.25) settings.background = pick(colors);
    if (random() < 0.4) settings.fontStyle = pick(fontStyles);
    if (random() < 0.05) settings.foreground = pick(['red', '#12', '#12345', '#ggg']);
    return settings;
}
function randomSelector() {
    const parts = 1 + Math.floor(random() * 3);
    const words = [];
    for (let i = 0; i < parts; i++) {
        words.push(pick(atoms));
        if (i < parts - 1 && random() < 0.25) words.push('>');
    }
    return words.join(' ');
}
function randomRule() {
    const rule = {};
    const form = random();
    if (form < 0.55) rule.scope = randomSelector();
    else if (form < 0.7) rule.scope = `${randomSelector()}, ${randomSelector()}`;
    else if (form < 0.8) rule.scope = [randomSelector(), randomSelector()];
    else if (form < 0.85) rule.scope = `,${randomSelector()},`;
    // otherwise no scope: a default rule
    const settingsForm = random();
    if (settingsForm < 0.93) rule.settings = randomSettings();
    else if (settingsForm < 0.96) rule.settings = {};
    return rule;
}

const handBuilt = [
    ['alternatives and prefixes', [
        { settings: { foreground: '#111111', background: '#eeeeee' } },
        { scope: 'entity.name, string', settings: { foreground: '#aa0000' } },
        { scope: 'entity.name.function', settings: { fontStyle: 'bold' } },
        { scope: ['comment', 'keyword.control'], settings: { foreground: '#00aa00', fontStyle: 'italic' } },
        { scope: 'support', settings: { foreground: '#0000aa', fontStyle: '' } },
    ]],
    ['parents and specificity', [
        { settings: { foreground: '#111111' } },
        { scope: 'meta.function', settings: { foreground: '#aa0000' } },
        { scope: 'meta.function entity.name', settings: { foreground: '#00aa00' } },
        { scope: 'source.js entity.name', settings: { foreground: '#0000aa' } },
        { scope: 'meta entity', settings: { background: '#cccccc' } },
        { scope: 'entity.name.function', settings: { fontStyle: 'underline' } },
        { scope: 'source.t meta.function entity.name.function', settings: { fontStyle: 'bold' } },
    ]],
    ['child combinator', [
        { settings: { foreground: '#111111' } },
        { scope: 'a > b', settings: { foreground: '#aa0000' } },
        { scope: 'a b', settings: { foreground: '#00aa00' } },
        { scope: 'a > b > c', settings: { fontStyle: 'italic' } },
        { scope: 'source.t > a', settings: { background: '#dddddd' } },
    ]],
    ['ties are decided by order', [
        { scope: 'string', settings: { foreground: '#111111' } },
        { scope: 'string', settings: { foreground: '#222222' } },
        { scope: 'string.quoted', settings: { fontStyle: 'bold' } },
        { scope: 'string.quoted', settings: { fontStyle: 'italic' } },
        { scope: 'comment', settings: { foreground: '#333333' } },
        { scope: 'comment', settings: { background: '#444444' } },
    ]],
    ['whole words only', [
        { settings: { foreground: '#111111' } },
        { scope: 'entity.name', settings: { foreground: '#aa0000' } },
        { scope: 'meta.function.call', settings: { foreground: '#00aa00' } },
        { scope: 'a', settings: { fontStyle: 'bold' } },
    ]],
    ['no default rule', [
        { scope: 'string', settings: { foreground: '#aa0000' } },
        { scope: 'comment', settings: { fontStyle: 'italic' } },
    ]],
    ['several default rules', [
        { settings: { foreground: '#111111' } },
        { settings: { background: '#222222', fontStyle: 'bold' } },
        { scope: '', settings: { foreground: '#333333' } },
        { scope: 'string', settings: { foreground: '#aa0000' } },
    ]],
    ['odd entries', [
        { settings: { foreground: '#111111' } },
        { scope: 'string' },
        { scope: 'comment', settings: null },
        { scope: 'keyword', settings: 'bold' },
        { scope: ' meta  entity ', settings: { foreground: '#aa0000' } },
        { scope: 'support,,variable', settings: { foreground: '#00aa00' } },
        { scope: ',,markup,,', settings: { fontStyle: 'bold' } },
    ]],
];

const sets = [];
for (const [name, rules] of handBuilt) sets.push({ name, rules });
for (let i = 0; i < 14; i++) {
    const rules = [{ settings: { foreground: pick(colors), background: pick(colors) } }];
    const count = 5 + Math.floor(random() * 10);
    for (let r = 0; r < count; r++) rules.push(randomRule());
    sets.push({ name: `random ${i}`, rules });
}
for (const [name, file] of [['Dark+', 'dark_plus.json'], ['Light+', 'light_plus.json'], ['2026 Dark', '2026-dark.json'], ['Dark High Contrast', 'hc_black.json']]) {
    sets.push({ name, rules: realRules(file), real: true });
}

function scopesOf(rules) {
    const names = new Set(atoms);
    for (const rule of rules) {
        const scopes = Array.isArray(rule.scope) ? rule.scope : typeof rule.scope === 'string' ? rule.scope.split(',') : [];
        for (const selector of scopes) {
            for (const word of selector.trim().split(' ')) {
                if (word && word !== '>') names.add(word);
            }
        }
    }
    const list = [...names];
    for (const name of [...list]) {
        list.push(`${name}.x`, `${name}.js`);
    }
    list.push('unrelated', 'other.thing', 'source.js', 'meta.embedded');
    return list;
}

// Whether a rule without a scope, which is a default rule, sets the field to a valid color. The
// tokenizer makes the default foreground black and the background white when none does, and the
// port leaves them unset, so the tests need to know.
function setsDefault(rules, field) {
    const valid = (value) => typeof value === 'string' && /^#([0-9a-f]{3,4}|[0-9a-f]{6}|[0-9a-f]{8})$/i.test(value);
    return rules.some((rule) => {
        if (!rule.settings || typeof rule.settings !== 'object' || !valid(rule.settings[field])) return false;
        const selectors = typeof rule.scope === 'string' ? rule.scope.replace(/^,+/, '').replace(/,+$/, '').split(',')
            : Array.isArray(rule.scope) ? rule.scope : [''];
        return selectors.some((selector) => selector.trim() === '');
    });
}

const registry = new tm.Registry({
    onigLib: Promise.resolve({ createOnigScanner: (s) => new onig.OnigScanner(s), createOnigString: (s) => new onig.OnigString(s) }),
    loadGrammar: async (name) => (name === 'source.t' ? grammar : null),
});
const grammarInstance = await registry.loadGrammar('source.t');

const data = { vscodeTextmate: version, sets: [] };
for (const set of sets) {
    registry.setTheme({ settings: set.rules });
    const colorMap = registry.getColorMap();
    const vocabulary = scopesOf(set.rules);
    const cases = [];
    const seen = new Set();
    const count = set.real ? 250 : 90;
    for (let n = 0; n < count; n++) {
        const depth = n === 0 ? 0 : 1 + Math.floor(random() * 4);
        const stack = [];
        for (let d = 0; d < depth; d++) stack.push(pick(vocabulary));
        const key = stack.join(' ');
        if (seen.has(key)) continue;
        seen.add(key);
        const line = `${stack.map((s) => `<${s}>`).join('')}@${stack.map(() => '</>').join('')}`;
        const result = grammarInstance.tokenizeLine2(line);
        const at = line.indexOf('@');
        let metadata = 0;
        for (let i = 0; i < result.tokens.length; i += 2) {
            if (result.tokens[i] <= at) metadata = result.tokens[i + 1];
        }
        const fontStyle = (metadata & 0x00007800) >>> 11;
        const foreground = colorMap[(metadata & 0x00ff8000) >>> 15] ?? null;
        const background = colorMap[(metadata & 0xff000000) >>> 24] ?? null;
        cases.push([stack, fontStyle, foreground, background]);
    }
    data.sets.push({ name: set.name, rules: set.rules, defaultForeground: setsDefault(set.rules, 'foreground'), defaultBackground: setsDefault(set.rules, 'background'), cases });
}

const text = JSON.stringify(data) + '\n';
if (process.argv.includes('--check')) {
    if (!fs.existsSync(output) || fs.readFileSync(output, 'utf8') !== text) {
        console.error(`${path.relative(root, output)} is out of date; run node scripts/generate-token-theme-cases.mjs`);
        process.exit(1);
    }
} else {
    fs.writeFileSync(output, text);
    const total = data.sets.reduce((sum, s) => sum + s.cases.length, 0);
    console.log(`wrote ${path.relative(root, output)}: ${data.sets.length} rule sets, ${total} cases (vscode-textmate ${version}), ${text.length} bytes`);
}
