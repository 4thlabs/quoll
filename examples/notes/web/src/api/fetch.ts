import type { Problem } from "./generated";

let token: string | undefined;

export function signIn(accessToken: string) {
  token = accessToken;
}

export function signOut() {
  token = undefined;
}

// An error the server answered, its message the problem's detail: what the page shows. `expired` when the server
// refused the token that the page still holds: it expired, or the server that gave it restarted.
export class ApiError extends Error {
  readonly status: number;
  readonly problem: Problem | undefined;
  readonly expired: boolean;

  constructor(status: number, problem?: Problem, expired = false) {
    super(problem?.detail ?? problem?.title ?? `the server answered ${status}`);
    this.name = "ApiError";
    this.status = status;
    this.problem = problem;
    this.expired = expired;
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

export function isExpired(error: unknown): boolean {
  return error instanceof ApiError && error.expired;
}

// The error of a request sent with the token `sent`. A refusal of a token signed out since, answered late, is no news
// of the session the page holds now.
async function apiError(response: Response, sent: string | undefined): Promise<ApiError> {
  const expired = response.status === 401 && sent !== undefined && sent === token;

  return new ApiError(response.status, await problemOf(response), expired);
}

// A proxy that cannot reach the server answers with no problem, and a body may be cut: the status alone is the error.
async function problemOf(response: Response): Promise<Problem | undefined> {
  if (!response.headers.get("Content-Type")?.startsWith("application/problem+json")) {
    return undefined;
  }

  try {
    return await response.json();
  } catch {
    return undefined;
  }
}

export async function apiFetch<T>(url: string, init: RequestInit): Promise<T> {
  const sent = token;
  const response = await fetch(url, { ...init, headers: authorization(init.headers) });

  if (!response.ok) {
    throw await apiError(response, sent);
  }

  return (response.status === 204 ? undefined : await response.json()) as T;
}

export type EventHandlers<T> = {
  // Each event's data, parsed.
  onItem: (item: T) => void;
  // The stream is open: what changed before it is not in it.
  onOpen?: () => void;
  // An end that was not asked for: the error of a failure, before the stream or within it, or nothing when the
  // server ended the stream.
  onEnd?: (error?: unknown) => void;
};

// Reads a stream of server-sent events until the function it returns is called.
export function apiEvents<T>(
  url: string,
  { onItem, onOpen = () => {}, onEnd = logFailure }: EventHandlers<T>,
): () => void {
  const controller = new AbortController();

  function ended(error?: unknown) {
    if (!controller.signal.aborted) {
      onEnd(error);
    }
  }

  readEvents(url, onItem, onOpen, controller.signal).then(() => ended(), ended);

  return () => controller.abort();
}

function logFailure(error?: unknown) {
  if (error !== undefined) {
    console.error(error);
  }
}

async function readEvents<T>(url: string, onItem: (item: T) => void, onOpen: () => void, signal: AbortSignal) {
  const sent = token;
  const response = await fetch(url, { headers: authorization(), signal });

  if (!response.ok) {
    throw await apiError(response, sent);
  }

  onOpen();

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
