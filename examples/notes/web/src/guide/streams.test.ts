import { describe, expect, it } from "vitest";

import { readChunks, readJsonLines } from "./streams";

// A body sent in these chunks, one each time it is read, then ended, or cut when `cut` says so.
function body(chunks: string[], cut = false) {
  const encoder = new TextEncoder();
  const left = [...chunks];
  const stream = new ReadableStream<Uint8Array>({
    pull(controller) {
      const chunk = left.shift();

      if (chunk !== undefined) {
        controller.enqueue(encoder.encode(chunk));
      } else if (cut) {
        controller.error(new TypeError("network error"));
      } else {
        controller.close();
      }
    },
  });

  return new Response(stream);
}

describe("readJsonLines", () => {
  it("reads lines split across chunks", async () => {
    const items: unknown[] = [];

    await readJsonLines(body(['{"id":1}\n{"i', 'd":2}\n', '{"id":3}\n']), (item) => items.push(item));

    expect(items).toEqual([{ id: 1 }, { id: 2 }, { id: 3 }]);
  });

  it("throws when the body is cut, after the lines that came", async () => {
    const items: unknown[] = [];

    await expect(readJsonLines(body(['{"id":1}\n{"id"'], true), (item) => items.push(item))).rejects.toThrow();
    expect(items).toEqual([{ id: 1 }]);
  });

  it("reads no line of a 204, which has no body", async () => {
    const items: unknown[] = [];

    await readJsonLines(new Response(null, { status: 204 }), (item) => items.push(item));

    expect(items).toEqual([]);
  });
});

describe("readChunks", () => {
  it("reads the chunks as they come", async () => {
    const chunks: string[] = [];
    const decoder = new TextDecoder();

    await readChunks(body(["ab", "c"]), (chunk) => chunks.push(decoder.decode(chunk)));

    expect(chunks).toEqual(["ab", "c"]);
  });

  it("reads no chunk of a 205, which has no body", async () => {
    const chunks: Uint8Array[] = [];

    await readChunks(new Response(null, { status: 205 }), (chunk) => chunks.push(chunk));

    expect(chunks).toEqual([]);
  });
});
