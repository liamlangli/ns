import { write } from "fs";

interface NSDBContext {
    buf: Buffer;
}

process.stdin.on("error", (err) => {
    console.error("stdin error:", err);
    process.exit(1);
});

const _ctx: NSDBContext = {
    buf: Buffer.alloc(0),
};

process.stdin.on("readable", () => {
    while (process.stdin.readable) {
        const chunk = process.stdin.read();
        if (chunk == null || chunk.length === 0) {
            break;
        }
        _ctx.buf = Buffer.concat([_ctx.buf, chunk]);
    }

    while (true) {
        const msg = extract_message(_ctx);
        if (msg === null) {
            break;
        }
        process_message(msg);
    }
});

function find_index(buf: Buffer, pat: string): number | null {
    const pattern: Uint8Array = new TextEncoder().encode(pat);

    for (let i = 0; i + pattern.length <= buf.length; i++) {
        const part = buf.slice(i, i + pattern.length);
        if (part.compare(pattern) === 0) {
            return i;
        }
    }
    return null;
}

function extract_message(ctx: NSDBContext): any | null {
    const header_index = find_index(ctx.buf, "\r\n\r\n");
    if (header_index == null) {
        return null;
    }

    const body_index = header_index + 4;
    const header = new TextDecoder().decode(ctx.buf.slice(0, header_index));
    let len: number | null = null;
    for (const line of header.split("\r\n")) {
        const [key, value] = line.split(": ");
        if (key === "Content-Length") {
            len = parseInt(value);
            break;
        }

        if (key !== "") {
            console.error("unknown header:", key);
        }
    }

    if (len === null) {
        console.error("Content-Length not found");
        return null;
    }

    const body = ctx.buf.slice(body_index, body_index + len);
    const body_str = new TextDecoder().decode(body);
    const body_obj = JSON.parse(body_str);
    ctx.buf = ctx.buf.slice(body_index + len);
    return body_obj;
}

interface DapRequest {
    seq: number;
    type: "request";
    command: string;
    arguments?: unknown;
}

function validate(msg: any): DapRequest | null {
    const { type, seq, command } = msg;
    return type === "request" &&
        typeof seq === "number" &&
        typeof command === "string"
        ? { ...msg, type, seq, command }
        : null;
}

let last_seq = 0;

function write_dap_response(res: any) {
    const json = JSON.stringify(res) + "\r\n";
    const encoded = new TextEncoder().encode(json);
    const len = encoded.length;
    process.stdout.write(`Content-Length: ${len}\r\n\r\n`);
    process.stdout.write(encoded);
}

function write_ack(req: DapRequest) {
    write_dap_response({
        type: "response",
        seq: ++last_seq,
        request_seq: req.seq,
        command: req.command,
        success: true,
    });
}

function process_message(msg: any) {
    const dap_request = validate(msg);
    if (dap_request === null) {
        console.error("invalid message:", msg);
        return;
    }

    console.log("request:", dap_request.command);

    try {
        switch (dap_request.command) {
            case "initialize":
                write_ack(dap_request);
                write_dap_response({
                    type: "event",
                    seq: ++last_seq,
                    event: "initialized"});
                return;
            case "launch":
                console.log("launch: " + JSON.stringify(dap_request.arguments));
                write_ack(dap_request);
                return;
            case "setBreakpoints":
                console.log("setBreakpoints: " + JSON.stringify(dap_request.arguments));
                write_ack(dap_request);
                return;
            case "setFunctionBreakpoints":
                console.log("setFunctionBreakpoints: " + JSON.stringify(dap_request.arguments));
                write_ack(dap_request);
                return;
            case "setExceptionBreakpoints":
                console.log("setExceptionBreakpoints: " + JSON.stringify(dap_request.arguments));
                write_ack(dap_request);
                return;
            case "threads":
                console.log("threads");
                write_ack(dap_request);
                write_dap_response({
                    type: "response",
                    seq: ++last_seq,
                    request_seq: dap_request.seq,
                    command: dap_request.command,
                    success: true,
                    body: { threads: [] },
                });
                return;
            case "disconnect":
                write_ack(dap_request);
                process.exit(0);
            default:
                throw new Error("unknown command:" + dap_request.command);
        }
    } catch (err) {
        console.error("error:", err);
        write_dap_response({
            type: "response",
            seq: ++last_seq,
            request_seq: dap_request.seq,
            command: dap_request.command,
            success: false,
        });
    }
}

console.log("ns_db started");
