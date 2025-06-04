import * as net from "net";
import * as vscode from "vscode";
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    StreamInfo,
    TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient;
let log: vscode.OutputChannel;

function start_lsp_client(context: vscode.ExtensionContext) {
    // get nanoscript configuration
    const ns_config = vscode.workspace.getConfiguration("ns");
    const mode = ns_config.get<"stdio" | "socket">("lsp.mode") ?? "socket";
    const port = ns_config.get<number>("lsp.port") ?? 9000;

    let server_options: ServerOptions;
    if (mode === "stdio") {
        server_options = {
            run: { command: "ns_lsp", transport: TransportKind.stdio },
            debug: { command: "ns_lsp", transport: TransportKind.stdio },
        }
    } else {
        server_options = (): Promise<StreamInfo> => {
            return new Promise((resolve, reject) => {
                const socket = net.connect({port}, () => {
                    log?.appendLine("[ns_lsp] connected to server");
                    resolve({
                        reader: socket,
                        writer: socket,
                    });
                });
                socket.on('error', (err) => {
                    log?.appendLine(`[ns_lsp] connection error: ${err.message}`);
                    reject(err)
                });
            });
        }
    }

    const client_options: LanguageClientOptions = {
        documentSelector: [{ scheme: "file", language: "ns" }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher("**/*.ns"),
        },
    };

    client = new LanguageClient("ns_lsp", "ns_lsp", server_options, client_options);
    client.start();
}

const DEFAULT_CONFIG: vscode.DebugConfiguration = {
    type: "ns_debug",
    request: "launch",
    name: "Debug NS Program"
};

export interface NSDebugConfiguration extends vscode.DebugConfiguration {
    mode: "stdio" | "socket";
    port: number;
}

class NSDebugConfigurationProvider implements vscode.DebugConfigurationProvider {
    resolveDebugConfiguration(
        _: vscode.WorkspaceFolder | undefined,
        config: vscode.DebugConfiguration
    ): vscode.ProviderResult<vscode.DebugConfiguration> {
        return !config.type && !config.request && !config.name ? DEFAULT_CONFIG : config;
    }

    resolveDebugConfigurationWithSubstitutedVariables(
        _: vscode.WorkspaceFolder | undefined,
        debugConfiguration: vscode.DebugConfiguration
    ): vscode.ProviderResult<vscode.DebugConfiguration> {
        return { ...DEFAULT_CONFIG, ...debugConfiguration };
    }
}

class NSDebugAdapterDescriptorFactory implements vscode.DebugAdapterDescriptorFactory {
    createDebugAdapterDescriptor(
        sess: vscode.DebugSession,
        executable: vscode.DebugAdapterExecutable | undefined
    ): vscode.ProviderResult<vscode.DebugAdapterDescriptor> {
        log?.appendLine("[ns_debug] launching debug adapter");
        if (executable) return executable;
        const config = vscode.workspace.getConfiguration("ns");
        const mode = config.get<"stdio" | "socket">("debugger.mode");
        const port = config.get<number>("debugger.port");
        if (mode === "socket") {
            return new vscode.DebugAdapterServer(port);
        } else  {
            return new vscode.DebugAdapterExecutable("ns_debug", ["--stdio"]);
        }
    }
}

function start_dap_client(context: vscode.ExtensionContext) {
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider("ns_debug", new NSDebugConfigurationProvider()),
        vscode.debug.registerDebugAdapterDescriptorFactory("ns_debug", new NSDebugAdapterDescriptorFactory())
    );
}

export function activate(context: vscode.ExtensionContext) {
    log = vscode.window.createOutputChannel("nslang");
    start_dap_client(context);
    log.appendLine("[nslang] extension activated");
    start_lsp_client(context);
    log.appendLine("[nslang] lsp client started");
}

export function deactivate(): Thenable<void> | undefined {
    return client?.stop();
}
