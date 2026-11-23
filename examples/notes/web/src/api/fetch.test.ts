import { afterEach, describe, expect, it, vi } from "vitest";

import { ApiError, apiEvents, apiFetch, signIn, signOut } from "./fetch";

// Each call of fetch answers a new response of `respond`.
function answer(respond: () => Response) {
  const fetch = vi.fn(async (_url: string, _init?: RequestInit) => respond());

  vi.stubGlobal("fetch", fetch);

  return fetch;
}

function problem(status: number, detail: string) {
  return new Response(JSON.stringify({ status, detail }), {
    status,
    headers: { "Content-Type": "application/problem+json" },
  });
}

// A body of server-sent events, sent in these chunks.
function events(...chunks: string[]) {
  const body = new ReadableStream<Uint8Array>({
    start(controller) {
      const encoder = new TextEncoder();

      for (const chunk of chunks) {
        controller.enqueue(encoder.encode(chunk));
      }

      controller.close();
    },
  });

  return new Response(body, { headers: { "Content-Type": "text/event-stream" } });
}

afterEach(() => {
  signOut();
  vi.unstubAllGlobals();
});

describe("apiFetch", () => {
  it("sends the token once signed in", async () => {
    const fetch = answer(() => Response.json([]));

    await apiFetch("/api/notes", {});
    signIn("secret");
    await apiFetch("/api/notes", { headers: { Accept: "application/json" } });

    const headers = fetch.mock.calls.map(([, init]) => new Headers(init?.headers));

    expect(headers[0].has("Authorization")).toBe(false);
    expect(headers[1].get("Authorization")).toBe("Bearer secret");
    expect(headers[1].get("Accept")).toBe("application/json");
  });

  it("returns the body, and null for a 204 or a 205", async () => {
    answer(() => Response.json({ id: 1 }));
    await expect(apiFetch("/api/notes/1", {})).resolves.toEqual({ id: 1 });

    answer(() => new Response(null, { status: 204 }));
    await expect(apiFetch("/api/notes/1", { method: "DELETE" })).resolves.toBeNull();

    answer(() => new Response(null, { status: 205 }));
    await expect(apiFetch("/api/notes/1", { method: "DELETE" })).resolves.toBeNull();
  });

  it("throws the problem of an error, its detail as the message", async () => {
    answer(() => problem(404, "no such note"));

    const error = await apiFetch("/api/notes/9", {}).catch((error: unknown) => error);

    expect(error).toBeInstanceOf(ApiError);
    expect(error).toMatchObject({ status: 404, message: "no such note", problem: { detail: "no such note" } });
  });

  it("calls a refusal of the token it holds an expiry, not one of a token signed out since", async () => {
    answer(() => problem(401, "unknown or expired token"));

    signIn("old");
    await expect(apiFetch("/api/notes", {})).rejects.toMatchObject({ status: 401, expired: true });

    // The answer comes once the page signed in again: it says nothing of the new token.
    let respond: (response: Response) => void = () => {};

    vi.stubGlobal("fetch", () => new Promise<Response>((resolve) => (respond = resolve)));
    const late = apiFetch("/api/notes", {}).catch((error: unknown) => error);

    signOut();
    signIn("new");
    respond(problem(401, "unknown or expired token"));

    expect(await late).toMatchObject({ status: 401, expired: false });
  });

  it("calls no refusal of a sign-in an expiry", async () => {
    answer(() => problem(401, "wrong user or password"));

    await expect(apiFetch("/api/token", { method: "POST" })).rejects.toMatchObject({ status: 401, expired: false });
  });

  it("throws the status of a problem whose body does not read", async () => {
    answer(() => new Response("{", { status: 500, headers: { "Content-Type": "application/problem+json" } }));

    await expect(apiFetch("/api/notes", {})).rejects.toMatchObject({
      status: 500,
      message: "the server answered 500",
      problem: undefined,
    });
  });

  it("throws the status of an error that is no problem", async () => {
    answer(() => new Response("Bad Gateway", { status: 502 }));

    await expect(apiFetch("/api/notes", {})).rejects.toMatchObject({
      status: 502,
      message: "the server answered 502",
      problem: undefined,
    });
  });
});

describe("apiEvents", () => {
  it("reads events split across chunks, whatever their name", async () => {
    answer(() => events('data: {"kind":"ed', 'ited","id":1}\n', "\nevent: other\ndata: 2\n\n"));
    const items: unknown[] = [];

    apiEvents("/api/notes/1/events", { onItem: (item) => items.push(item) });

    await vi.waitFor(() => expect(items).toEqual([{ kind: "edited", id: 1 }, 2]));
  });

  it("tells of its opening before its items, then of a clean end, with no error", async () => {
    // As quoll's server writes them: the comment that opens the stream, then the events, then the end, as on a stop.
    answer(() => events(": \n\n", "data: 1\n\n"));
    const heard: unknown[] = [];

    apiEvents("/api/notes/1/events", {
      onItem: (item) => heard.push(item),
      onOpen: () => heard.push("open"),
      onEnd: (error) => heard.push(["end", error]),
    });

    await vi.waitFor(() => expect(heard).toEqual(["open", 1, ["end", undefined]]));
  });

  it("ends at once, with no error, on a 204 or a 205, which has no body", async () => {
    for (const status of [204, 205]) {
      answer(() => new Response(null, { status }));
      const heard: unknown[] = [];

      apiEvents("/api/notes/1/events", {
        onItem: (item) => heard.push(item),
        onOpen: () => heard.push("open"),
        onEnd: (error) => heard.push(["end", error]),
      });

      await vi.waitFor(() => expect(heard).toEqual([["end", undefined]]));
    }
  });

  it("skips comments and events with no data, and joins lines of data", async () => {
    answer(() => events(": ping\n\nevent: empty\n\ndata: [1,\ndata: 2]\n\n"));
    const items: unknown[] = [];

    apiEvents("/api/notes/1/events", { onItem: (item) => items.push(item) });

    await vi.waitFor(() => expect(items).toEqual([[1, 2]]));
  });

  it("ends with the problem of an `event: problem`", async () => {
    answer(() => events('data: 1\n\nevent: problem\ndata: {"status":503,"detail":"the store is gone"}\n\ndata: 2\n\n'));
    const items: unknown[] = [];
    const onEnd = vi.fn();

    apiEvents("/api/notes/1/events", { onItem: (item) => items.push(item), onEnd });

    await vi.waitFor(() => expect(onEnd).toHaveBeenCalledOnce());
    expect(onEnd.mock.calls[0][0]).toMatchObject({ status: 503, message: "the store is gone" });
    expect(items).toEqual([1]);
  });

  it("sends the token, and reports the problem of a refusal", async () => {
    const fetch = answer(() => problem(401, "unknown or expired token"));
    const onEnd = vi.fn();

    signIn("secret");
    apiEvents("/api/notes/1/events", { onItem: () => {}, onEnd });

    await vi.waitFor(() => expect(onEnd).toHaveBeenCalledOnce());
    expect(new Headers(fetch.mock.calls[0][1]?.headers).get("Authorization")).toBe("Bearer secret");
    expect(onEnd.mock.calls[0][0]).toMatchObject({ status: 401, message: "unknown or expired token", expired: true });
  });
});
