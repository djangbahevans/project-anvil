// Generates src/workbench/include/anvil/workbench/menu_table.hpp: the menu bar of VS Code, with the menus,
// submenus, items, separators, mnemonic labels and `when` clauses that the pinned checkout registers.
//
// VS Code does not keep its menus in one place: modules append items to `MenuRegistry` as they load, and
// workbench contributions and services append more when they are created. The script loads the compiled
// desktop workbench in Node with just enough of a browser to let it load, creates every workbench
// contribution and the views service against inert services, and reads the registry back.
//   node scripts/generate-menus.mjs [--check]
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const vscodeDir = path.join(root, '.deps', 'vscode');
const output = path.join(root, 'src/workbench/include/anvil/workbench/menu_table.hpp');
const self = fileURLToPath(import.meta.url);

// Modules that load css or ask for a DOM class that Node does not have.
const cssHook = `data:text/javascript,${encodeURIComponent(`
export async function load(url, context, next) {
    if (url.endsWith('.css')) return { format: 'module', source: 'export default {};', shortCircuit: true };
    return next(url, context);
}`)}`;
const register = `data:text/javascript,${encodeURIComponent(`import { register } from 'node:module'; register(${JSON.stringify(cssHook)});`)}`;

// Whether an item exists can depend on a URL of the product; the shipping product has all of them.
const productUrls = [
    'documentationUrl',
    'introductoryVideosUrl',
    'tipsAndTricksUrl',
    'newsletterSignupUrl',
    'youTubeUrl',
    'requestFeatureUrl',
    'releaseNotesUrl',
    'privacyStatementUrl',
    'keyboardShortcutsUrlLinux',
    'keyboardShortcutsUrlMac',
    'keyboardShortcutsUrlWin',
];

async function dumpRegistry() {
    const { readFileSync } = await import('node:fs');
    const vs = vscodeDir;
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
    const product = { ...JSON.parse(readFileSync(path.join(vs, 'product.json'), 'utf8')) };
    for (const key of productUrls) {
        product[key] ??= 'https://example.invalid';
    }
    globalThis.vscode = {
        ipcRenderer: { on() {}, send() {}, invoke: async () => undefined, once() {} },
        ipcMessagePort: {},
        webFrame: {},
        context: { configuration: () => ({ product }) },
    };
    globalThis._VSCODE_FILE_ROOT = `file://${vs}/out`;

    const load = (file) => import(path.join(vs, 'out/vs', file));
    const { MenuId, MenuRegistry } = await load('platform/actions/common/actions.js');
    await load('workbench/workbench.desktop.main.js');

    // A service that accepts every call and every property, and calls back the functions given to `then`, so
    // that a contribution that registers its actions when a promise settles registers them here.
    const inert = (thenable = true) => {
        const proxy = new Proxy(function () {}, {
            get: (_target, key) => {
                if (key === 'then') return thenable ? (callback) => { try { callback?.(inert(false)); } catch {} return proxy; } : undefined;
                if (key === Symbol.toPrimitive) return () => '';
                if (key === Symbol.iterator) return function* () {};
                return proxy;
            },
            apply: () => proxy,
            construct: () => proxy,
            has: () => true,
        });
        return proxy;
    };
    const withOverrides = (overrides) => {
        const base = inert();
        return new Proxy(function () {}, { get: (_target, key) => (key in overrides ? overrides[key] : base[key]) });
    };

    const { WorkbenchContributionsRegistry } = await load('workbench/common/contributions.js');
    const registry = WorkbenchContributionsRegistry.INSTANCE;
    const contributions = new Set();
    for (const list of registry.contributionsByPhase.values()) list.forEach((c) => contributions.add(c));
    for (const c of registry.contributionsById.values()) contributions.add(c);
    for (const list of registry.contributionsByEditor.values()) list.forEach((c) => contributions.add(c));
    for (const { ctor } of contributions) {
        try {
            new ctor(...Array.from({ length: Math.max(ctor.length, 12) }, () => inert()));
        } catch {
            // A contribution that needs a real service registers nothing here; the checks below notice a missing menu.
        }
    }

    // The views service adds the View menu's entries for the view containers and views of the registries.
    const { Registry } = await load('platform/registry/common/platform.js');
    const { Extensions: ViewExtensions, ViewContainerLocation } = await load('workbench/common/views.js');
    const { Extensions: PaneExtensions } = await load('workbench/browser/panecomposite.js');
    const { getSingletonServiceDescriptors } = await load('platform/instantiation/common/extensions.js');
    const containers = Registry.as(ViewExtensions.ViewContainersRegistry);
    const views = Registry.as(ViewExtensions.ViewsRegistry);
    const noEvent = () => ({ dispose() {} });
    const registryIds = {
        [ViewContainerLocation.Sidebar]: PaneExtensions.Viewlets,
        [ViewContainerLocation.Panel]: PaneExtensions.Panels,
        [ViewContainerLocation.AuxiliaryBar]: PaneExtensions.Auxiliary,
    };
    const viewDescriptorService = withOverrides({
        viewContainers: containers.all,
        getViewContainerLocation: (container) => containers.getViewContainerLocation(container),
        getDefaultViewContainerLocation: (container) => containers.getViewContainerLocation(container),
        getDefaultContainerById: (id) => views.getViewContainer(id) ?? null,
        getViewContainerModel: (container) => ({
            allViewDescriptors: views.getViews(container),
            activeViewDescriptors: views.getViews(container),
            visibleViewDescriptors: views.getViews(container),
            onDidChangeAllViewDescriptors: noEvent,
            onDidChangeActiveViewDescriptors: noEvent,
        }),
    });
    const paneCompositeService = withOverrides({ getRegistryId: (location) => registryIds[location] });
    for (const [, descriptor] of getSingletonServiceDescriptors()) {
        if (descriptor.ctor?.name === 'ViewsService') {
            new descriptor.ctor(viewDescriptorService, paneCompositeService, ...Array.from({ length: 10 }, () => inert()));
        }
    }

    const title = (t) => (typeof t === 'string' ? { value: t, original: t } : t);
    const dump = (menuId) =>
        MenuRegistry.getMenuItems(menuId).map((item) => {
            if ('submenu' in item) {
                return { id: item.submenu.id, title: title(item.title), group: item.group, order: item.order, when: item.when?.serialize(), children: dump(item.submenu) };
            }
            const { id, precondition, toggled } = item.command;
            return {
                id,
                title: title(item.command.title),
                group: item.group,
                order: item.order,
                when: item.when?.serialize(),
                precondition: precondition?.serialize(),
                toggled: (toggled?.condition ?? toggled)?.serialize(),
            };
        });
    const { Extensions: ConfigurationExtensions } = await load('platform/configuration/common/configurationRegistry.js');
    const properties = Registry.as(ConfigurationExtensions.Configuration).getConfigurationProperties();
    const defaults = {};
    for (const [key, property] of Object.entries(properties)) {
        defaults[key] = property.default === undefined ? null : property.default;
    }
    process.stdout.write(`JSON${JSON.stringify({ menus: dump(MenuId.MenubarMainMenu), defaults })}\n`);
    process.exit(0);
}

if (process.env.ANVIL_MENU_PLATFORM) {
    Object.defineProperty(process, 'platform', { value: process.env.ANVIL_MENU_PLATFORM });
    await dumpRegistry();
}

function run(platform) {
    const result = spawnSync(process.execPath, ['--import', register, self], {
        env: { ...process.env, ANVIL_MENU_PLATFORM: platform },
        encoding: 'utf8',
        maxBuffer: 64 * 1024 * 1024,
        timeout: 300000,
    });
    const line = result.stdout?.split('\n').find((l) => l.startsWith('JSON'));
    if (!line) {
        throw new Error(`reading the menus for ${platform} failed:\n${result.stderr?.slice(-2000)}`);
    }
    return JSON.parse(line.slice(4));
}

const version = JSON.parse(fs.readFileSync(path.join(vscodeDir, 'package.json'), 'utf8')).version;
const { menus: linux, defaults: linuxDefaults } = run('linux');
const { menus: windows, defaults: windowsDefaults } = run('win32');
if (JSON.stringify(linux) !== JSON.stringify(windows)) {
    throw new Error('the menus of Linux and Windows differ; the table would need a platform column');
}

// VS Code's order: a group sorts as a string with `navigation` first and no group last, then by `order`,
// then by the original title (`menuService.js`, `MenuInfo.compareMenuItems`).
const compare = (a, b) => {
    const ga = a.group || '';
    const gb = b.group || '';
    if (ga !== gb) {
        if (!ga) return 1;
        if (!gb) return -1;
        if (ga === 'navigation') return -1;
        if (gb === 'navigation') return 1;
        const value = ga.localeCompare(gb);
        if (value !== 0) return value;
    }
    const oa = a.order || 0;
    const ob = b.order || 0;
    if (oa !== ob) return oa < ob ? -1 : 1;
    return a.title.original.localeCompare(b.title.original);
};

const rows = [];
const flatten = (items, depth) => {
    let previous;
    for (const item of [...items].sort(compare)) {
        const group = item.group || '';
        if (depth > 0 && previous !== undefined && group !== previous) {
            rows.push({ depth, kind: 'separator', label: '', id: '', when: '', precondition: '', toggled: '' });
        }
        previous = group;
        const label = (item.title.mnemonicTitle ?? item.title.value).replaceAll('{0}', 'Default');
        rows.push({
            depth,
            kind: item.children ? 'submenu' : 'item',
            label,
            id: item.id,
            when: item.when ?? '',
            precondition: item.precondition ?? '',
            toggled: item.toggled ?? '',
        });
        if (item.children) {
            flatten(item.children, depth + 1);
        }
    }
};
flatten(linux, 0);

// The settings that the clauses read as `config.<key>`, with the defaults that apply while the settings file says nothing.
const configKeys = [
    ...new Set(
        rows.flatMap((row) =>
            [row.when, row.precondition, row.toggled].flatMap((clause) =>
                [...clause.matchAll(/\bconfig\.([\w.]+)/g)].map((match) => match[1]),
            ),
        ),
    ),
].sort();
const configDefaults = configKeys.map((key) => {
    if (!(key in linuxDefaults)) {
        throw new Error(`the clauses read config.${key}, which no configuration registers`);
    }
    if (JSON.stringify(linuxDefaults[key]) !== JSON.stringify(windowsDefaults[key])) {
        throw new Error(`the default of ${key} differs between Linux and Windows`);
    }
    return { key, value: JSON.stringify(linuxDefaults[key]) };
});

const submenus = rows.filter((row) => row.kind === 'submenu' && row.depth === 0).map((row) => row.id);
const expected = [
    'MenubarFileMenu',
    'MenubarEditMenu',
    'MenubarSelectionMenu',
    'MenubarViewMenu',
    'MenubarGoMenu',
    'MenubarDebugMenu',
    'MenubarTerminalMenu',
    'MenubarHelpMenu',
    'MenubarPreferencesMenu',
];
if (JSON.stringify(submenus) !== JSON.stringify(expected)) {
    throw new Error(`the top-level menus are ${submenus.join(', ')}`);
}
for (const [menu, command] of [
    ['MenubarEditMenu', 'editor.action.clipboardPasteAction'],
    ['MenubarViewMenu', 'workbench.view.explorer'],
    ['MenubarViewMenu', 'workbench.action.terminal.toggleTerminal'],
    ['MenubarTerminalMenu', 'workbench.action.terminal.new'],
    ['MenubarFileMenu', 'workbench.action.openSettings'],
    ['MenubarHelpMenu', 'workbench.action.openDocumentationUrl'],
]) {
    if (!rows.some((row) => row.id === command)) {
        throw new Error(`${command} of ${menu} is missing: a registration that the script does not reach`);
    }
}

const quote = (text) => `"${text.replaceAll('\\', '\\\\').replaceAll('"', '\\"')}"`;
const generated = [
    '// Generated by scripts/generate-menus.mjs; do not edit.',
    `// The menu bar of VS Code ${version} on Linux and Windows, in the order it shows: each entry follows its parent`,
    '// and is as deep as its parent plus one. A submenu has the id of its menu, an item the id of its command.',
    '#pragma once',
    '',
    '#include <cstdint>',
    '#include <string_view>',
    '',
    'namespace anvil::workbench {',
    '',
    'enum class MenuEntryKind : std::uint8_t { item, submenu, separator };',
    '',
    'struct MenuEntry {',
    '    std::uint8_t depth;',
    '    MenuEntryKind kind;',
    '    // The title with `&&` before the mnemonic character, as VS Code writes it.',
    '    std::string_view label;',
    '    std::string_view id;',
    '    // The `when` clause under which the entry is shown, or empty.',
    '    std::string_view when;',
    '    // The condition under which the item is enabled, or empty.',
    '    std::string_view precondition;',
    '    // The condition under which the item shows a check mark, or empty.',
    '    std::string_view toggled;',
    '};',
    '',
    '// A setting that the clauses read as `config.<key>`, with its default as JSON.',
    'struct MenuConfigDefault {',
    '    std::string_view key;',
    '    std::string_view json;',
    '};',
    '',
    'inline constexpr MenuConfigDefault menu_config_defaults[] = {',
    ...configDefaults.map((entry) => `    {${quote(entry.key)}, ${quote(entry.value)}},`),
    '};',
    '',
    'inline constexpr MenuEntry menu_table[] = {',
    ...rows.map(
        (row) => `    {${row.depth}, MenuEntryKind::${row.kind}, ${quote(row.label)}, ${quote(row.id)}, ${quote(row.when)}, ${quote(row.precondition)}, ${quote(row.toggled)}},`,
    ),
    '};',
    '',
    '} // namespace anvil::workbench',
    '',
].join('\n');

if (process.argv.includes('--check')) {
    const current = fs.existsSync(output) ? fs.readFileSync(output, 'utf8') : '';
    if (current !== generated) {
        console.error(`${path.relative(root, output)} is out of date; run node scripts/generate-menus.mjs`);
        process.exit(1);
    }
    console.log(`${path.relative(root, output)} is up to date`);
} else {
    fs.writeFileSync(output, generated);
    console.log(`wrote ${rows.length} entries to ${path.relative(root, output)}`);
}
