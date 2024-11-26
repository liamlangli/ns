import * as vscode from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from "vscode-languageclient/node";
import { NSDebugger } from "./nsdb";

let client: LanguageClient;

function start_lsp_client(context: vscode.ExtensionContext) {
  const server_module = context.asAbsolutePath("out/server.js");
  const server_options: ServerOptions = {
    run: { module: server_module, transport: TransportKind.ipc },
    debug: { module: server_module, transport: TransportKind.ipc },
  };

  const client_options: LanguageClientOptions = {
    documentSelector: [{ scheme: "file", language: "ns" }],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher("**/.clientrc"),
    },
  };

  client = new LanguageClient("ns", "ns", server_options, client_options);
  client.start();
}

function start_dap_client(context: vscode.ExtensionContext) {
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory("nsdb", {
      createDebugAdapterDescriptor: (session) => {
        return new vscode.DebugAdapterInlineImplementation(new NSDebugger());
      },
    })
  );
}

export function activate(context: vscode.ExtensionContext) {
  start_dap_client(context);
  start_lsp_client(context);
}

export function deactivate(): Thenable<void> | undefined {
  return client ? client.stop() : undefined;
}
