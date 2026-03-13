const BYTES_PER_U32 = 4;
const TOKEN_RECORD_U32 = 6;
const AST_RECORD_U32 = 6;
const COUNTER_U32 = 4;

const INGEST_SHADER = /* wgsl */`
struct Limits {
    sourceWordCount: u32,
    sourceByteLength: u32,
    functionCount: u32,
    maxTokensPerFunction: u32,
    maxAstPerFunction: u32,
}
@group(0) @binding(0) var<storage, read> ingest_words: array<u32>;
@group(0) @binding(1) var<storage, read_write> source_words: array<u32>;
@group(0) @binding(2) var<uniform> limits: Limits;
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= limits.sourceWordCount) { return; }
    source_words[gid.x] = ingest_words[gid.x];
}
`;

const TOKENIZE_SHADER = /* wgsl */`
struct Limits {
    sourceWordCount: u32,
    sourceByteLength: u32,
    functionCount: u32,
    maxTokensPerFunction: u32,
    maxAstPerFunction: u32,
}
struct FunctionSpan { start: u32, end: u32, }
struct TokenRecord {
    functionId: u32,
    start: u32,
    end: u32,
    kind: u32,
    value0: u32,
    value1: u32,
}
struct Counters {
    tokenCount: atomic<u32>,
    astCount: atomic<u32>,
    tokenOverflow: atomic<u32>,
    astOverflow: atomic<u32>,
}
@group(0) @binding(0) var<storage, read> spans: array<FunctionSpan>;
@group(0) @binding(1) var<storage, read> source_words: array<u32>;
@group(0) @binding(2) var<storage, read_write> tokens: array<TokenRecord>;
@group(0) @binding(3) var<storage, read_write> counters: Counters;
@group(0) @binding(4) var<uniform> limits: Limits;
fn load_byte(offset: u32) -> u32 {
    if (offset >= limits.sourceByteLength) { return 0u; }
    let w = source_words[offset / 4u];
    let shift = (offset % 4u) * 8u;
    return (w >> shift) & 0xffu;
}
fn is_alpha(ch: u32) -> bool {
    return (ch >= 65u && ch <= 90u) || (ch >= 97u && ch <= 122u) || ch == 95u;
}
fn is_digit(ch: u32) -> bool { return ch >= 48u && ch <= 57u; }
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let fn_id = gid.x;
    if (fn_id >= limits.functionCount) { return; }
    let span = spans[fn_id];
    var cursor = span.start;
    var local_token_count = 0u;
    loop {
        if (cursor >= span.end || cursor >= limits.sourceByteLength || local_token_count >= limits.maxTokensPerFunction) { break; }
        var ch = load_byte(cursor);
        if (ch == 32u || ch == 9u || ch == 10u || ch == 13u) { cursor = cursor + 1u; continue; }
        var kind = 3u;
        let start = cursor;
        if (is_alpha(ch)) {
            kind = 1u;
            cursor = cursor + 1u;
            loop {
                if (cursor >= span.end) { break; }
                ch = load_byte(cursor);
                if (!(is_alpha(ch) || is_digit(ch))) { break; }
                cursor = cursor + 1u;
            }
        } else if (is_digit(ch)) {
            kind = 2u;
            cursor = cursor + 1u;
            loop {
                if (cursor >= span.end) { break; }
                ch = load_byte(cursor);
                if (!is_digit(ch)) { break; }
                cursor = cursor + 1u;
            }
        } else {
            cursor = cursor + 1u;
        }
        let token_idx = atomicAdd(&counters.tokenCount, 1u);
        let max_tokens = limits.functionCount * limits.maxTokensPerFunction;
        if (token_idx >= max_tokens) { atomicStore(&counters.tokenOverflow, 1u); continue; }
        tokens[token_idx].functionId = fn_id;
        tokens[token_idx].start = start;
        tokens[token_idx].end = cursor;
        tokens[token_idx].kind = kind;
        tokens[token_idx].value0 = 0u;
        tokens[token_idx].value1 = 0u;
        local_token_count = local_token_count + 1u;
    }
}
`;

const PARSE_SHADER = /* wgsl */`
struct Limits {
    sourceWordCount: u32,
    sourceByteLength: u32,
    functionCount: u32,
    maxTokensPerFunction: u32,
    maxAstPerFunction: u32,
}
struct TokenRecord {
    functionId: u32,
    start: u32,
    end: u32,
    kind: u32,
    value0: u32,
    value1: u32,
}
struct AstNode {
    functionId: u32,
    tokenStart: u32,
    tokenCount: u32,
    kind: u32,
    payload0: u32,
    payload1: u32,
}
struct Counters {
    tokenCount: atomic<u32>,
    astCount: atomic<u32>,
    tokenOverflow: atomic<u32>,
    astOverflow: atomic<u32>,
}
@group(0) @binding(0) var<storage, read> tokens: array<TokenRecord>;
@group(0) @binding(1) var<storage, read_write> ast_nodes: array<AstNode>;
@group(0) @binding(2) var<storage, read_write> counters: Counters;
@group(0) @binding(3) var<uniform> limits: Limits;
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let fn_id = gid.x;
    if (fn_id >= limits.functionCount) { return; }
    let token_count = atomicLoad(&counters.tokenCount);
    var emitted = 0u;
    for (var i = 0u; i < token_count; i = i + 1u) {
        let tok = tokens[i];
        if (tok.functionId != fn_id || emitted >= limits.maxAstPerFunction) { continue; }
        let node_idx = atomicAdd(&counters.astCount, 1u);
        let max_nodes = limits.functionCount * limits.maxAstPerFunction;
        if (node_idx >= max_nodes) { atomicStore(&counters.astOverflow, 1u); continue; }
        ast_nodes[node_idx].functionId = fn_id;
        ast_nodes[node_idx].tokenStart = tok.start;
        ast_nodes[node_idx].tokenCount = tok.end - tok.start;
        ast_nodes[node_idx].kind = tok.kind;
        ast_nodes[node_idx].payload0 = i;
        ast_nodes[node_idx].payload1 = 0u;
        emitted = emitted + 1u;
    }
}
`;

function divUp(a, b) { return Math.ceil(a / b); }
function toU32Words(bytes) {
    const words = new Uint32Array(Math.ceil(bytes.length / BYTES_PER_U32));
    for (let i = 0; i < bytes.length; i++) {
        const prev = words[i >>> 2] ?? 0;
        const byte = bytes[i] ?? 0;
        words[i >>> 2] = prev | (byte << ((i & 3) * 8));
    }
    return words;
}

export class GPUParser {
    constructor(device, opts = {}) {
        this.device = device;
        this.maxSourceBytes = opts.maxSourceBytes ?? 1 << 20;
        this.maxFunctions = opts.maxFunctions ?? 256;
        this.maxTokensPerFunction = opts.maxTokensPerFunction ?? 1024;
        this.maxAstPerFunction = opts.maxAstPerFunction ?? 1024;

        const sourceWords = Math.ceil(this.maxSourceBytes / BYTES_PER_U32);
        const tokenCap = this.maxFunctions * this.maxTokensPerFunction;
        const astCap = this.maxFunctions * this.maxAstPerFunction;

        this.limitsBuffer = device.createBuffer({ size: 5 * 4, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
        this.ingestUploadBuffer = device.createBuffer({ size: sourceWords * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
        this.sourceBuffer = device.createBuffer({ size: sourceWords * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
        this.functionSpanBuffer = device.createBuffer({ size: this.maxFunctions * 8, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
        this.tokenBuffer = device.createBuffer({ size: tokenCap * TOKEN_RECORD_U32 * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
        this.astBuffer = device.createBuffer({ size: astCap * AST_RECORD_U32 * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
        this.counterBuffer = device.createBuffer({ size: COUNTER_U32 * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC });

        this.tokenReadback = device.createBuffer({ size: tokenCap * TOKEN_RECORD_U32 * 4, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
        this.astReadback = device.createBuffer({ size: astCap * AST_RECORD_U32 * 4, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
        this.counterReadback = device.createBuffer({ size: COUNTER_U32 * 4, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });

        this.ingestPipeline = device.createComputePipeline({ layout: 'auto', compute: { module: device.createShaderModule({ code: INGEST_SHADER }), entryPoint: 'main' } });
        this.tokenizePipeline = device.createComputePipeline({ layout: 'auto', compute: { module: device.createShaderModule({ code: TOKENIZE_SHADER }), entryPoint: 'main' } });
        this.parsePipeline = device.createComputePipeline({ layout: 'auto', compute: { module: device.createShaderModule({ code: PARSE_SHADER }), entryPoint: 'main' } });

        this.ingestBG = device.createBindGroup({
            layout: this.ingestPipeline.getBindGroupLayout(0),
            entries: [
                { binding: 0, resource: { buffer: this.ingestUploadBuffer } },
                { binding: 1, resource: { buffer: this.sourceBuffer } },
                { binding: 2, resource: { buffer: this.limitsBuffer } },
            ],
        });
        this.tokenizeBG = device.createBindGroup({
            layout: this.tokenizePipeline.getBindGroupLayout(0),
            entries: [
                { binding: 0, resource: { buffer: this.functionSpanBuffer } },
                { binding: 1, resource: { buffer: this.sourceBuffer } },
                { binding: 2, resource: { buffer: this.tokenBuffer } },
                { binding: 3, resource: { buffer: this.counterBuffer } },
                { binding: 4, resource: { buffer: this.limitsBuffer } },
            ],
        });
        this.parseBG = device.createBindGroup({
            layout: this.parsePipeline.getBindGroupLayout(0),
            entries: [
                { binding: 0, resource: { buffer: this.tokenBuffer } },
                { binding: 1, resource: { buffer: this.astBuffer } },
                { binding: 2, resource: { buffer: this.counterBuffer } },
                { binding: 3, resource: { buffer: this.limitsBuffer } },
            ],
        });
    }

    async run(source, functionSpans) {
        const sourceBytes = new TextEncoder().encode(source);
        const clampedSource = sourceBytes.subarray(0, this.maxSourceBytes);
        const spans = functionSpans.slice(0, this.maxFunctions).map((span) => ({
            start: Math.min(span.start, clampedSource.length),
            end: Math.min(Math.max(span.end, span.start), clampedSource.length),
        }));

        const sourceWords = toU32Words(clampedSource);
        const spanArray = new Uint32Array(this.maxFunctions * 2);
        for (let i = 0; i < spans.length; i++) {
            const span = spans[i];
            if (!span) continue;
            spanArray[i * 2] = span.start;
            spanArray[i * 2 + 1] = span.end;
        }

        this.device.queue.writeBuffer(this.ingestUploadBuffer, 0, sourceWords.buffer.slice(sourceWords.byteOffset, sourceWords.byteOffset + sourceWords.byteLength));
        this.device.queue.writeBuffer(this.functionSpanBuffer, 0, spanArray);
        this.device.queue.writeBuffer(this.counterBuffer, 0, new Uint32Array(COUNTER_U32));
        this.device.queue.writeBuffer(this.limitsBuffer, 0, new Uint32Array([
            sourceWords.length,
            clampedSource.length,
            spans.length,
            this.maxTokensPerFunction,
            this.maxAstPerFunction,
        ]));

        const encoder = this.device.createCommandEncoder({ label: 'gpu-parse' });
        let pass = encoder.beginComputePass({ label: 'source-ingestion' });
        pass.setPipeline(this.ingestPipeline);
        pass.setBindGroup(0, this.ingestBG);
        pass.dispatchWorkgroups(Math.max(1, divUp(sourceWords.length, 64)));
        pass.end();

        pass = encoder.beginComputePass({ label: 'tokenization' });
        pass.setPipeline(this.tokenizePipeline);
        pass.setBindGroup(0, this.tokenizeBG);
        pass.dispatchWorkgroups(Math.max(1, divUp(spans.length, 64)));
        pass.end();

        pass = encoder.beginComputePass({ label: 'parse-ast' });
        pass.setPipeline(this.parsePipeline);
        pass.setBindGroup(0, this.parseBG);
        pass.dispatchWorkgroups(Math.max(1, divUp(spans.length, 64)));
        pass.end();

        encoder.copyBufferToBuffer(this.counterBuffer, 0, this.counterReadback, 0, this.counterReadback.size);
        encoder.copyBufferToBuffer(this.tokenBuffer, 0, this.tokenReadback, 0, this.tokenReadback.size);
        encoder.copyBufferToBuffer(this.astBuffer, 0, this.astReadback, 0, this.astReadback.size);
        this.device.queue.submit([encoder.finish()]);

        const [counterData, tokenData, astData] = await Promise.all([
            this.readMappedU32(this.counterReadback),
            this.readMappedU32(this.tokenReadback),
            this.readMappedU32(this.astReadback),
        ]);

        const tokenCount = Math.min(counterData[0] ?? 0, this.maxFunctions * this.maxTokensPerFunction);
        const astCount = Math.min(counterData[1] ?? 0, this.maxFunctions * this.maxAstPerFunction);

        const tokens = [];
        for (let i = 0; i < tokenCount; i++) {
            const base = i * TOKEN_RECORD_U32;
            tokens.push({
                functionId: tokenData[base],
                start: tokenData[base + 1],
                end: tokenData[base + 2],
                kind: tokenData[base + 3],
                value0: tokenData[base + 4],
                value1: tokenData[base + 5],
            });
        }

        const astNodes = [];
        for (let i = 0; i < astCount; i++) {
            const base = i * AST_RECORD_U32;
            astNodes.push({
                functionId: astData[base],
                tokenStart: astData[base + 1],
                tokenCount: astData[base + 2],
                kind: astData[base + 3],
                payload0: astData[base + 4],
                payload1: astData[base + 5],
            });
        }

        const astByFunction = Array.from({ length: spans.length }, () => []);
        for (const node of astNodes) if (node.functionId < astByFunction.length) astByFunction[node.functionId].push(node);

        return {
            sourceLength: clampedSource.length,
            functionSpans: spans,
            counters: {
                tokenCount,
                astCount,
                tokenOverflow: (counterData[2] ?? 0) !== 0,
                astOverflow: (counterData[3] ?? 0) !== 0,
            },
            tokens,
            astNodes,
            astByFunction,
        };
    }

    async readMappedU32(buffer) {
        await buffer.mapAsync(GPUMapMode.READ);
        const copy = new Uint32Array(buffer.getMappedRange().slice(0));
        buffer.unmap();
        return copy;
    }
}
