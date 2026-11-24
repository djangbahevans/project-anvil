// Records what VS Code's own language service decides for a corpus of paths and first lines, as
// tests/core/data/language_cases.json. tests/core/language_registry_test.cpp replays the cases
// against core/language_registry.cpp, so the port stays faithful. The built-in languages are
// registered in VS Code's `LanguagesRegistry` as the workbench does, from the pinned checkout, so
// the file changes when that is upgraded.
//   node scripts/generate-language-cases.mjs [--check]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { readBuiltinLanguages, vscodeVersion } from './builtin-languages.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const vscodeDir = path.join(root, '.deps', 'vscode');
const output = path.join(root, 'tests/core/data/language_cases.json');

const out = (file) => import(path.join(vscodeDir, 'out/vs', file));
const { LanguageService } = await out('editor/common/services/languageService.js');
const { clearConfiguredLanguageAssociations, registerConfiguredLanguageAssociation } = await out(
    'editor/common/services/languagesAssociations.js',
);
const { URI } = await out('base/common/uri.js');

const contributions = await readBuiltinLanguages(vscodeDir);
const service = new LanguageService(false);
service._registry.setDynamicLanguages(contributions.slice(1));
const version = vscodeVersion(vscodeDir);

// `untitled` keeps the path as it is, where `file` would turn it into a native one.
const resource = (p) => URI.from({ scheme: 'untitled', path: p });

function languageOf(p, firstLine) {
    const id = service.guessLanguageIdByFilepathOrFirstLine(p === '' ? null : resource(p), firstLine || undefined);
    return id && service.isRegisteredLanguageId(id) ? id : 'plaintext';
}

// A path that the pattern matches, with each wildcard filled in.
function instance(pattern, fill = 'x') {
    let text = pattern;
    for (let open = text.indexOf('{'); open >= 0; open = text.indexOf('{')) {
        const close = text.indexOf('}', open);
        if (close < 0) {
            break;
        }
        text = text.slice(0, open) + text.slice(open + 1, close).split(',')[0] + text.slice(close + 1);
    }
    return text.replaceAll('**/', `${fill}/${fill}/`).replaceAll('**', fill).replaceAll('*', fill).replaceAll('?', 'z');
}

const paths = new Set(['/', '/a', '/a/', '/a/b/', '/.', '/..', '/a//b.ts', '/a/b.ts/', '/a/b.ts/c', '/a.b/c', 'a.ts', 'ts']);
const firstLines = new Set(['']);
const directories = ['/w', '/w/proj/src', '/home/u/.config/git', '/w/.github/agents', '/w/rebase-merge'];
const tricky = [
    '.', '..', '.ts', 'ts', 'a.ts', 'a.d.ts', 'a.D.TS', 'a.test.ts', 'a.ts.bak', 'a.tsx', 'a.ts~', 'a.ts.', 'a b.ts', 'é.ts',
    'É.TS', 'a.d', '.d.ts', '..ts', 'a..ts', 'package.json', 'Package.JSON', 'a.json', 'a.jsonc', 'a.json5', '.vscode',
    '.eslintrc', '.eslintrc.json', '.gitignore', '.env', '.env.local', '.env.production.local', 'env', 'Dockerfile',
    'dockerfile', 'DOCKERFILE', 'Dockerfile.dev', 'dockerfile.prod', 'Containerfile', 'Containerfile.x', 'a.dockerfile',
    'Makefile', 'makefile', 'GNUmakefile', 'a.mk', 'CMakeLists.txt', 'a.cmake', 'tsconfig.json', 'tsconfig.app.json',
    'tsconfig-app.json', 'jsconfig.json', 'jsconfig.x.json', 'docker-compose.yml', 'docker-compose.override.yaml',
    'compose.yml', 'compose.yaml', 'compose.prod.yml', 'my-docker-compose.yml', 'a.yml', 'a.yaml', 'README', 'README.md',
    'readme.MD', 'a.mdc', 'Jenkinsfile', 'Jenkinsfile.x', 'snippets.json', 'my-snippets.json', 'settings.json', 'config',
    'a.sh', 'a.bash', '.bashrc', '.zshrc', 'a.py', 'a.pyw', 'a.rb', 'Gemfile', 'Rakefile', 'a.c', 'a.h', 'a.cpp', 'a.hpp',
    'a.cc', 'a.cxx', 'a.rs', 'a.go', 'a.java', 'a.cs', 'a.php', 'a.pl', 'a.pm', 'a.raku', 'a.lua', 'a.r', 'a.R', 'a.sql',
    'a.html', 'a.htm', 'a.css', 'a.scss', 'a.less', 'a.xml', 'a.svg', 'a.xsl', 'a.txt', 'a.TXT', 'a.log', 'a.ini',
    'a.gitconfig', 'a.diff', 'a.patch', 'a.code-workspace', 'a.code-snippets', '.babelrc', '.jshintrc',
];
for (const directory of directories) {
    for (const name of tricky) {
        paths.add(`${directory}/${name}`);
    }
}
for (const name of tricky) {
    paths.add(name);
}
paths.add('/home/u/.config/git/config');
paths.add('/home/u/.git/config');
paths.add('/w/.git/config');
paths.add('/w/.git/config.bak');
paths.add('/w/.git/rebase-merge/done');
paths.add('/w/.git/rebase-merge/done.bak');
paths.add('/home/u/.config/Code/User/snippets/x.json');
paths.add('/home/u/.config/Code/User/profiles/p/snippets/x.json');
paths.add('/w/.github/hooks/a.json');
paths.add('/w/.github/hooks/sub/a.json');
paths.add('/w/.claude/rules/a/b/c.md');
paths.add('/w/.claude/agents/a.md');
paths.add('/w/.cursor/rules/a.mdc');
paths.add('/w/.cursor/a.mdc');

for (const language of contributions) {
    for (const extension of language.extensions ?? []) {
        for (const name of [`a${extension}`, `A${extension.toUpperCase()}`, `a.b${extension}`, `${extension}`, `a${extension}.bak`]) {
            paths.add(`/w/${name}`);
        }
        paths.add(`/w/dir${extension}/file`);
    }
    for (const filename of language.filenames ?? []) {
        for (const name of [filename, filename.toUpperCase(), `x.${filename}`, `${filename}.x`, `x${filename}`]) {
            paths.add(`/w/${name}`);
            paths.add(name);
        }
        paths.add(`/w/${filename}/inner`);
    }
    for (const pattern of language.filenamePatterns ?? []) {
        for (const fill of ['x', 'a.b', 'Q']) {
            const filled = instance(pattern, fill);
            paths.add(`/w/${filled}`);
            paths.add(`/w/${filled}.bak`);
            paths.add(filled);
            paths.add(`/w/${filled.toUpperCase()}`);
        }
        paths.add(`/w/${pattern}`);
    }
}

for (const line of [
    '#!/bin/bash', '#!/bin/sh', '#!/usr/bin/env bash', '#!/usr/bin/env zsh', '#!/usr/bin/env python', '#!/usr/bin/env python3',
    '#!/usr/bin/python3.11', '#!python', '#! /usr/bin/ruby', '#!/usr/bin/env ruby', '#!/usr/bin/perl', '#!/usr/bin/perl6',
    '#!/usr/bin/env node', '#!/usr/bin/env deno', '#!/usr/bin/env bun', '#!/usr/bin/env ts-node', '#!/usr/bin/env pwsh',
    '#!/usr/bin/php', '#!/usr/bin/env php', '#!/usr/bin/env groovy', '#!/usr/bin/env julia', '#!/usr/bin/julia1.9',
    '#!/usr/bin/make -f', '#cloud-config', '#cloud-config\n', '#cloud-config extra', ' #cloud-config', '<?xml version="1.0"?>',
    '<?xml', ' <?xml', '<svg xmlns="http://www.w3.org/2000/svg">', '<!DOCTYPE svg PUBLIC', '<!doctype svg', '<!DOCTYPE html>',
    '# -*- mode: shell-script -*-', '# -*- mode: python -*-', 'use v6;', 'my class Foo', 'raku', 'x raku', '=begin pod',
    '#!', '#', '', ' ', '   ', '\t', 'hello world', '{"a": 1}', '# comment', '﻿#!/bin/bash', '﻿', '﻿﻿#!/bin/bash',
    '﻿<?xml version="1.0"?>', '#!/bin/bash\r', '#!/bin/bash ', '#!/bin/bash # comment', 'é#!/bin/bash', '#!/bin/bash é',
]) {
    firstLines.add(line);
}

const sortedPaths = [...paths].sort();
const cases = [];
const record = (p, firstLine) => cases.push({ path: p, first_line: firstLine, language: languageOf(p, firstLine) });

// Every path alone, with a first line that no rule reads, and with each first line on a few paths
// that no rule gives a language.
for (const p of sortedPaths) {
    record(p, '');
    record(p, '#!/usr/bin/env python3');
}
for (const line of [...firstLines].sort()) {
    for (const p of ['/w/unknown', '/w/a.unknownext', '/w/a.ts', '/w/Makefile', '/w/a.sh', '/w/dir/', '', 'x']) {
        record(p, line);
    }
}

// The user's associations (`files.associations`) over the registered rules.
const userSets = [
    [{ pattern: '*.ts', id: 'python' }],
    [{ pattern: '*.ts', id: 'python' }, { pattern: '*.ts', id: 'ruby' }],
    [{ pattern: '*.ts', id: 'python' }, { pattern: '**/*.ts', id: 'ruby' }],
    [{ pattern: '*', id: 'json' }, { pattern: '*.md', id: 'markdown' }],
    [{ pattern: '**/src/*.ts', id: 'javascript' }, { pattern: '*.ts', id: 'ruby' }],
    [{ pattern: 'Makefile', id: 'python' }, { pattern: 'build/**', id: 'shellscript' }],
    [{ pattern: '*.SH', id: 'yaml' }],
    [{ pattern: '*.nosuchext', id: 'nosuchlanguage' }],
    [{ pattern: '*.{ts,js}', id: 'json' }, { pattern: '*.t?', id: 'ruby' }],
    [{ pattern: 'a*', id: 'json' }, { pattern: '*a', id: 'ruby' }],
    [{ pattern: '**/proj/**', id: 'cpp' }],
    [{ pattern: '.env*', id: 'ini' }],
];
const userPaths = [
    '/w/a.ts', '/w/proj/src/a.ts', '/w/src/a.ts', '/w/a.js', '/w/a.md', '/w/README.md', '/w/Makefile', '/w/build/x/y.sh',
    '/w/a.SH', '/w/a.sh', '/w/a.nosuchext', '/w/aba', '/w/a', '/w/.env', '/w/.env.local', '/w/dir/', '/w/unknown', '/w/a.cpp',
];
const userCases = [];
for (const associations of userSets) {
    clearConfiguredLanguageAssociations();
    for (const { pattern, id } of associations) {
        registerConfiguredLanguageAssociation({ id, mime: `text/x-${id}`, filepattern: pattern });
    }
    userCases.push({
        associations: associations.map(({ pattern, id }) => ({ pattern, language: id })),
        cases: userPaths.flatMap((p) =>
            ['', '#!/bin/bash'].map((firstLine) => ({ path: p, first_line: firstLine, language: languageOf(p, firstLine) })),
        ),
    });
}
clearConfiguredLanguageAssociations();

const languages = service._registry.getRegisteredLanguageIds().map((id) => ({
    id,
    name: service._registry.getLanguageName(id),
    mime_types: service._registry.getMimeType(id) === null ? [] : service._registry._languages[id].mimetypes,
    extensions: [...service._registry.getExtensions(id)].sort(),
    filenames: [...service._registry.getFilenames(id)],
}));

const lines = (items) => items.map((item) => JSON.stringify(item)).join(',\n');
const generated =
    `{"vscode":${JSON.stringify(version)},\n"languages":[\n${lines(languages)}\n],\n` +
    `"cases":[\n${lines(cases)}\n],\n"user_cases":[\n${lines(userCases)}\n]}\n`;

if (process.argv.includes('--check')) {
    const current = fs.existsSync(output) ? fs.readFileSync(output, 'utf8') : '';
    if (current !== generated) {
        console.error(`${path.relative(root, output)} is out of date; run node scripts/generate-language-cases.mjs`);
        process.exit(1);
    }
    console.log(`language cases up to date (${cases.length} cases, VS Code ${version})`);
} else {
    fs.writeFileSync(output, generated);
    const found = new Set(cases.map((c) => c.language));
    console.log(`language cases: ${cases.length} cases over ${found.size} languages, ${userCases.length} user sets (VS Code ${version})`);
}
