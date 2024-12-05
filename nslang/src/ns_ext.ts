import * as vscode from "vscode";
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient;

function start_lsp_client(context: vscode.ExtensionContext) {
    const server_options: ServerOptions = {
        run: { command: "ns_lsp", transport: { kind: TransportKind.socket, port: 5000 } },
        debug: { command: "ns_lsp", transport: { kind: TransportKind.socket, port: 5000 } },
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
    runtimeExecutable: "ns_dap",
    program: "${file}",
};

class NSDebugConfigurationProvider implements vscode.DebugConfigurationProvider {
    resolveDebugConfiguration(
        folder: vscode.WorkspaceFolder | undefined,
        config: vscode.DebugConfiguration
    ): vscode.ProviderResult<vscode.DebugConfiguration> {
        return !config.type && !config.request && !config.name ? DEFAULT_CONFIG : config;
    }

    resolveDebugConfigurationWithSubstitutedVariables(
        folder: vscode.WorkspaceFolder | undefined,
        debugConfiguration: vscode.DebugConfiguration
    ): vscode.ProviderResult<vscode.DebugConfiguration> {
        return { ...DEFAULT_CONFIG, ...debugConfiguration };
    }
}

class NSDebugAdapterDescriptorFactory implements vscode.DebugAdapterDescriptorFactory {
    createDebugAdapterDescriptor(
        session: vscode.DebugSession,
        executable: vscode.DebugAdapterExecutable | undefined
    ): vscode.ProviderResult<vscode.DebugAdapterDescriptor> {
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
    start_dap_client(context);
    start_lsp_client(context);
}

export function deactivate(): Thenable<void> | undefined {
    return client?.stop();
}
