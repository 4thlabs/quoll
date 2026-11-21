import type { Problem } from "./generated";

let token: string | undefined;

export function signIn(accessToken: string) {
  token = accessToken;
}

export function signOut() {
  token = undefined;
}

// An error the server answered, its message the problem's detail: what the page shows.
export class ApiError extends Error {
  readonly status: number;
  readonly problem: Problem | undefined;

  constructor(status: number, problem?: Problem) {
    super(problem?.detail ?? problem?.title ?? `the server answered ${status}`);
    this.name = "ApiError";
    this.status = status;
    this.problem = problem;
  }
}

// The error type of orval's hooks, whatever the problem a route declares: every one is a `Problem`.
export type ErrorType<_Body> = ApiError;

function authorization(init?: HeadersInit): Headers {
  const headers = new Headers(init);

  if (token) {
    headers.set("Authorization", `Bearer ${token}`);
  }

  return headers;
}

// The token was refused: it expired, or the server that gave it restarted.
export function isUnauthorized(error: unknown): boolean {
  return error instanceof ApiError && error.status === 401;
}

// A proxy that cannot reach the server answers with no problem, and a body may be cut: the status alone is the error.
async function apiError(response: Response): Promise<ApiError> {
  const isProblem = response.headers.get("Content-Type")?.startsWith("application/problem+json");

  if (!isProblem) {
    return new ApiError(response.status);
  }

  try {
    return new ApiError(response.status, await response.json());
  } catch {
    return new ApiError(response.status);
  }
}

export async function apiFetch<T>(url: string, init: RequestInit): Promise<T> {
  const response = await fetch(url, { ...init, headers: authorization(init.headers) });

  if (!response.ok) {
    throw await apiError(response);
  }

  return (response.status === 204 ? undefined : await response.json()) as T;
}

// Reads a stream of server-sent events, calling `onItem` with each one's data parsed, until the function it returns
// is called. `onEnd` hears of an end it did not ask for: with the error of a failure, before the stream or within it,
// or with nothing when the server ended the stream.
export function apiEvents<T>(
  url: string,
  onItem: (item: T) => void,
  onEnd: (error?: unknown) => void = logFailure,
): () => void {
  const controller = new AbortController();

  function ended(error?: unknown) {
    if (!controller.signal.aborted) {
      onEnd(error);
    }
  }

  readEvents(url, onItem, controller.signal).then(() => ended(), ended);

  return () => controller.abort();
}

function logFailure(error?: unknown) {
  if (error !== undefined) {
    console.error(error);
  }
}

async function readEvents<T>(url: string, onItem: (item: T) => void, signal: AbortSignal) {
  const response = await fetch(url, { headers: authorization(), signal });

  if (!response.ok) {
    throw await apiError(response);
  }

  const reader = response.body!.pipeThrough(new TextDecoderStream()).getReader();
  let buffer = "";

  for (;;) {
    const { done, value } = await reader.read();

    if (done) {
      return;
    }

    // A chunk may end within an event: its start waits in `buffer` for the next one.
    const blocks = (buffer + value).split("\n\n");
    buffer = blocks.pop() ?? "";

    for (const block of blocks) {
      const { event, data } = parseEvent(block);

      if (data === undefined) {
        continue;
      }

      // The stream failed after its 200: the problem says the status it would have answered.
      if (event === "problem") {
        const problem: Problem = JSON.parse(data);

        throw new ApiError(problem.status ?? 500, problem);
      }

      onItem(JSON.parse(data) as T);
    }
  }
}

// One event, its lines `field: value`; a line that starts with `:` is a comment.
function parseEvent(block: string): { event?: string; data?: string } {
  let event: string | undefined;
  let data: string | undefined;

  for (const line of block.split("\n")) {
    const colon = line.indexOf(":");
    const field = colon < 0 ? line : line.slice(0, colon);
    const value = colon < 0 ? "" : line.slice(colon + 1).replace(/^ /, "");

    if (field === "event") {
      event = value;
    } else if (field === "data") {
      data = data === undefined ? value : `${data}\n${value}`;
    }
  }

  return { event, data };
}
