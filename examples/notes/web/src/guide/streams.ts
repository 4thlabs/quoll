// Code of the TypeScript guide that the page does not use, type-checked with it: a `JsonLines` and a `ByteStream`
// answer read with `fetch`, from the `Response` of a request sent as `readEvents` sends its own.

// Reads a `JsonLines` answer, each line the JSON of an item, ended by `\n` as quoll's server writes it. A failure of
// the server after the 200 cuts the body: `read` throws, which is no clean end.
export async function readJsonLines<T>(response: Response, onItem: (item: T) => void): Promise<void> {
  const reader = response.body!.pipeThrough(new TextDecoderStream()).getReader();
  let buffer = "";

  for (;;) {
    const { done, value } = await reader.read();

    if (done) {
      return;
    }

    // A chunk may end within a line: its start waits in `buffer` for the next one.
    const lines = (buffer + value).split("\n");
    buffer = lines.pop() ?? "";

    for (const line of lines) {
      onItem(JSON.parse(line) as T);
    }
  }
}

// Reads a `ByteStream` answer, its chunks as they come. A failure after the 200 cuts it too.
export async function readChunks(response: Response, onChunk: (chunk: Uint8Array) => void): Promise<void> {
  const reader = response.body!.getReader();

  for (;;) {
    const { done, value } = await reader.read();

    if (done) {
      return;
    }

    onChunk(value);
  }
}
