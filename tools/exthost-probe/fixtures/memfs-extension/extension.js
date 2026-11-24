const vscode = require('vscode');

// An in-memory file system for the scheme `memfs`, and a read-only one for `memro` that holds one
// file. The commands use `workspace.fs` on these resources, as another extension would; `rename`
// and `copy` reach the editor, which calls the provider, because the extension host has no shortcut
// for them.
class MemoryFileSystem {
    constructor(readonly) {
        this.readonly = readonly;
        this.entries = new Map([['/', { type: vscode.FileType.Directory, children: new Set(), ctime: 1, mtime: 1 }]]);
        this._emitter = new vscode.EventEmitter();
        this.onDidChangeFile = this._emitter.event;
        this.watches = [];
    }
    fire(type, path) {
        this._emitter.fire([{ type, uri: vscode.Uri.parse(`${this.scheme}:${path}`) }]);
    }
    watch(uri, options) {
        const record = { path: uri.path, recursive: options.recursive };
        this.watches.push(record);
        return new vscode.Disposable(() => {
            this.watches.splice(this.watches.indexOf(record), 1);
        });
    }
    entry(uri) {
        const entry = this.entries.get(uri.path.replace(/(.)\/$/, '$1'));
        if (!entry) {
            throw vscode.FileSystemError.FileNotFound(uri);
        }
        return entry;
    }
    stat(uri) {
        const entry = this.entry(uri);
        return { type: entry.type, ctime: entry.ctime, mtime: entry.mtime, size: entry.data ? entry.data.length : 0 };
    }
    readDirectory(uri) {
        const entry = this.entry(uri);
        if (entry.type !== vscode.FileType.Directory) {
            throw vscode.FileSystemError.FileNotADirectory(uri);
        }
        return [...entry.children].sort().map((name) => [name, this.entries.get(join(uri.path, name)).type]);
    }
    readFile(uri) {
        const entry = this.entry(uri);
        if (entry.type !== vscode.FileType.File) {
            throw vscode.FileSystemError.FileIsADirectory(uri);
        }
        return entry.data;
    }
    writeFile(uri, content, options) {
        const parent = this.entry(vscode.Uri.parse(`${uri.scheme}:${dirname(uri.path)}`));
        const existing = this.entries.get(uri.path);
        if (existing && !options.overwrite) {
            throw vscode.FileSystemError.FileExists(uri);
        }
        if (!existing && !options.create) {
            throw vscode.FileSystemError.FileNotFound(uri);
        }
        this.entries.set(uri.path, { type: vscode.FileType.File, data: Uint8Array.from(content), ctime: 1, mtime: Date.now() });
        parent.children.add(basename(uri.path));
        this.fire(existing ? vscode.FileChangeType.Changed : vscode.FileChangeType.Created, uri.path);
    }
    createDirectory(uri) {
        const parent = this.entry(vscode.Uri.parse(`${uri.scheme}:${dirname(uri.path)}`));
        if (this.entries.has(uri.path)) {
            throw vscode.FileSystemError.FileExists(uri);
        }
        this.entries.set(uri.path, { type: vscode.FileType.Directory, children: new Set(), ctime: 1, mtime: 1 });
        parent.children.add(basename(uri.path));
        this.fire(vscode.FileChangeType.Created, uri.path);
    }
    delete(uri) {
        this.entry(uri);
        this.entries.delete(uri.path);
        this.entry(vscode.Uri.parse(`${uri.scheme}:${dirname(uri.path)}`)).children.delete(basename(uri.path));
        this.fire(vscode.FileChangeType.Deleted, uri.path);
    }
    rename(from, to, options) {
        const entry = this.entry(from);
        if (this.entries.has(to.path) && !options.overwrite) {
            throw vscode.FileSystemError.FileExists(to);
        }
        const parent = this.entry(vscode.Uri.parse(`${to.scheme}:${dirname(to.path)}`));
        this.entries.delete(from.path);
        this.entry(vscode.Uri.parse(`${from.scheme}:${dirname(from.path)}`)).children.delete(basename(from.path));
        this.entries.set(to.path, entry);
        parent.children.add(basename(to.path));
        this.fire(vscode.FileChangeType.Deleted, from.path);
        this.fire(vscode.FileChangeType.Created, to.path);
    }
    copy(from, to, options) {
        const entry = this.entry(from);
        if (this.entries.has(to.path) && !options.overwrite) {
            throw vscode.FileSystemError.FileExists(to);
        }
        this.entries.set(to.path, { ...entry });
        this.entry(vscode.Uri.parse(`${to.scheme}:${dirname(to.path)}`)).children.add(basename(to.path));
        this.fire(vscode.FileChangeType.Created, to.path);
    }
}

const dirname = (path) => path.slice(0, path.lastIndexOf('/')) || '/';
const basename = (path) => path.slice(path.lastIndexOf('/') + 1);
const join = (base, name) => (base.endsWith('/') ? base : `${base}/`) + name;

exports.activate = (context) => {
    const providers = { memfs: new MemoryFileSystem(false), memro: new MemoryFileSystem(true) };
    for (const [scheme, provider] of Object.entries(providers)) {
        provider.scheme = scheme;
        context.subscriptions.push(
            vscode.workspace.registerFileSystemProvider(scheme, provider, { isCaseSensitive: true, isReadonly: provider.readonly }),
        );
    }
    providers.memro.entries.set('/fixed.txt', { type: vscode.FileType.File, data: Buffer.from('read only'), ctime: 1, mtime: 1 });
    providers.memro.entries.get('/').children.add('fixed.txt');

    const events = [];
    const watchers = [];
    const outcome = async (run) => {
        try {
            return { ok: (await run()) ?? null };
        } catch (error) {
            return { error: { code: error.code ?? null, name: error.name, message: error.message } };
        }
    };
    const uri = (text) => vscode.Uri.parse(text);
    const command = (id, handler) =>
        context.subscriptions.push(vscode.commands.registerCommand(`anvil.memfs.${id}`, (...args) => outcome(() => handler(...args))));
    const fs = vscode.workspace.fs;

    command('read', async (text) => Buffer.from(await fs.readFile(uri(text))).toString());
    command('write', (text, data) => fs.writeFile(uri(text), Buffer.from(data)));
    command('rename', (from, to) => fs.rename(uri(from), uri(to)));
    command('copy', (from, to) => fs.copy(uri(from), uri(to)));
    command('delete', (text) => fs.delete(uri(text)));
    command('fire', (scheme, kind, path) => providers[scheme].fire(vscode.FileChangeType[kind], path));
    command('watch', (text) => {
        const watcher = vscode.workspace.createFileSystemWatcher(new vscode.RelativePattern(uri(text), '**'));
        const id = watchers.length;
        watchers.push(watcher);
        for (const kind of ['Create', 'Change', 'Delete']) {
            watcher[`onDid${kind}`]((changed) => events.push({ watcher: id, kind: kind.toLowerCase(), uri: changed.toString() }));
        }
        return id;
    });
    command('events', () => events.slice());
    command('watches', (scheme) => providers[scheme].watches.slice());
};
