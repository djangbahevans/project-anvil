// What the scripts that record the behavior of VS Code's editor commands share: loading the
// compiled editor of the pinned checkout into Node, and making a corpus that is the same on every
// run.
import fs from 'node:fs';
import { register } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
export const vscodeDir = path.join(root, '.deps', 'vscode');
export const version = JSON.parse(fs.readFileSync(path.join(vscodeDir, 'package.json'), 'utf8')).version;

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

export const load = (file) => import(path.join(vscodeDir, 'out/vs', file));

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

export function corpus(seed) {
    const next = random(seed);
    const between = (low, high) => low + Math.floor(next() * (high - low + 1));
    const chance = (p) => next() < p;
    const pick = (list) => list[Math.floor(next() * list.length)];

    // Selections in order of position, with at least one position between neighbors so that the
    // editor does not merge them, as [anchor line, anchor column, active line, active column].
    const selectionsFor = (lines) => {
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
    };
    return { between, chance, pick, selectionsFor };
}

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

// Writes the cases, one to a line, or with --check fails when the file does not hold them.
export function record(script, output, fields, cases) {
    const header = Object.entries({ vscode: version, ...fields }).map(
        ([key, value]) => `${JSON.stringify(key)}:${JSON.stringify(value)},`,
    );
    const generated = ['{', ...header, '"cases":[', cases.map((c) => JSON.stringify(c)).join(',\n'), ']}', ''].join('\n');
    const name = path.relative(root, output);
    if (process.argv.includes('--check')) {
        const current = fs.existsSync(output) ? fs.readFileSync(output, 'utf8') : '';
        if (current !== generated) {
            console.error(`${name} is out of date; run node scripts/${script}`);
            process.exit(1);
        }
        console.log(`${name} is up to date (${cases.length} cases, VS Code ${version})`);
    } else {
        fs.writeFileSync(output, generated);
        console.log(`wrote ${name} (${cases.length} cases, VS Code ${version})`);
    }
}
