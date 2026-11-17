// Development probe that plays the editor's role toward the upstream VS Code extension host.
// It listens on a Unix socket, completes the handshake, and logs every RPC request the
// extension host sends. Usage:
//   node probe.mjs --vscode <vscode checkout> [--extension <extension dir>]... [--seconds <n>]
//                  [--dump <file>]   writes the raw bytes received from the extension host
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const args = process.argv.slice(2);
const argValues = (name) => args.flatMap((a, i) => (a === name ? [args[i + 1]] : []));
const vscodeDir = path.resolve(argValues('--vscode')[0] ?? '');
const extensionDirs = argValues('--extension').map((p) => path.resolve(p));
const runSeconds = Number(argValues('--seconds')[0] ?? 8);
const dumpPath = argValues('--dump')[0];
if (dumpPath) fs.writeFileSync(dumpPath, '');
if (!fs.existsSync(path.join(vscodeDir, 'out', 'bootstrap-fork.js'))) {
    console.error('--vscode must point to a transpiled VS Code checkout (run build/next/index.ts transpile)');
    process.exit(2);
}

const FrameType = { Regular: 1, Control: 2, Ack: 3, Disconnect: 5, ReplayRequest: 6, Pause: 7, Resume: 8, KeepAlive: 9 };
const RpcType = {
    RequestJSONArgs: 1, RequestJSONArgsWithCancellation: 2, RequestMixedArgs: 3,
    RequestMixedArgsWithCancellation: 4, Acknowledged: 5, Cancel: 6, ReplyOKEmpty: 7,
    ReplyOKVSBuffer: 8, ReplyOKJSON: 9, ReplyOKJSONWithBuffers: 10, ReplyErrError: 11, ReplyErrEmpty: 12,
};
const rpcTypeName = Object.fromEntries(Object.entries(RpcType).map(([k, v]) => [v, k]));

// Proxy identifiers are numbered in source order across the whole protocol file.
function loadProxyNames() {
    const source = fs.readFileSync(path.join(vscodeDir, 'src/vs/workbench/api/common/extHost.protocol.ts'), 'utf8');
    return [...source.matchAll(/createProxyIdentifier<[^>]*>\('([A-Za-z0-9_]+)'\)/g)].map((m) => m[1]);
}
const proxyNames = loadProxyNames();
const proxyName = (id) => proxyNames[id - 1] ?? `#${id}`;

function frame(type, id, ack, payload = Buffer.alloc(0)) {
    const header = Buffer.alloc(13);
    header.writeUInt8(type, 0);
    header.writeUInt32BE(id, 1);
    header.writeUInt32BE(ack, 5);
    header.writeUInt32BE(payload.length, 9);
    return Buffer.concat([header, payload]);
}

function rpcMessage(type, req, body = Buffer.alloc(0)) {
    const header = Buffer.alloc(5);
    header.writeUInt8(type, 0);
    header.writeUInt32BE(req, 1);
    return Buffer.concat([header, body]);
}

function readLongString(buf, offset) {
    const len = buf.readUInt32BE(offset);
    return [buf.toString('utf8', offset + 4, offset + 4 + len), offset + 4 + len];
}

function decodeRequest(buf) {
    const type = buf.readUInt8(0);
    const req = buf.readUInt32BE(1);
    let offset = 5;
    const rpcId = buf.readUInt8(offset++);
    const methodLen = buf.readUInt8(offset++);
    const method = buf.toString('utf8', offset, offset + methodLen);
    offset += methodLen;
    let argsText;
    if (type === RpcType.RequestJSONArgs || type === RpcType.RequestJSONArgsWithCancellation) {
        [argsText] = readLongString(buf, offset);
    } else {
        const count = buf.readUInt8(offset++);
        const parts = [];
        for (let i = 0; i < count; i++) {
            const kind = buf.readUInt8(offset++);
            if (kind === 1) {
                const [s, next] = readLongString(buf, offset);
                parts.push(s);
                offset = next;
            } else if (kind === 2) {
                const len = buf.readUInt32BE(offset);
                parts.push(`<buffer ${len} bytes>`);
                offset += 4 + len;
            } else if (kind === 3) {
                const bufferCount = buf.readUInt32BE(offset);
                const [s, next] = readLongString(buf, offset + 4);
                parts.push(s);
                offset = next;
                for (let b = 0; b < bufferCount; b++) offset += 4 + buf.readUInt32BE(offset);
            } else {
                parts.push('undefined');
            }
        }
        argsText = `[${parts.join(',')}]`;
    }
    return { type, req, rpcId, method, argsText };
}

// Replies for main-thread calls whose result the extension host dereferences.
const cannedReplies = {
    'MainThreadWindow.$getInitialState': { isFocused: true, isActive: true },
    'MainThreadChatSkills.$getTools': [],
};

const uri = (p) => ({ $mid: 1, scheme: 'file', path: p });
const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'exthost-probe-'));

// The extension host compares identifiers by the precomputed `_lower` field.
const extensionIdentifier = (value) => ({ value, _lower: value.toLowerCase() });

function buildInitData() {
    const allExtensions = extensionDirs.map((dir) => {
        const manifest = JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8'));
        return {
            ...manifest,
            identifier: extensionIdentifier(`${manifest.publisher}.${manifest.name}`),
            extensionLocation: uri(dir),
            isBuiltin: false,
            isUserBuiltin: false,
            isUnderDevelopment: false,
            targetPlatform: 'linux-x64',
        };
    });
    const ids = allExtensions.map((e) => e.identifier);
    return {
        version: '1.146.0',
        quality: undefined,
        parentPid: process.pid,
        environment: {
            isExtensionDevelopmentDebug: false,
            appName: 'Anvil',
            appHost: 'desktop',
            appRoot: uri(vscodeDir),
            appLanguage: 'en',
            isExtensionTelemetryLoggingOnly: true,
            appUriScheme: 'anvil',
            globalStorageHome: uri(path.join(scratch, 'globalStorage')),
            workspaceStorageHome: uri(path.join(scratch, 'workspaceStorage')),
        },
        workspace: null,
        extensions: {
            versionId: 1,
            allExtensions,
            activationEvents: Object.fromEntries(allExtensions.map((e) => [e.identifier.value, e.activationEvents ?? []])),
            myExtensions: ids,
        },
        telemetryInfo: { sessionId: 'probe', machineId: 'probe', sqmId: '', devDeviceId: '', firstSessionDate: new Date().toISOString() },
        logLevel: 2,
        loggers: [],
        logsLocation: uri(path.join(scratch, 'logs')),
        autoStart: true,
        remote: { isRemote: false, authority: undefined, connectionData: null },
        consoleForward: { includeStack: false, logNative: false },
        uiKind: 1,
    };
}

const socketPath = path.join(scratch, 'exthost.sock');
let hostPid;

function reportResidentMemory() {
    try {
        const status = fs.readFileSync(`/proc/${hostPid}/status`, 'utf8');
        console.log(`[probe] extension host ${status.match(/VmRSS:\s+(\d+) kB/)?.[0]}`);
    } catch {
        // /proc is Linux-only; the measurement is informational.
    }
}
let outgoingId = 0;
let incomingId = 0;
let handshakeState = 'waiting-ready';
let rpcBuffer = Buffer.alloc(0);

const server = net.createServer((socket) => {
    console.log('[probe] extension host connected');
    const send = (payload) => socket.write(frame(FrameType.Regular, ++outgoingId, incomingId, payload));
    const pending = new Map();
    let nextRequest = 1;

    function call(service, method, ...callArgs) {
        const rpcId = proxyNames.indexOf(service) + 1;
        if (rpcId === 0) throw new Error(`unknown proxy ${service}`);
        const req = nextRequest++;
        const methodBytes = Buffer.from(method);
        const argsBytes = Buffer.from(JSON.stringify(callArgs));
        const body = Buffer.concat([
            Buffer.from([rpcId, methodBytes.length]), methodBytes,
            Buffer.alloc(4), argsBytes,
        ]);
        body.writeUInt32BE(argsBytes.length, 2 + methodBytes.length);
        console.log(`[rpc] -> ${service}.${method} req=${req} ${JSON.stringify(callArgs)}`);
        send(rpcMessage(RpcType.RequestJSONArgs, req, body));
        return new Promise((resolve) => pending.set(req, resolve));
    }

    async function afterHandshake() {
        const emptyModel = { contents: {}, keys: [], overrides: [] };
        await call('ExtHostConfiguration', '$initializeConfiguration', {
            defaults: emptyModel, policy: emptyModel, application: emptyModel, userLocal: emptyModel,
            userRemote: emptyModel, workspace: emptyModel, folders: [], configurationScopes: [],
        });
        await call('ExtHostWorkspace', '$initializeWorkspace', null, true);
        await call('ExtHostExtensionService', '$activateByEvent', '*', 0);
        await call('ExtHostCommands', '$executeContributedCommand', 'anvil.hello', 'anvil');
        reportResidentMemory();
    }

    function onPayload(payload) {
        if (handshakeState === 'waiting-ready') {
            if (payload.length === 1 && payload[0] === 2) {
                console.log('[probe] <- Ready; -> init data');
                send(Buffer.from(JSON.stringify(buildInitData())));
                handshakeState = 'waiting-initialized';
            }
            return;
        }
        if (handshakeState === 'waiting-initialized') {
            if (payload.length === 1 && payload[0] === 1) {
                console.log('[probe] <- Initialized');
                handshakeState = 'rpc';
                if (extensionDirs.length > 0) void afterHandshake();
            }
            return;
        }
        const type = payload.readUInt8(0);
        const req = payload.readUInt32BE(1);
        if (type >= RpcType.ReplyOKEmpty) {
            const waiter = pending.get(req);
            pending.delete(req);
            const body = (type === RpcType.ReplyOKJSON || type === RpcType.ReplyErrError) ? readLongString(payload, 5)[0] : type === RpcType.ReplyOKEmpty ? 'undefined' : `<${rpcTypeName[type]}>`;
            console.log(`[rpc] <- reply req=${req} ${body}`);
            waiter?.(body);
            return;
        }
        if (type >= RpcType.RequestJSONArgs && type <= RpcType.RequestMixedArgsWithCancellation) {
            const r = decodeRequest(payload);
            const shown = r.argsText.length > 200 ? `${r.argsText.slice(0, 200)}...` : r.argsText;
            console.log(`[rpc] <- ${proxyName(r.rpcId)}.${r.method} req=${r.req} ${shown}`);
            send(rpcMessage(RpcType.Acknowledged, req));
            const key = `${proxyName(r.rpcId)}.${r.method}`;
            if (key in cannedReplies) {
                const json = Buffer.from(JSON.stringify(cannedReplies[key]));
                const lengthPrefix = Buffer.alloc(4);
                lengthPrefix.writeUInt32BE(json.length);
                send(rpcMessage(RpcType.ReplyOKJSON, req, Buffer.concat([lengthPrefix, json])));
            } else {
                send(rpcMessage(RpcType.ReplyOKEmpty, req));
            }
        } else {
            console.log(`[rpc] <- ${rpcTypeName[type] ?? type} req=${req}`);
        }
    }

    socket.on('data', (chunk) => {
        if (dumpPath) fs.appendFileSync(dumpPath, chunk);
        rpcBuffer = Buffer.concat([rpcBuffer, chunk]);
        let sawRegular = false;
        while (rpcBuffer.length >= 13) {
            const type = rpcBuffer.readUInt8(0);
            const id = rpcBuffer.readUInt32BE(1);
            const length = rpcBuffer.readUInt32BE(9);
            if (rpcBuffer.length < 13 + length) break;
            const payload = rpcBuffer.subarray(13, 13 + length);
            rpcBuffer = rpcBuffer.subarray(13 + length);
            if (type === FrameType.Regular) {
                incomingId = id;
                sawRegular = true;
                onPayload(payload);
            } else if (type !== FrameType.Ack && type !== FrameType.KeepAlive && type !== FrameType.Resume) {
                console.log(`[frame] <- type ${type}`);
            }
        }
        if (sawRegular) socket.write(frame(FrameType.Ack, 0, incomingId));
    });
    socket.on('close', () => console.log('[probe] socket closed'));
});

server.listen(socketPath, () => {
    // Inherited Electron and VS Code variables switch the extension host into Electron-only code paths.
    const inherited = Object.fromEntries(
        Object.entries(process.env).filter(([k]) => k !== 'ELECTRON_RUN_AS_NODE' && !k.startsWith('VSCODE_')),
    );
    const env = {
        ...inherited,
        VSCODE_ESM_ENTRYPOINT: 'vs/workbench/api/node/extensionHostProcess',
        VSCODE_EXTHOST_IPC_HOOK: socketPath,
        VSCODE_HANDLES_UNCAUGHT_ERRORS: 'true',
    };
    const child = spawn(process.execPath, [path.join(vscodeDir, 'out/bootstrap-fork.js'), '--type=extensionHost'], {
        cwd: vscodeDir,
        env,
        stdio: ['ignore', 'inherit', 'inherit'],
    });
    hostPid = child.pid;
    child.on('exit', (code, signal) => {
        console.log(`[probe] extension host exited code=${code} signal=${signal}`);
        process.exit(0);
    });
    setTimeout(() => {
        console.log('[probe] time limit reached; terminating');
        child.kill();
    }, runSeconds * 1000);
});

process.on('exit', () => fs.rmSync(scratch, { recursive: true, force: true }));
