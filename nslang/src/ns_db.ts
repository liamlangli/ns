process.stdin.on('error', (err) => {
	console.error('stdin error:', err);
	process.exit(1);
});

let buffer = Buffer.from([]);

process.stdin.on('readable', () => {
	while (process.stdin.readable) {
		const chunk = process.stdin.read();
		if (chunk == null || chunk.length === 0) {
			break;
		}

		buffer = Buffer.concat([buffer, chunk]);
	}

	while (true) {
		const message = extract_message(buffer);
		if (message === null) {
			break;
		}

		process_message(message);
	}
});


function find_index(buf: Buffer, pat: string): number | null {
	const pattern: Uint8Array = new TextEncoder().encode(pat)

	for (let i = 0; i + pattern.length <= buffer.length; i++) {
	  const part = buffer.slice(i, i + pattern.length)
	  if (part.compare(pattern) === 0) {
		return i
	  }
	}
	return null
}

function extract_message(buffer: Buffer): any | null {
	const header_index = find_index(buffer, '\r\n\r\n');
	if (header_index == null) {
		return null;
	}

	const body_index = header_index + 4;
	const header = new TextDecoder().decode(buffer.slice(0, header_index));
	let len: number | null = null;
	for (const line of header.split('\r\n')) {
		const [key, value] = line.split(': ');
		if (key === 'Content-Length') {
			len = parseInt(value);
			break;
		}

		if (key !== '') {
			console.error('unknown header:', key);
		}
	}

	if (len === null) {
		console.error('Content-Length not found');
		return null;
	}

	const body = buffer.slice(body_index, body_index + len);
	const body_obj = JSON.parse(new TextDecoder().decode(body));
	return body_obj;
}

function process_message(request: any) {
	console.log(request);
}

console.log('ns_db started');