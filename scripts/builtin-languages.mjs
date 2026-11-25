// The languages that VS Code registers without an extension of the user's: `plaintext` from
// `modesRegistry.ts` and the `contributes.languages` of its built-in extensions, in the order the
// editor registers them. Shared by generate-languages.mjs and generate-language-cases.mjs.
import fs from 'node:fs';
import path from 'node:path';

const isUndefinedOrStrings = (value) =>
    value === undefined || (Array.isArray(value) && value.every((item) => typeof item === 'string'));

// What `isValidLanguageExtensionPoint` of the workbench's language service accepts.
function isValid(value) {
    return (
        !!value &&
        typeof value.id === 'string' &&
        isUndefinedOrStrings(value.extensions) &&
        isUndefinedOrStrings(value.filenames) &&
        isUndefinedOrStrings(value.filenamePatterns) &&
        (value.firstLine === undefined || typeof value.firstLine === 'string') &&
        isUndefinedOrStrings(value.aliases) &&
        isUndefinedOrStrings(value.mimetypes)
    );
}

// The `contributes.languages` entries of the built-in extensions with the folder of their extension,
// in the order the editor registers them. The extension scanner sorts the built-in extensions by the
// name of their folder.
export function* builtinLanguageContributions(vscodeDir) {
    const extensionsDir = path.join(vscodeDir, 'extensions');
    const folders = fs
        .readdirSync(extensionsDir, { withFileTypes: true })
        .filter((entry) => entry.isDirectory())
        .map((entry) => entry.name)
        .sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
    for (const folder of folders) {
        const manifest = path.join(extensionsDir, folder, 'package.json');
        if (!fs.existsSync(manifest)) {
            continue;
        }
        const languages = JSON.parse(fs.readFileSync(manifest, 'utf8')).contributes?.languages;
        for (const language of languages ?? []) {
            yield { folder: path.join(extensionsDir, folder), language };
        }
    }
}

export async function readBuiltinLanguages(vscodeDir) {
    const { ModesRegistry } = await import(path.join(vscodeDir, 'out/vs/editor/common/languages/modesRegistry.js'));
    const registered = ModesRegistry.getLanguages();
    if (registered.length !== 1 || registered[0].id !== 'plaintext') {
        throw new Error('the editor registers languages other than plaintext of its own; the generator has to learn them');
    }

    const contributions = [...registered];
    for (const { folder, language } of builtinLanguageContributions(vscodeDir)) {
        if (!isValid(language)) {
            throw new Error(`${path.basename(folder)}: invalid language contribution ${JSON.stringify(language)}`);
        }
        contributions.push({
            id: language.id,
            extensions: language.extensions,
            filenames: language.filenames,
            filenamePatterns: language.filenamePatterns,
            firstLine: language.firstLine,
            aliases: language.aliases,
            mimetypes: language.mimetypes,
            configuration: language.configuration,
        });
    }
    return contributions;
}

export function vscodeVersion(vscodeDir) {
    return JSON.parse(fs.readFileSync(path.join(vscodeDir, 'package.json'), 'utf8')).version;
}
