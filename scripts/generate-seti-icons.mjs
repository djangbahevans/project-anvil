// Refreshes what the editor ships of VS Code's Seti file icon theme from the copy in the pinned
// checkout (extensions/theme-seti): the font, copied byte for byte, its notice, and the table of
// the icons of the languages, file names and file extensions in src/ui/seti_icon_table.hpp.
//   node scripts/generate-seti-icons.mjs [--check]
// --check changes nothing and fails when any of the three differs from the checkout.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const extension = path.join(root, '.deps', 'vscode', 'extensions', 'theme-seti');
const fontOutput = path.join(root, 'resources', 'seti', 'seti.woff');
const licenseOutput = path.join(root, 'resources', 'seti', 'LICENSE-seti.txt');
const tableOutput = path.join(root, 'src', 'ui', 'seti_icon_table.hpp');
const check = process.argv.includes('--check');

const version = JSON.parse(fs.readFileSync(path.join(extension, 'package.json'), 'utf8')).version;
const theme = JSON.parse(fs.readFileSync(path.join(extension, 'icons', 'vs-seti-icon-theme.json'), 'utf8'));

// The glyph of a definition of the theme. A definition with no color takes `inherited`, and has
// none of its own where nothing is given.
function glyph(id, inherited) {
    const definition = theme.iconDefinitions[id];
    if (!definition) {
        throw new Error(`no icon definition ${id}`);
    }
    const unknown = Object.keys(definition).filter((key) => key !== 'fontCharacter' && key !== 'fontColor');
    const character = /^\\([0-9A-Fa-f]{4})$/.exec(definition.fontCharacter ?? '');
    const wanted = definition.fontColor ?? inherited;
    const color = /^#([0-9A-Fa-f]{6})$/.exec(wanted ?? '');
    if (unknown.length > 0 || !character || (wanted !== undefined && !color)) {
        throw new Error(`${id} is not a glyph with a color: ${JSON.stringify(definition)}`);
    }
    return { code: character[1].toUpperCase(), color: color?.[1].toUpperCase(), text: wanted };
}

// VS Code gives `jsonc` the icon of `json` where a section does not name it.
function languagesOf(section) {
    const languages = { ...section.languageIds };
    if (!languages.jsonc && languages.json) {
        languages.jsonc = languages.json;
    }
    return languages;
}

const darkLanguages = languagesOf(theme);
const lightLanguages = languagesOf(theme.light);
const darkDefault = glyph(theme.file);
const lightDefault = glyph(theme.light.file);
if (!darkDefault.color || !lightDefault.color) {
    throw new Error('the default file icon has no color');
}
for (const [dark, light] of [
    [darkLanguages, lightLanguages],
    [theme.fileNames, theme.light.fileNames],
    [theme.fileExtensions, theme.light.fileExtensions],
]) {
    for (const key of Object.keys(light)) {
        if (!dark[key]) {
            throw new Error(`${key} has a light icon and no dark one`);
        }
    }
}

// The rules follow the style sheet that VS Code makes of the theme (fileIconThemeData.ts). A
// definition without a color is left with that of the default file icon. On a light theme the rule
// of the light default icon is as specific as the dark rule of a language and comes after it, so a
// language with no light icon of its own shows the light default one.
const entries = Object.keys(darkLanguages)
    .sort()
    .map((id) => ({
        id,
        dark: glyph(darkLanguages[id], darkDefault.text),
        light: lightLanguages[id] ? glyph(lightLanguages[id], lightDefault.text) : lightDefault,
    }));

// File names and extensions, as VS Code compares them: in lower case. Which of them wins for a
// file, and where a glyph without a color takes one from, is decided when the icon is looked up
// (src/ui/seti_icons.cpp).
function filesOf(kind) {
    const dark = theme[kind];
    const light = theme.light[kind];
    const entries = Object.keys(dark).map((key) => {
        // A key with a directory (`dir/name`) would need the parent of a file to be matched.
        if (!/^[\x21-\x7e]+$/.test(key) || /["\\/]/.test(key)) {
            throw new Error(`${kind}: ${key} is not a name that the lookup can match`);
        }
        // Without a light rule of its own, a name keeps its dark one on a light theme, which is
        // more specific than what the light rules say of its extensions. An extension would not.
        if (!light[key] && kind !== 'fileNames') {
            throw new Error(`${kind}: ${key} has no light icon, which the lookup does not model`);
        }
        const own = glyph(dark[key]);
        return { key: key.toLowerCase(), dark: own, light: light[key] ? glyph(light[key]) : own };
    });
    entries.sort((a, b) => (a.key < b.key ? -1 : a.key > b.key ? 1 : 0));
    if (new Set(entries.map((entry) => entry.key)).size !== entries.length) {
        throw new Error(`${kind}: two keys differ only in case`);
    }
    return entries;
}

const fileNames = filesOf('fileNames');
const fileExtensions = filesOf('fileExtensions');

const cppGlyph = (value) => `{0x${value.code}, ${value.color ? `0xFF${value.color}` : '0'}}`;
const cppTable = (name, rows) => [
    `inline constexpr SetiIconEntry ${name}[] = {`,
    ...rows.map((entry) => `    {"${entry.key}", {${cppGlyph(entry.dark)}, ${cppGlyph(entry.light)}}},`),
    '};',
];
const table = [
    '// Generated by scripts/generate-seti-icons.mjs; do not edit.',
    `// The icons that the Seti file icon theme of VS Code (theme-seti ${version}) gives languages,`,
    '// file names and file extensions, each table ordered by its key, with the glyph and color of an',
    '// icon on a dark and on a light theme. A color of 0 is none of its own.',
    '#pragma once',
    '',
    '#include "anvil/ui/seti_icons.hpp"',
    '',
    'namespace anvil::ui {',
    '',
    `inline constexpr SetiIcon seti_default_icon_data{${cppGlyph(darkDefault)}, ${cppGlyph(lightDefault)}};`,
    '',
    ...cppTable('seti_icon_table_data', entries.map((entry) => ({ ...entry, key: entry.id }))),
    '',
    ...cppTable('seti_file_name_table_data', fileNames),
    '',
    ...cppTable('seti_file_extension_table_data', fileExtensions),
    '',
    '} // namespace anvil::ui',
    '',
].join('\n');

const wanted = [
    [fontOutput, fs.readFileSync(path.join(extension, 'icons', 'seti.woff'))],
    [licenseOutput, fs.readFileSync(path.join(extension, 'ThirdPartyNotices.txt'))],
    [tableOutput, Buffer.from(table)],
];

const differences = [];
for (const [target, content] of wanted) {
    const current = fs.existsSync(target) ? fs.readFileSync(target) : null;
    if (current === null || !current.equals(content)) {
        differences.push(path.relative(root, target));
        if (!check) {
            fs.mkdirSync(path.dirname(target), { recursive: true });
            fs.writeFileSync(target, content);
        }
    }
}

const counts = `${entries.length} languages, ${fileNames.length} file names, ${fileExtensions.length} extensions`;
if (check) {
    if (differences.length > 0) {
        console.error(`${differences.join(', ')} out of date; run node scripts/generate-seti-icons.mjs`);
        process.exit(1);
    }
    console.log(`Seti icons up to date (${counts}, theme-seti ${version})`);
} else {
    console.log(`Seti icons: ${counts}, ${differences.length} files written (theme-seti ${version})`);
}
