const fs = require("fs");
const path = require("path");
const vscode = require("vscode");

function findProjectRoot(filePath) {
    let directory = path.dirname(filePath);

    while (true) {
        if (fs.existsSync(path.join(directory, "ns.mod"))) {
            return directory;
        }

        const parent = path.dirname(directory);
        if (parent === directory) {
            return undefined;
        }
        directory = parent;
    }
}

async function execute(action) {
    const editor = vscode.window.activeTextEditor;
    const document = editor && editor.document;

    if (!document || document.languageId !== "ns" || document.uri.scheme !== "file") {
        void vscode.window.showWarningMessage(
            "Open a saved Nano Script file before running or building."
        );
        return;
    }

    await vscode.workspace.saveAll(false);

    const filePath = document.uri.fsPath;
    const projectRoot = findProjectRoot(filePath);
    const workingDirectory = projectRoot || path.dirname(filePath);
    const args = projectRoot ? [action] : [action, filePath];
    const executable = vscode.workspace
        .getConfiguration("nslang", document.uri)
        .get("executablePath", "ns")
        .trim() || "ns";
    const workspaceFolder = vscode.workspace.getWorkspaceFolder(document.uri);
    const scope = workspaceFolder || vscode.TaskScope.Workspace;
    const title = action === "run" ? "Run Project" : "Build Project";
    const execution = new vscode.ShellExecution(executable, args, {
        cwd: workingDirectory
    });
    const task = new vscode.Task(
        {
            type: "nanoscript",
            action,
            project: projectRoot || filePath
        },
        scope,
        title,
        "Nano Script",
        execution
    );

    task.presentationOptions = {
        reveal: vscode.TaskRevealKind.Always,
        panel: vscode.TaskPanelKind.Dedicated,
        focus: action === "run"
    };

    await vscode.tasks.executeTask(task);
}

function activate(context) {
    context.subscriptions.push(
        vscode.commands.registerCommand("nslang.run", () => execute("run")),
        vscode.commands.registerCommand("nslang.build", () => execute("build"))
    );
}

function deactivate() {}

module.exports = {
    activate,
    deactivate
};
