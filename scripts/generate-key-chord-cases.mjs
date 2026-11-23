// Records what VS Code's own parser and label providers do with a corpus of key binding strings, as
// tests/core/data/key_chord_cases.json. tests/core/key_chord_test.cpp replays the cases against
// core/key_chord.cpp, so the port stays faithful. The parser is loaded from the pinned VS Code
// checkout, so the file changes when that is upgraded.
//   node scripts/generate-key-chord-cases.mjs [--check]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const vscodeDir = path.join(root, '.deps', 'vscode');
const output = path.join(root, 'tests/core/data/key_chord_cases.json');

const out = (file) => import(path.join(vscodeDir, 'out/vs', file));
const { KeyCode, KeyCodeUtils } = await out('base/common/keyCodes.js');
const { KeybindingParser } = await out('base/common/keybindingParser.js');
const { USLayoutResolvedKeybinding } = await out('platform/keybinding/common/usLayoutResolvedKeybinding.js');
const { OperatingSystem } = await out('base/common/platform.js');
const version = JSON.parse(fs.readFileSync(path.join(vscodeDir, 'package.json'), 'utf8')).version;

const platforms = { linux: OperatingSystem.Linux, windows: OperatingSystem.Windows, macos: OperatingSystem.Macintosh };
const modifierKeys = { [KeyCode.Ctrl]: 'ctrl', [KeyCode.Shift]: 'shift', [KeyCode.Alt]: 'alt', [KeyCode.Meta]: 'meta' };

// The port keeps a modifier that is also the key out of the modifiers: `ctrl+ctrl` is `ctrl`. It does
// not read `[KeyK]`, a key by its position, or the key that stands for an input method at work.
function describe(input) {
    const keybinding = KeybindingParser.parseKeybinding(input);
    const unusable = (chord) =>
        chord.keyCode === undefined || chord.keyCode === KeyCode.Unknown || chord.keyCode === KeyCode.KEY_IN_COMPOSITION;
    if (!keybinding || keybinding.chords.length === 0 || keybinding.chords.some(unusable)) {
        return { input, steps: null };
    }
    const steps = keybinding.chords.map((chord) => {
        const own = modifierKeys[chord.keyCode];
        return {
            ctrl: chord.ctrlKey && own !== 'ctrl',
            shift: chord.shiftKey && own !== 'shift',
            alt: chord.altKey && own !== 'alt',
            meta: chord.metaKey && own !== 'meta',
            key: KeyCodeUtils.toUserSettingsUS(chord.keyCode).toLowerCase(),
        };
    });
    // VS Code prints such a chord as its modifiers alone, which the port, that keeps the key and not
    // the repeated modifier, does not repeat, so the case records the steps only.
    if (keybinding.chords.some((chord) => modifierKeys[chord.keyCode] && chord[`${modifierKeys[chord.keyCode]}Key`])) {
        return { input, steps, duplicate_modifier: true };
    }
    const settings = {};
    const labels = {};
    for (const [name, os] of Object.entries(platforms)) {
        const resolved = new USLayoutResolvedKeybinding(keybinding.chords, os);
        settings[name] = resolved.getUserSettingsLabel();
        labels[name] = resolved.getLabel();
    }
    return { input, steps, settings, labels };
}

const keyNames = new Set();
for (const name of Object.keys(KeyCode)) {
    const code = KeyCode[name];
    if (typeof code !== 'number' || code <= 0 || name === 'MAX_VALUE') {
        continue;
    }
    for (const spelling of [
        KeyCodeUtils.toUserSettingsUS(code),
        KeyCodeUtils.toUserSettingsGeneral(code),
        KeyCodeUtils.toString(code),
    ]) {
        if (spelling) {
            keyNames.add(spelling);
            keyNames.add(spelling.toLowerCase());
        }
    }
}

const modifierSets = [[], ['ctrl', 'shift'], ['alt', 'meta']];
const inputs = new Set();
for (const key of keyNames) {
    for (const modifiers of modifierSets) {
        inputs.add([...modifiers, key].join('+'));
    }
}

// Every subset of the modifiers, in both orders, with the other spellings of meta and both
// separators, on a few keys.
const allModifiers = ['ctrl', 'shift', 'alt', 'meta'];
for (let mask = 0; mask < 16; ++mask) {
    const held = allModifiers.filter((_, i) => mask & (1 << i));
    for (const key of ['k', 'f5', 'left', '-', 'numpad_add']) {
        for (const order of [held, [...held].reverse()]) {
            inputs.add([...order, key].join('+'));
            inputs.add([...order, key].join('-'));
            inputs.add([...order, key].join('+').toUpperCase());
            inputs.add([...order.map((m) => (m === 'meta' ? 'cmd' : m)), key].join('+'));
            inputs.add([...order.map((m) => (m === 'meta' ? 'win' : m)), key].join('+'));
            inputs.add([...order.map((m) => m[0].toUpperCase() + m.slice(1)), key].join('+'));
        }
    }
}

const chords = [
    'ctrl+k ctrl+c', 'ctrl+k ctrl+u', 'Ctrl+K Ctrl+C', 'ctrl+k  ctrl+c', '  ctrl+k ctrl+c  ', 'ctrl+k\tctrl+c',
    'ctrl+shift+k cmd+j', 'shift shift', 'ctrl ctrl', 'ctrl+ctrl', 'shift+shift', 'alt+alt+k', 'ctrl+shift', 'ctrl+alt',
    'ctrl+k ctrl+k ctrl+k', 'ctrl+k ctrl+k ctrl+k ctrl+k', 'ctrl+k ctrl+k ctrl+k ctrl+k ctrl+k', 'a b c d e f',
    'cmd+shift+p', 'f1', 'F1', 'escape', 'ctrl+oem_3', 'ctrl+numpad_add', 'alt+numpad0', 'ctrl+-', 'ctrl--', 'ctrl+=',
    'ctrl+,', 'ctrl+.', 'ctrl+/', "ctrl+'", 'ctrl+;', 'ctrl+[', 'ctrl+]', 'ctrl+\\', 'ctrl+`', '-', '=',
];
const invalid = [
    '', ' ', '\t', 'ctrl+', 'ctrl+ k', 'ctrl+shift+', '+', '++', 'ctrl++', 'foo', 'foo+k', 'ctrl+foo', 'ctrl+foo+k',
    'ctrl+k foo', 'leftarrow', 'ctrl+leftarrow', 'LeftArrow', 'del', 'ctrl+del', 'ctrl+return', 'ctrl+esc', 'ctrl+cmd',
    'super+k', 'ctrl+super+k', 'ctrl+[KeyK]', '[KeyK]', 'ctrl+f0', 'ctrl+f25', 'ctrl+k,ctrl+c', 'ctrl + k', 'ctrl +k',
    'ctrl+k+', 'k+k', 'oem_', 'numpad', 'numpad10', 'ctrl+é', 'ctrl+😀', 'keyincomposition', 'unknown', 'ctrl+meta+',
];
for (const input of [...chords, ...invalid]) {
    inputs.add(input);
}

const cases = [...inputs].sort().map(describe);
const generated = JSON.stringify({ vscode: version, cases }, null, 0).replaceAll('},{"input"', '},\n{"input"') + '\n';

if (process.argv.includes('--check')) {
    const current = fs.existsSync(output) ? fs.readFileSync(output, 'utf8') : '';
    if (current !== generated) {
        console.error(`${path.relative(root, output)} is out of date; run node scripts/generate-key-chord-cases.mjs`);
        process.exit(1);
    }
    console.log(`key chord cases up to date (${cases.length} cases, VS Code ${version})`);
} else {
    fs.writeFileSync(output, generated);
    console.log(`key chord cases: ${cases.length} cases, ${cases.filter((c) => c.steps === null).length} rejected (VS Code ${version})`);
}
