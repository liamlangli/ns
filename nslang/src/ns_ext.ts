import * as vscode from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from "vscode-languageclient/node";
import path from "path";

let client: LanguageClient;

function start_lsp_client(context: vscode.ExtensionContext) {
  const server_module = context.asAbsolutePath("dist/ns_lsp.js");
  const server_options: ServerOptions = {
    run: { module: server_module, transport: TransportKind.ipc },
    debug: { module: server_module, transport: TransportKind.ipc },
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

const NSDB_DEBUG_TYPE = "nsdb";

const DEFAULT_CONFIG: vscode.DebugConfiguration = {
  type: NSDB_DEBUG_TYPE,
  request: "launch",
  name: "Debug NS Program"
};

class NSDebugConfigurationProvider implements vscode.DebugConfigurationProvider {
  resolveDebugConfiguration(folder: vscode.WorkspaceFolder | undefined, config: vscode.DebugConfiguration, token?: vscode.CancellationToken): vscode.ProviderResult<vscode.DebugConfiguration> {
    if (!config.type && !config.request && !config.name) {
      return DEFAULT_CONFIG;
    }
    return config;
  }

  resolveDebugConfigurationWithSubstitutedVariables(folder: vscode.WorkspaceFolder | undefined, debugConfiguration: vscode.DebugConfiguration, token?: vscode.CancellationToken): vscode.ProviderResult<vscode.DebugConfiguration> {
    return Object.assign({}, DEFAULT_CONFIG, debugConfiguration);
  }
}

class NSDebugAdapterDescriptorFactory implements vscode.DebugAdapterDescriptorFactory {
  createDebugAdapterDescriptor(session: vscode.DebugSession, executable: vscode.DebugAdapterExecutable | undefined): vscode.ProviderResult<vscode.DebugAdapterDescriptor> {
    if (executable == null) {
      executable = new vscode.DebugAdapterExecutable('node', ['/Users/bytedance/os/ns/nslang/dist/ns_db.js'], {});
    }
    return executable;
  }
}

function start_dap_client(context: vscode.ExtensionContext) {
  context.subscriptions.push(
    vscode.debug.registerDebugConfigurationProvider(NSDB_DEBUG_TYPE, new NSDebugConfigurationProvider())
  );

  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory(NSDB_DEBUG_TYPE, new NSDebugAdapterDescriptorFactory())
  );
}

export function activate(context: vscode.ExtensionContext) {
  start_dap_client(context);
  start_lsp_client(context);
}

export function deactivate(): Thenable<void> | undefined {
  return client ? client.stop() : undefined;
}
