import * as vscode from "vscode";
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient;
let log: vscode.OutputChannel;

function start_lsp_client(context: vscode.ExtensionContext) {
    const server_options: ServerOptions = {
        run: { command: "ns_lsp", transport: TransportKind.stdio },
        debug: { command: "ns_lsp", transport: TransportKind.stdio },
    };

    const client_options: LanguageClientOptions = {
        documentSelector: [{ scheme: "file", language: "ns" }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher("**/*.ns"),
        },
    };

    client = new LanguageClient("ns_lsp", "ns_lsp", server_options, client_options);
    client.start();
    console.log("ns_lsp client started");
}

const DEFAULT_CONFIG: vscode.DebugConfiguration = {
    type: "nsdb",
    request: "launch",
    name: "Debug NS Program",
    program: "${file}",
};

class NSDebugConfigurationProvider implements vscode.DebugConfigurationProvider {
    resolveDebugConfiguration(
        folder: vscode.WorkspaceFolder | undefined,
        config: vscode.DebugConfiguration
    ): vscode.ProviderResult<vscode.DebugConfiguration> {
        log?.appendLine("resolveDebugConfiguration");
        return !config.type && !config.request && !config.name ? DEFAULT_CONFIG : config;
    }

    resolveDebugConfigurationWithSubstitutedVariables(
        folder: vscode.WorkspaceFolder | undefined,
        debugConfiguration: vscode.DebugConfiguration
    ): vscode.ProviderResult<vscode.DebugConfiguration> {
        log?.appendLine("resolveDebugConfigurationWithSubstitutedVariables");
        return { ...DEFAULT_CONFIG, ...debugConfiguration };
    }
}

class NSDebugAdapterDescriptorFactory implements vscode.DebugAdapterDescriptorFactory {
    createDebugAdapterDescriptor(
        session: vscode.DebugSession,
        executable: vscode.DebugAdapterExecutable | undefined
    ): vscode.ProviderResult<vscode.DebugAdapterDescriptor> {
        log?.appendLine("createDebugAdapterDescriptor");
        return executable ?? new vscode.DebugAdapterServer(5001);
    }
}

function start_dap_client(context: vscode.ExtensionContext) {
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider("nsdb", new NSDebugConfigurationProvider()),
        vscode.debug.registerDebugAdapterDescriptorFactory("nsdb", new NSDebugAdapterDescriptorFactory())
    );
}

export function activate(context: vscode.ExtensionContext) {
    log = vscode.window.createOutputChannel("nslang");
    start_dap_client(context);
    log.appendLine("nslang activated");
    start_lsp_client(context);
    log.appendLine("nslang lsp client started");
}

export function deactivate(): Thenable<void> | undefined {
    return client?.stop();
}
