import {
  createConnection,
  TextDocuments,
  Diagnostic,
  DiagnosticSeverity,
  ProposedFeatures,
  InitializeParams,
  TextDocumentSyncKind,
  InitializeResult,
} from "vscode-languageserver/node";

import { TextDocument } from "vscode-languageserver-textdocument";

// Create a connection for the server
const connection = createConnection(ProposedFeatures.all);

// Create a simple text document manager
const documents: TextDocuments<TextDocument> = new TextDocuments(TextDocument);

connection.onInitialize((params: InitializeParams): InitializeResult => {
  return {
    capabilities: {
      textDocumentSync: TextDocumentSyncKind.Incremental,
      // Add other capabilities like hover, autocomplete, etc., here
    },
  };
});

// Example: Validate text documents
documents.onDidChangeContent((change) => {
  validateTextDocument(change.document);
});

async function validateTextDocument(textDocument: TextDocument): Promise<void> {
  const diagnostics: Diagnostic[] = [];
  const text = textDocument.getText();
  const lines = text.split(/\r?\n/g);

  lines.forEach((line: string, i: number) => {
    if (line.includes("error")) {
      diagnostics.push({
        severity: DiagnosticSeverity.Error,
        range: {
          start: { line: i, character: line.indexOf("error") },
          end: { line: i, character: line.indexOf("error") + 5 },
        },
        message: `Found the word "error"`,
        source: "ns",
      });
    }
  });

  connection.sendDiagnostics({ uri: textDocument.uri, diagnostics });
}

// Listen for document events
documents.listen(connection);
connection.listen();
