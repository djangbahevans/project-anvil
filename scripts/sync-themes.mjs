// Copies the color themes that VS Code ships (the built-in extension `theme-defaults`) from the
// pinned checkout into resources/themes, with a themes.json that lists them and the VS Code license
// text. The theme files are copied byte for byte.
//   node scripts/sync-themes.mjs [--check]
// --check changes nothing and fails when resources/themes differs from the checkout.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const extension = path.join(root, '.deps', 'vscode', 'extensions', 'theme-defaults');
const output = path.join(root, 'resources', 'themes');
const check = process.argv.includes('--check');

const manifest = JSON.parse(fs.readFileSync(path.join(extension, 'package.json'), 'utf8'));
const labels = JSON.parse(fs.readFileSync(path.join(extension, 'package.nls.json'), 'utf8'));
const localized = (text) => {
    const key = /^%(.*)%$/.exec(text)?.[1];
    if (key === undefined) {
        return text;
    }
    if (!(key in labels)) {
        throw new Error(`package.nls.json has no label ${key}`);
    }
    return labels[key];
};

const wanted = new Map();
const themes = manifest.contributes.themes.map((theme) => {
    const file = path.basename(theme.path);
    wanted.set(file, fs.readFileSync(path.join(extension, theme.path)));
    return { id: theme.id ?? localized(theme.label), label: localized(theme.label), uiTheme: theme.uiTheme, path: file };
});
wanted.set('themes.json', Buffer.from(JSON.stringify(themes, null, 2) + '\n'));
wanted.set('LICENSE-vscode.txt', fs.readFileSync(path.join(root, '.deps', 'vscode', 'LICENSE.txt')));

const differences = [];
for (const [name, content] of wanted) {
    const target = path.join(output, name);
    const current = fs.existsSync(target) ? fs.readFileSync(target) : null;
    if (current === null || !current.equals(content)) {
        differences.push(name);
        if (!check) {
            fs.mkdirSync(output, { recursive: true });
            fs.writeFileSync(target, content);
        }
    }
}
const stale = fs.existsSync(output) ? fs.readdirSync(output).filter((name) => !wanted.has(name)) : [];

if (check) {
    if (differences.length > 0 || stale.length > 0) {
        console.error(`resources/themes is out of date (${[...differences, ...stale].join(', ')}); run node scripts/sync-themes.mjs`);
        process.exit(1);
    }
} else {
    for (const name of stale) {
        fs.rmSync(path.join(output, name));
    }
    console.log(`resources/themes: ${themes.length} themes, ${differences.length} files written, ${stale.length} removed`);
}
