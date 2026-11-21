import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { cleanup, render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, beforeEach, expect, it, vi } from "vitest";

import { signOut } from "./api/fetch";
import type { Note } from "./api/generated";
import { App } from "./App";

// The notes server, as far as the page calls it: the demo's password is `right`, and the events of note 1 are the
// ones a test pushes.
class FakeServer {
  notes: Note[] = [{ id: 1, title: "Milk", body: "Two bottles" }];
  updates: unknown[] = [];
  private events?: ReadableStreamDefaultController<Uint8Array>;

  fetch = async (url: string, init?: RequestInit): Promise<Response> => {
    const { pathname } = new URL(url, "http://localhost");
    const method = init?.method ?? "GET";

    if (method === "POST" && pathname === "/api/token") {
      const login = new URLSearchParams(init?.body as URLSearchParams);

      return login.get("password") === "right"
        ? Response.json({ access_token: "t1" })
        : problem(401, "wrong user or password");
    }

    if (new Headers(init?.headers).get("Authorization") !== "Bearer t1") {
      return problem(401, "unknown or expired token");
    }

    if (method === "GET" && pathname === "/api/notes") {
      return Response.json(this.notes);
    }

    if (method === "GET" && pathname === "/api/notes/1") {
      return this.notes.length > 0 ? Response.json(this.notes[0]) : problem(404, "no such note");
    }

    if (method === "PUT" && pathname === "/api/notes/1") {
      const changes = JSON.parse(init?.body as string);

      this.updates.push(changes);
      this.notes[0] = { id: 1, ...changes };

      return Response.json(this.notes[0]);
    }

    if (method === "GET" && pathname === "/api/notes/1/events") {
      const body = new ReadableStream<Uint8Array>({ start: (controller) => (this.events = controller) });

      return new Response(body, { headers: { "Content-Type": "text/event-stream" } });
    }

    return problem(404, `no route ${method} ${pathname}`);
  };

  push(event: object) {
    this.events?.enqueue(new TextEncoder().encode(`data: ${JSON.stringify(event)}\n\n`));
  }
}

function problem(status: number, detail: string) {
  return new Response(JSON.stringify({ status, detail }), {
    status,
    headers: { "Content-Type": "application/problem+json" },
  });
}

let server: FakeServer;

beforeEach(() => {
  server = new FakeServer();
  vi.stubGlobal("fetch", vi.fn(server.fetch));
  render(
    <QueryClientProvider client={new QueryClient({ defaultOptions: { queries: { retry: false } } })}>
      <App />
    </QueryClientProvider>,
  );
});

afterEach(() => {
  cleanup();
  signOut();
  vi.unstubAllGlobals();
});

async function signInWith(password: string) {
  const user = userEvent.setup();

  await user.type(screen.getByLabelText("Password"), password);
  await user.click(screen.getByRole("button", { name: "Sign in" }));

  return user;
}

it("shows the problem of a wrong password", async () => {
  await signInWith("wrong");

  expect(await screen.findByRole("alert")).toHaveProperty("textContent", "wrong user or password");
});

it("edits a note, then closes it once another tab removes it", async () => {
  const user = await signInWith("right");

  await user.click(await screen.findByRole("button", { name: "Milk" }));
  expect(await screen.findByText("Two bottles")).toBeDefined();

  await user.click(screen.getByRole("button", { name: "Edit" }));
  await user.clear(screen.getByLabelText("Text"));
  await user.type(screen.getByLabelText("Text"), "Three bottles");
  await user.click(screen.getByRole("button", { name: "Save" }));

  expect(await screen.findByText("Three bottles")).toBeDefined();
  expect(server.updates).toEqual([{ title: "Milk", body: "Three bottles" }]);

  server.notes = [];
  server.push({ kind: "removed", id: 1 });

  await vi.waitFor(() => expect(screen.getByRole("main").children).toHaveLength(0));
  await vi.waitFor(() => {
    expect(within(screen.getByRole("navigation")).queryByRole("button", { name: "Milk" })).toBeNull();
  });
});
