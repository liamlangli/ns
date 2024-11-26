import { LoggingDebugSession, StoppedEvent, TerminatedEvent } from "vscode-debugadapter";
import { DebugProtocol } from "vscode-debugprotocol";
import { promises as fs } from 'fs';
import * as Net from 'net';

export class NSDebugger extends LoggingDebugSession {
    private running = false;
    sendSen: any;

    constructor() {
        super('nsdb');
    }

    protected initializeRequest(response: DebugProtocol.InitializeResponse, args: DebugProtocol.InitializeRequestArguments): void {
        response.body = response.body || {};
        response.body.supportsConfigurationDoneRequest = true;
        this.sendResponse(response);
    }

    protected launchRequest(response: DebugProtocol.LaunchResponse, args: DebugProtocol.LaunchRequestArguments, request?: DebugProtocol.Request): void {
        this.running = true;
        console.log(args);
        this.sendEvent(new StoppedEvent("breakpoint", 1));
        this.sendResponse(response);
    }

    protected disconnectRequest(response: DebugProtocol.DisconnectResponse, args: DebugProtocol.DisconnectArguments, request?: DebugProtocol.Request): void {
        this.running = false;
        this.sendEvent(new TerminatedEvent());
        this.sendResponse(response);
    }

    protected setBreakPointsRequest(response: DebugProtocol.SetBreakpointsResponse, args: DebugProtocol.SetBreakpointsArguments, request?: DebugProtocol.Request): void {
        this.sendResponse(response);
    }
}

/*
 * debugAdapter.js is the entrypoint of the debug adapter when it runs as a separate process.
 */

/*
 * Since here we run the debug adapter as a separate ("external") process, it has no access to VS Code API.
 * So we can only use node.js API for accessing files.
 */
const fs_accessor = {
	isWindows: process.platform === 'win32',
	readFile(path: string): Promise<Uint8Array> {
		return fs.readFile(path);
	},
	writeFile(path: string, contents: Uint8Array): Promise<void> {
		return fs.writeFile(path, contents);
	}
};

let port = 0;
const args = process.argv.slice(2);
args.forEach(function (val, index, array) {
	const portMatch = /^--server=(\d{4,5})$/.exec(val);
	if (portMatch) {
		port = parseInt(portMatch[1], 10);
	}
});

if (port > 0) {
	// start a server that creates a new session for every connection request
	console.error(`waiting for debug protocol on port ${port}`);
	Net.createServer((socket) => {
		console.error('>> accepted connection from client');
		socket.on('end', () => {
			console.error('>> client connection closed\n');
		});
		const session = new NSDebugger();
		session.setRunAsServer(true);
		session.start(socket, socket);
	}).listen(port);
} else {
	// start a single session that communicates via stdin/stdout
	const session = new NSDebugger();
	process.on('SIGTERM', () => {
		session.shutdown();
	});
	session.start(process.stdin, process.stdout);
}