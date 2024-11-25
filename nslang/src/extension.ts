import * as vscode from 'vscode';
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from 'vscode-languageclient/node';

let client: LanguageClient;

export function activate(context: vscode.ExtensionContext) {
  const server_module = context.asAbsolutePath('out/server.js');
  const server_options: ServerOptions = {
    run: { module: server_module, transport: TransportKind.ipc },
    debug: { module: server_module, transport: TransportKind.ipc },
  };

  const client_options: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'ns' }],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher('**/.clientrc'),
    },
  };

  client = new LanguageClient(
    'ns',
    'ns',
    server_options,
    client_options
  );

  client.start();
}

export function deactivate(): Thenable<void> | undefined {
  return client ? client.stop() : undefined;
}