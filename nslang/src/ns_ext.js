const net = require("net");
const vscode = require("vscode");
const fs = require("fs");
const path = require("path");
const { spawn } = require("child_process");
const { LanguageClient } = require("vscode-languageclient/node");

let single_client;
let single_server;
let single_port;
const workspace_clients = new Map();
const workspace_servers = new Map();
const workspace_ports = new Map();
let next_port_offset = 0;
let log;
let lsp_command;
let lsp_transport = "socket";
const SERVER_READY_TIMEOUT_MS = 8000;
const SERVER_START_MAX_ATTEMPTS = 20;

function strip_ansi(s) {
    if (!s) return "";
    return s.replace(/\x1B\[[0-9;]*[A-Za-z]/g, "");
}

function stop_workspace_client(folder_uri) {
    const c = workspace_clients.get(folder_uri);
    if (!c) return;
    if (log) log.appendLine(`[ns_lsp] stop client workspace=${folder_uri} reason=workspace-removed`);
    workspace_clients.delete(folder_uri);
    void c.stop();
}

function stop_workspace_server(folder_uri) {
    const s = workspace_servers.get(folder_uri);
    if (!s) return;
    if (log) log.appendLine(`[ns_lsp] stop server workspace=${folder_uri} reason=workspace-removed`);
    workspace_servers.delete(folder_uri);
    workspace_ports.delete(folder_uri);
    try {
        s.kill();
    } catch {
        // ignore
    }
}

function stop_all_lsp_clients() {
    if (log) log.appendLine("[ns_lsp] stop all reason=extension-deactivate");
    const stops = [];
    for (const [, c] of workspace_clients) {
        stops.push(c.stop());
    }
    workspace_clients.clear();
    for (const [, s] of workspace_servers) {
        try {
            s.kill();
        } catch {
            // ignore
        }
    }
    workspace_servers.clear();
    workspace_ports.clear();
    if (single_client) {
        stops.push(single_client.stop());
        single_client = undefined;
    }
    if (single_server) {
        if (log) log.appendLine("[ns_lsp] stop server scope=single reason=extension-deactivate");
        try {
            single_server.kill();
        } catch {
            // ignore
        }
        single_server = undefined;
        single_port = undefined;
    }
    return Promise.all(stops).then(() => undefined);
}

function is_windows() {
    return process.platform === "win32";
}

function expand_home(p) {
    if (!p) return p;
    if (p.startsWith("~/") || p === "~") {
        const home = process.env.HOME || process.env.USERPROFILE;
        if (!home) return p;
        return p === "~" ? home : path.join(home, p.slice(2));
    }
    return p;
}

function executable_exists(p) {
    if (!p) return false;
    try {
        fs.accessSync(p, fs.constants.X_OK);
        return true;
    } catch {
        try {
            fs.accessSync(p, fs.constants.F_OK);
            return true;
        } catch {
            return false;
        }
    }
}

function command_in_path(cmd) {
    const path_env = process.env.PATH || "";
    const parts = path_env.split(path.delimiter);
    const names = is_windows() ? [cmd, `${cmd}.exe`] : [cmd];
    for (const dir of parts) {
        if (!dir) continue;
        for (const n of names) {
            const full = path.join(dir, n);
            if (executable_exists(full)) return full;
        }
    }
    return null;
}

function resolve_lsp_command(context) {
    const config = vscode.workspace.getConfiguration("ns");
    const configured = config.get("lsp.serverPath");
    if (configured && typeof configured === "string" && configured.trim().length > 0) {
        const p = expand_home(configured.trim());
        if (executable_exists(p)) return p;
        if (log) log.appendLine(`[ns_lsp] configured serverPath not found: ${p}`);
    }

    const exe_name = is_windows() ? "ns_lsp.exe" : "ns_lsp";
    const candidates = [];

    if (vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders.length > 0) {
        for (const f of vscode.workspace.workspaceFolders) {
            candidates.push(path.join(f.uri.fsPath, "bin", exe_name));
        }
    }

    if (context && context.extensionPath) {
        candidates.push(path.join(context.extensionPath, "..", "bin", exe_name));
        candidates.push(path.join(context.extensionPath, "bin", exe_name));
    }

    const home = process.env.HOME || process.env.USERPROFILE;
    if (home) {
        candidates.push(path.join(home, ".cache", "ns", "bin", exe_name));
    }

    for (const c of candidates) {
        if (executable_exists(c)) return c;
    }

    const in_path = command_in_path("ns_lsp");
    return in_path || exe_name;
}

function lsp_socket_server_options(port) {
    return () =>
        new Promise((resolve, reject) => {
            let attempts = 0;
            const max_attempts = 40;
            const retry_ms = 150;

            const try_connect = () => {
                attempts += 1;
                const socket = net.connect({ port }, () => {
                    if (log) log.appendLine(`[ns_lsp] connected on port ${port}`);
                    resolve({ reader: socket, writer: socket });
                });
                socket.on("error", (err) => {
                    socket.destroy();
                    if (attempts >= max_attempts) {
                        if (log) log.appendLine(`[ns_lsp] connection error on port ${port}: ${err.message}`);
                        reject(err);
                        return;
                    }
                    setTimeout(try_connect, retry_ms);
                });
            };

            try_connect();
        });
}

function lsp_stdio_server_options(cwd) {
    return {
        command: lsp_command,
        args: ["--stdio"],
        options: cwd ? { cwd } : undefined,
    };
}

function wait_for_port_ready(port, timeout_ms) {
    return new Promise((resolve, reject) => {
        const start = Date.now();
        const tick = () => {
            const socket = net.connect({ port }, () => {
                socket.end();
                resolve();
            });
            socket.on("error", () => {
                socket.destroy();
                if (Date.now() - start >= timeout_ms) {
                    reject(new Error(`timeout waiting for ns_lsp on port ${port}`));
                    return;
                }
                setTimeout(tick, 100);
            });
        };
        tick();
    });
}

function wait_for_spawn_or_exit(proc, port, timeout_ms) {
    return new Promise((resolve, reject) => {
        let settled = false;
        const finish_ok = () => {
            if (settled) return;
            settled = true;
            cleanup();
            resolve();
        };
        const finish_err = (err) => {
            if (settled) return;
            settled = true;
            cleanup();
            reject(err);
        };
        const on_error = (err) => finish_err(err);
        const on_exit = (code, signal) => {
            finish_err(new Error(`ns_lsp exited before ready (code=${code ?? "null"} signal=${signal ?? "null"} port=${port})`));
        };
        const cleanup = () => {
            proc.off("error", on_error);
            proc.off("exit", on_exit);
        };
        proc.once("error", on_error);
        proc.once("exit", on_exit);
        wait_for_port_ready(port, timeout_ms).then(finish_ok).catch(finish_err);
    });
}

function lsp_client_options(folder) {
    if (folder) {
        const root = folder.uri.fsPath.replace(/\\/g, "/");
        const pattern = `${root}/**/*.ns`;
        return {
            documentSelector: [{ scheme: "file", language: "ns", pattern }],
            synchronize: {
                fileEvents: vscode.workspace.createFileSystemWatcher(new vscode.RelativePattern(folder, "**/*.ns")),
            },
            workspaceFolder: folder,
        };
    }

    return {
        documentSelector: [{ scheme: "file", language: "ns" }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher("**/*.ns"),
        },
    };
}

function start_lsp_server_process(port, cwd) {
    const args = ["--socket", "--port", String(port)];
    const proc = spawn(lsp_command, args, {
        cwd,
        stdio: ["ignore", "pipe", "pipe"],
    });

    if (proc.stdout) {
        proc.stdout.on("data", (chunk) => {
            const text = strip_ansi(chunk.toString()).trimEnd();
            if (log && text.length > 0) log.appendLine(`[ns_lsp:${port}] ${text}`);
        });
    }
    if (proc.stderr) {
        proc.stderr.on("data", (chunk) => {
            const text = strip_ansi(chunk.toString()).trimEnd();
            if (log && text.length > 0) log.appendLine(`[ns_lsp:${port}:err] ${text}`);
        });
    }
    proc.on("exit", (code, signal) => {
        if (log) log.appendLine(`[ns_lsp:${port}] exited code=${code ?? "null"} signal=${signal ?? "null"}`);
    });
    proc.on("error", (err) => {
        if (log) log.appendLine(`[ns_lsp:${port}] failed to start: ${err.message}`);
    });
    return proc;
}

function next_workspace_port(base_port) {
    const p = base_port + next_port_offset;
    next_port_offset += 1;
    return p;
}

function attach_client_state_log(client, label) {
    client.onDidChangeState((e) => {
        if (log) log.appendLine(`[ns_lsp] client(${label}) state ${e.oldState} -> ${e.newState}`);
    });
}

function resolve_lsp_transport() {
    const config = vscode.workspace.getConfiguration("ns");
    const mode = config.get("lsp.transport") ?? "auto";
    if (mode === "socket") return "socket";
    if (mode === "stdio") return "stdio";
    return vscode.env.remoteName ? "socket" : "stdio";
}

async function try_start_server_with_port_retry(base_port, cwd, label) {
    let last_err;
    for (let i = 0; i < SERVER_START_MAX_ATTEMPTS; i += 1) {
        const port = next_workspace_port(base_port);
        const server = start_lsp_server_process(port, cwd);
        try {
            await wait_for_spawn_or_exit(server, port, SERVER_READY_TIMEOUT_MS);
            return { server, port };
        } catch (err) {
            last_err = err;
            if (log) log.appendLine(`[ns_lsp] start failed label=${label} port=${port}: ${err.message}`);
            try {
                server.kill();
            } catch {
                // ignore
            }
        }
    }
    throw new Error(`failed to start ns_lsp for ${label} after ${SERVER_START_MAX_ATTEMPTS} ports (${last_err ? last_err.message : "unknown error"})`);
}

async function start_single_lsp_client(port) {
    if (single_client) return true;
    if (!single_server) {
        const cwd = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
        try {
            const started = await try_start_server_with_port_retry(port, cwd, "single");
            single_server = started.server;
            single_port = started.port;
        } catch (err) {
            if (log) log.appendLine(`[ns_lsp] single server failed before ready: ${err.message}`);
            single_server = undefined;
            void vscode.window.showErrorMessage(`nslang: failed to start ns_lsp (${err.message})`);
            return false;
        }
    }
    if (!single_server || single_server.killed) return false;
    const server_options = lsp_socket_server_options(single_port ?? port);
    const client_options = lsp_client_options();
    single_client = new LanguageClient("ns_lsp", "ns_lsp", server_options, client_options);
    attach_client_state_log(single_client, "single");
    single_client.start();
    return true;
}

async function start_workspace_lsp_clients(context, base_port) {
    let started_count = 0;
    const folders = vscode.workspace.workspaceFolders ?? [];
    for (const folder of folders) {
        const key = folder.uri.toString();
        if (workspace_clients.has(key)) continue;
        try {
            const started = await try_start_server_with_port_retry(base_port, folder.uri.fsPath, folder.name);
            workspace_ports.set(key, started.port);
            workspace_servers.set(key, started.server);
            const server_options = lsp_socket_server_options(started.port);
            const client_options = lsp_client_options(folder);
            const id = `ns_lsp_${folder.name}`;
            const c = new LanguageClient(id, `ns_lsp (${folder.name})`, server_options, client_options);
            attach_client_state_log(c, folder.name);
            workspace_clients.set(key, c);
            c.start();
            started_count += 1;
        } catch (err) {
            if (log) log.appendLine(`[ns_lsp] workspace server (${folder.name}) failed before ready: ${err.message}`);
            workspace_servers.delete(key);
            workspace_ports.delete(key);
            void vscode.window.showErrorMessage(`nslang: failed to start ns_lsp for ${folder.name} (${err.message})`);
        }
    }

    context.subscriptions.push(
        vscode.workspace.onDidChangeWorkspaceFolders((evt) => {
            for (const removed of evt.removed) {
                const key = removed.uri.toString();
                stop_workspace_client(key);
                stop_workspace_server(key);
            }
            for (const added of evt.added) {
                const key = added.uri.toString();
                if (workspace_clients.has(key)) continue;
                try_start_server_with_port_retry(base_port, added.uri.fsPath, added.name)
                    .then((started) => {
                        workspace_ports.set(key, started.port);
                        workspace_servers.set(key, started.server);
                        const server_options = lsp_socket_server_options(started.port);
                        const client_options = lsp_client_options(added);
                        const id = `ns_lsp_${added.name}`;
                        const c = new LanguageClient(id, `ns_lsp (${added.name})`, server_options, client_options);
                        attach_client_state_log(c, added.name);
                        workspace_clients.set(key, c);
                        c.start();
                    })
                    .catch((err) => {
                        if (log) log.appendLine(`[ns_lsp] workspace server (${added.name}) failed before ready: ${err.message}`);
                        workspace_servers.delete(key);
                        workspace_ports.delete(key);
                        void vscode.window.showErrorMessage(`nslang: failed to start ns_lsp for ${added.name} (${err.message})`);
                    });
            }
        }),
    );
    return started_count;
}

function start_single_lsp_client_stdio() {
    if (single_client) return true;
    const cwd = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
    const server_options = lsp_stdio_server_options(cwd);
    const client_options = lsp_client_options();
    single_client = new LanguageClient("ns_lsp", "ns_lsp", server_options, client_options);
    attach_client_state_log(single_client, "single");
    single_client.start();
    return true;
}

function start_workspace_lsp_clients_stdio(context) {
    let started_count = 0;
    const folders = vscode.workspace.workspaceFolders ?? [];
    for (const folder of folders) {
        const key = folder.uri.toString();
        if (workspace_clients.has(key)) continue;
        const server_options = lsp_stdio_server_options(folder.uri.fsPath);
        const client_options = lsp_client_options(folder);
        const id = `ns_lsp_${folder.name}`;
        const c = new LanguageClient(id, `ns_lsp (${folder.name})`, server_options, client_options);
        attach_client_state_log(c, folder.name);
        workspace_clients.set(key, c);
        c.start();
        started_count += 1;
    }

    context.subscriptions.push(
        vscode.workspace.onDidChangeWorkspaceFolders((evt) => {
            for (const removed of evt.removed) {
                const key = removed.uri.toString();
                stop_workspace_client(key);
            }
            for (const added of evt.added) {
                const key = added.uri.toString();
                if (workspace_clients.has(key)) continue;
                const server_options = lsp_stdio_server_options(added.uri.fsPath);
                const client_options = lsp_client_options(added);
                const id = `ns_lsp_${added.name}`;
                const c = new LanguageClient(id, `ns_lsp (${added.name})`, server_options, client_options);
                attach_client_state_log(c, added.name);
                workspace_clients.set(key, c);
                c.start();
            }
        }),
    );
    return started_count;
}

async function start_lsp_client(context) {
    const ns_config = vscode.workspace.getConfiguration("ns");
    const port = ns_config.get("lsp.port") ?? 5000;
    const workspace_mode = ns_config.get("lsp.workspaceMode") ?? "perWorkspace";
    lsp_transport = resolve_lsp_transport();
    if (log) log.appendLine(`[ns_lsp] transport=${lsp_transport}`);

    if (lsp_transport === "stdio") {
        if (workspace_mode === "single") {
            return start_single_lsp_client_stdio();
        }
        return start_workspace_lsp_clients_stdio(context) > 0;
    }

    if (workspace_mode === "single") {
        return await start_single_lsp_client(port);
    }
    return (await start_workspace_lsp_clients(context, port)) > 0;
}

const DEFAULT_CONFIG = {
    type: "ns_debug",
    request: "launch",
    name: "Debug NS Program",
};

class NSDebugConfigurationProvider {
    resolveDebugConfiguration(_, config) {
        return !config.type && !config.request && !config.name ? DEFAULT_CONFIG : config;
    }

    resolveDebugConfigurationWithSubstitutedVariables(_, debugConfiguration) {
        return { ...DEFAULT_CONFIG, ...debugConfiguration };
    }
}

class NSDebugAdapterDescriptorFactory {
    createDebugAdapterDescriptor(_sess, executable) {
        if (log) log.appendLine("[ns_debug] launching debug adapter");
        if (executable) return executable;
        const config = vscode.workspace.getConfiguration("ns");
        const mode = config.get("debugger.mode");
        const port = config.get("debugger.port");
        if (mode === "socket") {
            return new vscode.DebugAdapterServer(port);
        }
        return new vscode.DebugAdapterExecutable("ns_debug", ["--stdio"]);
    }
}

function start_dap_client(context) {
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider("ns_debug", new NSDebugConfigurationProvider()),
        vscode.debug.registerDebugAdapterDescriptorFactory("ns_debug", new NSDebugAdapterDescriptorFactory()),
    );
}

function activate(context) {
    log = vscode.window.createOutputChannel("nslang");
    lsp_command = resolve_lsp_command(context);
    if (log) log.appendLine(`[ns_lsp] server command: ${lsp_command}`);
    start_dap_client(context);
    log.appendLine("[nslang] extension activated");
    context.subscriptions.push({
        dispose() {
            if (log) log.appendLine("[nslang] extension disposed");
        },
    });
    context.subscriptions.push(
        vscode.workspace.onDidChangeWorkspaceFolders((evt) => {
            if (log) log.appendLine(`[nslang] workspace folders changed removed=${evt.removed.length} added=${evt.added.length}`);
        }),
    );
    void start_lsp_client(context)
        .then((started) => {
            if (!started) {
                throw new Error("no LSP client started");
            }
            log.appendLine("[nslang] lsp client started");
        })
        .catch((err) => {
            log.appendLine(`[nslang] lsp startup failed: ${err.message}`);
            void vscode.window.showErrorMessage(`nslang: LSP startup failed (${err.message})`);
        });
}

function deactivate() {
    if (log) log.appendLine("[nslang] deactivate called");
    return stop_all_lsp_clients();
}

module.exports = {
    activate,
    deactivate,
};
