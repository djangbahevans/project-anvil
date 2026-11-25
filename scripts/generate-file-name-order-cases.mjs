// Records how VS Code's explorer orders file names (`compareFileNamesDefault`) for a fixed corpus,
// as tests/core/data/file_name_order_cases.json. tests/core/file_name_order_test.cpp checks that
// core/file_name_order.cpp orders the same names the same way.
// The comparer is loaded from the pinned VS Code checkout, and the collator is the one of the
// running Node.js with the locale `en-US`, so run this on a Node.js with full ICU data.
//   node scripts/generate-file-name-order-cases.mjs [--check]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const vscodeDir = path.join(root, '.deps', 'vscode');
const output = path.join(root, 'tests/core/data/file_name_order_cases.json');

const comparers = await import(path.join(vscodeDir, 'out/vs/base/common/comparers.js'));
const version = JSON.parse(fs.readFileSync(path.join(vscodeDir, 'package.json'), 'utf8')).version;

const groups = {
    numbers: [
        'file1.txt', 'file2.txt', 'file10.txt', 'file20.txt', 'file100.txt', 'file01.txt', 'file001.txt',
        'file1', 'file', 'file0.txt', 'a1b2', 'a1b10', 'a2b1', '1', '2', '10', '007', '7', '08', '9',
        'v1.2.10', 'v1.10.2', 'v1.2.9', '2024-01-02.log', '2024-01-10.log', '2024-1-3.log',
        '123456789012345678901234567890', '123456789012345678901234567891',
    ],
    case: [
        'readme', 'README', 'Readme', 'ReadMe', 'readMe', 'a', 'A', 'b', 'B', 'ab', 'aB', 'Ab', 'AB',
        'alpha', 'Beta', 'gamma', 'Delta', 'zeta', 'Zulu', 'apple.txt', 'Apple.txt', 'apple.TXT',
    ],
    accents: [
        'e', 'é', 'è', 'ê', 'ë', 'E', 'É', 'f', 'cafe', 'café', 'cafè', 'cafés', 'cafe2', 'a', 'á', 'à',
        'â', 'ã', 'ä', 'å', 'b', 'c', 'ç', 'd', 'n', 'ñ', 'o', 'ó', 'ò', 'ô', 'õ', 'ö', 'ø', 'p', 'u',
        'ú', 'ù', 'û', 'ü', 'y', 'ý', 'ÿ', 'z', 'æ', 'ae', 'af', 'ß', 'ss', 'st', 'Ø', 'Æ', 'Ñ', 'Ö',
        'résumé.txt', 'resume.txt', 'resumes.txt', 'Résumé.txt',
    ],
    punctuation: [
        'a b', 'a_b', 'a-b', 'a,b', 'a;b', 'a:b', 'a!b', 'a?b', 'a.b', "a'b", 'a"b', 'a(b', 'a)b',
        'a[b', 'a]b', 'a{b', 'a}b', 'a@b', 'a*b', 'a/b', 'a\\b', 'a&b', 'a#b', 'a%b', 'a`b', 'a^b',
        'a+b', 'a<b', 'a=b', 'a>b', 'a|b', 'a~b', 'a$b', 'a1b', 'aab', 'ab', 'a', '_a', '-a', '.a',
        '~a', '$a', ' a', '1a', '(a)', '[a]',
    ],
    dots: [
        '.git', '.gitignore', '.github', '.vscode', '.env', '.env.local', '.eslintrc.json', 'a.b.c',
        'a.b', 'a', 'a.', '.', '..', 'a..b', 'a.b.c.d', 'index.html', 'index.js', 'index.ts', 'index',
        'index.d.ts', 'index.test.ts', 'Makefile', 'makefile', 'CMakeLists.txt', 'cmake', 'src', 'Src',
    ],
    other: [
        '日本語.txt', '中文.txt', 'файл', 'Файл', 'αβγ', 'emoji-😀.txt', 'emoji-🙂.txt',
        'a😀', 'a😀b', 'z', 'z日', 'Ω', 'ω', 'Σ', 'σ',
    ],
};

const order = (names) => {
    const sorted = [...new Set(names)].sort(comparers.compareFileNamesDefault);
    for (let i = 1; i < sorted.length; i++) {
        if (comparers.compareFileNamesDefault(sorted[i - 1], sorted[i]) === 0) {
            throw new Error(`names that compare equal: ${JSON.stringify([sorted[i - 1], sorted[i]])}`);
        }
    }
    return sorted;
};

const cases = Object.entries(groups).map(([name, names]) => ({ name, sorted: order(names) }));
cases.push({ name: 'all', sorted: order(Object.values(groups).flat()) });

const text = JSON.stringify({ vscode: version, locale: new Intl.Collator().resolvedOptions().locale, cases }, null, 1) + '\n';
if (process.argv.includes('--check')) {
    if (fs.readFileSync(output, 'utf8') !== text) {
        console.error(`${output} is out of date`);
        process.exit(1);
    }
} else {
    fs.writeFileSync(output, text);
}
