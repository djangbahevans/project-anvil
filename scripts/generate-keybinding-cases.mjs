// Records what VS Code's own `KeybindingResolver` does with random tables of key bindings, as
// tests/core/data/keybinding_cases.json. tests/core/keybindings_test.cpp replays the cases against
// core/keybindings.cpp, so the port stays faithful. The resolver is loaded from the pinned VS Code
// checkout, so the file changes when that is upgraded.
//   node scripts/generate-keybinding-cases.mjs [--check]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const vscodeDir = path.join(root, '.deps', 'vscode');
const output = path.join(root, 'tests/core/data/keybinding_cases.json');

const load = (file) => import(path.join(vscodeDir, 'out/vs', file));
const { KeybindingResolver, ResultKind } = await load('platform/keybinding/common/keybindingResolver.js');
const { ResolvedKeybindingItem } = await load('platform/keybinding/common/resolvedKeybindingItem.js');
const { USLayoutResolvedKeybinding } = await load('platform/keybinding/common/usLayoutResolvedKeybinding.js');
const { KeybindingParser } = await load('base/common/keybindingParser.js');
const { ContextKeyExpr } = await load('platform/contextkey/common/contextkey.js');
const { OperatingSystem } = await load('base/common/platform.js');
const version = JSON.parse(fs.readFileSync(path.join(vscodeDir, 'package.json'), 'utf8')).version;

// A small, deterministic random source, so that the file only changes with VS Code.
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
const rand = random(20260101);
const pick = (list) => list[Math.floor(rand() * list.length)];
const chance = (p) => rand() < p;

const presses = ['a', 'b', 'ctrl+k', 'ctrl+c', 'ctrl+u', 'shift+a'];
const commands = ['c0', 'c1', 'c2', 'c3', 'c4'];
const whens = [undefined, undefined, 'a', 'b', 'a && b', '!a', 'a || b', 'a && !b', 'c', 'false', 'a && b && c', '!b'];
const keys = ['a', 'b', 'c'];

function chordOf(length) {
    return Array.from({ length }, () => pick(presses)).join(' ');
}

function randomTable() {
    const bindings = [];
    const count = 3 + Math.floor(rand() * 9);
    for (let i = 0; i < count; ++i) {
        const user = chance(0.35);
        const kind = rand();
        const binding = { source: user ? 'user' : chance(0.25) ? 'extension' : 'builtin' };
        if (kind < 0.15) {
            // A removal: of a command, with or without a key, with or without a `when`.
            binding.command = `-${pick(commands)}`;
            if (chance(0.7)) {
                binding.key = chordOf(1 + Math.floor(rand() * 2));
            }
        } else {
            binding.command = (chance(0.1) ? '^' : '') + pick(commands);
            binding.key = chordOf(1 + Math.floor(rand() * 3));
        }
        const when = pick(whens);
        if (when !== undefined) {
            binding.when = when;
        }
        bindings.push(binding);
    }
    return bindings;
}

function item(binding) {
    const parsed = binding.key === undefined ? null : KeybindingParser.parseKeybinding(binding.key);
    const resolved = parsed ? new USLayoutResolvedKeybinding(parsed.chords, OperatingSystem.Linux) : undefined;
    const when = binding.when === undefined ? undefined : ContextKeyExpr.deserialize(binding.when);
    return new ResolvedKeybindingItem(resolved, binding.command, undefined, when, binding.source !== 'user', null, false);
}

function dispatchString(text) {
    const parsed = KeybindingParser.parseKeybinding(text);
    return new USLayoutResolvedKeybinding(parsed.chords, OperatingSystem.Linux).getDispatchChords();
}

const kinds = { [ResultKind.NoMatchingKb]: 'none', [ResultKind.MoreChordsNeeded]: 'more', [ResultKind.KbFound]: 'found' };

const tables = [];
for (let t = 0; t < 200; ++t) {
    const bindings = randomTable();
    // VS Code's own order is the defaults (the built-in ones, then the extensions') and then the user's.
    const order = { builtin: 0, extension: 1, user: 2 };
    const ordered = bindings.map((b, i) => ({ b, i })).sort((x, y) => order[x.b.source] - order[y.b.source] || x.i - y.i).map((x) => x.b);
    const defaults = ordered.filter((b) => b.source !== 'user').map(item);
    const overrides = ordered.filter((b) => b.source === 'user').map(item);
    const resolver = new KeybindingResolver(defaults, overrides, () => {});

    const queries = [];
    const withKeys = bindings.filter((b) => b.key !== undefined);
    for (let q = 0; q < 16; ++q) {
        const context = Object.fromEntries(keys.map((k) => [k, chance(0.5)]));
        // Most queries press what a binding says, or the start of it, so that bindings are found.
        let sequence;
        if (withKeys.length > 0 && chance(0.7)) {
            const steps = pick(withKeys).key.split(' ');
            sequence = steps.slice(0, 1 + Math.floor(rand() * steps.length));
            if (chance(0.15)) {
                sequence[sequence.length - 1] = pick(presses);
            }
        } else {
            sequence = Array.from({ length: 1 + Math.floor(rand() * 3) }, () => pick(presses));
        }
        const dispatched = sequence.map((press) => dispatchString(press)[0]);
        const result = resolver.resolve({ getValue: (key) => context[key] }, dispatched.slice(0, -1), dispatched[dispatched.length - 1]);
        queries.push({
            context,
            presses: sequence,
            kind: kinds[result.kind],
            command: result.kind === ResultKind.KbFound ? result.commandId : undefined,
            bubble: result.kind === ResultKind.KbFound ? result.isBubble : undefined,
        });
    }

    const lookups = commands.map((command) => ({
        command,
        keys: resolver.lookupKeybindings(command).map((found) => found.chords.join(' ').toLowerCase()),
    }));
    const primaries = [];
    for (const command of commands) {
        for (let q = 0; q < 2; ++q) {
            const context = Object.fromEntries(keys.map((k) => [k, chance(0.5)]));
            const service = { contextMatchesRules: (rules) => (rules ? rules.evaluate({ getValue: (key) => context[key] }) : true) };
            for (const enforce of [false, true]) {
                const found = resolver.lookupPrimaryKeybinding(command, service, enforce);
                primaries.push({ command, context, enforce, key: found ? found.chords.join(' ').toLowerCase() : null, when: found?.when ? found.when.serialize() : null });
            }
        }
    }
    tables.push({ bindings, queries, lookups, primaries });
}

// Pairs of `when` expressions and whether VS Code proves that the first implies the second.
const terms = ['a', 'b', 'c', '!a', '!b', 'a == 1', 'a == 2', 'a =~ /x/'];
const expressions = new Set([...terms]);
for (const x of terms) {
    for (const y of terms) {
        expressions.add(`${x} && ${y}`);
        expressions.add(`${x} || ${y}`);
    }
}
for (const x of terms.slice(0, 4)) {
    expressions.add(`${x} && a && b`);
    expressions.add(`(${x} || b) && c`);
}
const list = [...expressions];
const implications = [];
for (let n = 0; n < 1500; ++n) {
    const a = pick(list);
    const b = pick(list);
    const included = KeybindingResolver.whenIsEntirelyIncluded(ContextKeyExpr.deserialize(a), ContextKeyExpr.deserialize(b));
    implications.push({ a, b, implies: included });
}

const generated = JSON.stringify({ vscode: version, tables, implications }) + '\n';
if (process.argv.includes('--check')) {
    const current = fs.existsSync(output) ? fs.readFileSync(output, 'utf8') : '';
    if (current !== generated) {
        console.error(`${path.relative(root, output)} is out of date; run node scripts/generate-keybinding-cases.mjs`);
        process.exit(1);
    }
    console.log(`keybinding cases up to date (${tables.length} tables, VS Code ${version})`);
} else {
    fs.writeFileSync(output, generated);
    const found = tables.flatMap((t) => t.queries).filter((q) => q.kind === 'found').length;
    const more = tables.flatMap((t) => t.queries).filter((q) => q.kind === 'more').length;
    console.log(`keybinding cases: ${tables.length} tables, ${tables.length * 16} queries (${found} found, ${more} more keys needed), ${implications.length} implications (VS Code ${version})`);
}
