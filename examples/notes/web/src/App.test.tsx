import { cleanup, render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, beforeEach, expect, it, vi } from "vitest";

import { signOut } from "./api/fetch";
import type { Note } from "./api/generated";
import { App } from "./App";

const PAGE_SIZE = 20;

// The notes server, as far as the page calls it: the demo's password is `right`, and it takes one token at a time.
class FakeServer {
  notes: Note[] = [{ id: 1, title: "Milk", body: "Two bottles" }];
  requests: string[] = [];
  refuseEvents = false;
  private token = "t1";
  private streams: ReadableStreamDefaultController<Uint8Array>[] = [];
  private paused?: Hold;
  private delayed?: Hold;

  fetch = async (url: string, init?: RequestInit): Promise<Response> => {
    const { pathname } = new URL(url, "http://localhost");
    const request = `${init?.method ?? "GET"} ${pathname}`;

    this.requests.push(request);
    await this.paused?.wait(request);

    const response = await this.answer(url, init);

    await this.delayed?.wait(request);

    return response;
  };

  private async answer(url: string, init?: RequestInit): Promise<Response> {
    const { pathname, searchParams } = new URL(url, "http://localhost");
    const method = init?.method ?? "GET";

    if (method === "POST" && pathname === "/api/token") {
      const login = new URLSearchParams(init?.body as URLSearchParams);

      return login.get("password") === "right"
        ? Response.json({ access_token: this.token })
        : problem(401, "wrong user or password");
    }

    if (new Headers(init?.headers).get("Authorization") !== `Bearer ${this.token}`) {
      return problem(401, "unknown or expired token");
    }

    if (method === "POST" && pathname === "/api/notes") {
      const note = { id: Math.max(0, ...this.notes.map((note) => note.id)) + 1, ...JSON.parse(init?.body as string) };

      this.notes.push(note);

      return Response.json(note, { status: 201 });
    }

    if (method === "GET" && pathname === "/api/notes") {
      const page = Number(searchParams.get("page") ?? 1);

      return Response.json(this.notes.slice((page - 1) * PAGE_SIZE, page * PAGE_SIZE));
    }

    const [, id, events] = pathname.match(/^\/api\/notes\/(\d+)(\/events)?$/) ?? [];
    const note = this.notes.find((note) => note.id === Number(id));

    if (!note) {
      return problem(404, "no such note");
    }

    if (events) {
      return this.refuseEvents ? problem(401, "unknown or expired token") : this.stream();
    }

    if (method === "GET") {
      return Response.json(note);
    }

    if (method === "PUT") {
      return Response.json(this.change({ id: note.id, ...JSON.parse(init?.body as string) }));
    }

    if (method === "DELETE") {
      this.remove(note.id);

      return new Response(null, { status: 204 });
    }

    return problem(405, `no route ${method} ${pathname}`);
  }

  // A change, from the page or from elsewhere, which the note's watchers hear of.
  change(note: Note): Note {
    this.notes = this.notes.map((stored) => (stored.id === note.id ? note : stored));
    this.send({ kind: "edited", ...note });

    return note;
  }

  remove(id: number) {
    this.notes = this.notes.filter((note) => note.id !== id);
    this.send({ kind: "removed", id });
  }

  // As a restart of the server does: the token it gave is refused.
  forgetToken() {
    this.token = "t2";
  }

  // The streams of events open now.
  get watchers(): number {
    return this.streams.length;
  }

  endEvents() {
    this.streams.forEach((stream) => stream.close());
    this.streams = [];
  }

  // The requests chosen, all of them by default, wait until the function returned is called.
  pause(requests: (request: string) => boolean = () => true): () => void {
    this.paused = new Hold(requests);

    return this.paused.release;
  }

  // The requests chosen are carried out, and their answers wait until the function returned is called.
  delayAnswers(requests: (request: string) => boolean): () => void {
    this.delayed = new Hold(requests);

    return this.delayed.release;
  }

  private stream(): Response {
    const body = new ReadableStream<Uint8Array>({ start: (controller) => void this.streams.push(controller) });

    return new Response(body, { headers: { "Content-Type": "text/event-stream" } });
  }

  private send(event: object) {
    const chunk = new TextEncoder().encode(`data: ${JSON.stringify(event)}\n\n`);

    this.streams.forEach((stream) => stream.enqueue(chunk));
  }
}

class Hold {
  release = () => {};
  private released = new Promise<void>((resolve) => (this.release = resolve));
  private readonly requests: (request: string) => boolean;

  constructor(requests: (request: string) => boolean) {
    this.requests = requests;
  }

  async wait(request: string) {
    if (this.requests(request)) {
      await this.released;
    }
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
  render(<App />);
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

async function openNote(title: string) {
  const user = await signInWith("right");

  await user.click(await screen.findByRole("button", { name: title }));
  await screen.findByRole("article");
  await vi.waitFor(() => expect(server.watchers).toBe(1));

  return user;
}

function noteList() {
  return within(screen.getByRole("navigation"));
}

it("shows the problem of a wrong password", async () => {
  await signInWith("wrong");

  expect(await screen.findByRole("alert")).toHaveProperty("textContent", "wrong user or password");
});

it("edits a note, then closes it once another tab removes it", async () => {
  const user = await openNote("Milk");

  expect(screen.getByText("Two bottles")).toBeDefined();

  await user.click(screen.getByRole("button", { name: "Edit" }));
  await user.clear(screen.getByLabelText("Text"));
  await user.type(screen.getByLabelText("Text"), "Three bottles");
  await user.click(screen.getByRole("button", { name: "Save" }));

  expect(await screen.findByText("Three bottles")).toBeDefined();
  expect(screen.queryByText(/changed elsewhere/)).toBeNull();

  server.remove(1);

  await vi.waitFor(() => expect(screen.getByRole("main").children).toHaveLength(0));
  await vi.waitFor(() => expect(noteList().queryByRole("button", { name: "Milk" })).toBeNull());
});

it("lists a note created after its form was left", async () => {
  const user = await signInWith("right");

  await user.click(await screen.findByRole("button", { name: "New note" }));
  await user.type(screen.getByLabelText("Title"), "Eggs");

  const resume = server.pause((request) => request === "POST /api/notes");

  await user.click(screen.getByRole("button", { name: "Save" }));
  await user.click(screen.getByRole("button", { name: "Cancel" }));
  resume();

  expect(await noteList().findByRole("button", { name: "Eggs" })).toBeDefined();
});

it("shows a change made between the note's read and the opening of its events", async () => {
  const user = await signInWith("right");
  const resume = server.pause((request) => request === "GET /api/notes/1/events");

  await user.click(await screen.findByRole("button", { name: "Milk" }));
  expect(await screen.findByText("Two bottles")).toBeDefined();

  // No one watches the note yet: its event is lost.
  server.change({ id: 1, title: "Milk", body: "Three bottles" });
  resume();

  expect(await screen.findByText("Three bottles")).toBeDefined();
});

it("shows a missing note's error at once, asking once", async () => {
  server.notes.push({ id: 2, title: "Eggs", body: "" });
  const user = await signInWith("right");

  const eggs = await screen.findByRole("button", { name: "Eggs" });

  server.notes.pop();
  await user.click(eggs);

  expect(await screen.findByText("no such note")).toBeDefined();
  expect(server.requests.filter((request) => request === "GET /api/notes/2")).toHaveLength(1);
});

it("signs out to the form once the server refuses the token", async () => {
  const user = await openNote("Milk");

  server.forgetToken();
  await user.click(screen.getByRole("button", { name: "Remove" }));

  expect(await screen.findByText("Your session ended: sign in again.")).toBeDefined();
  expect(screen.getByRole("button", { name: "Sign in" })).toBeDefined();
});

it("signs out to the form once a search is refused", async () => {
  const user = await signInWith("right");

  await noteList().findByRole("button", { name: "Milk" });
  server.forgetToken();
  await user.type(screen.getByLabelText("Find text"), "milk");
  await user.click(screen.getByRole("button", { name: "Search" }));

  expect(await screen.findByText("Your session ended: sign in again.")).toBeDefined();
});

it("signs out to the form when the note's events refuse the token", async () => {
  server.refuseEvents = true;
  await signInWith("right");

  await userEvent.click(await screen.findByRole("button", { name: "Milk" }));

  expect(await screen.findByText("Your session ended: sign in again.")).toBeDefined();
});

it("says when the note's events end, and follows them again", async () => {
  const user = await openNote("Milk");

  server.endEvents();
  await user.click(await screen.findByRole("button", { name: "Follow them again" }));

  expect(screen.queryByText(/no longer show/)).toBeNull();
  await vi.waitFor(() => expect(server.watchers).toBe(1));
  server.change({ id: 1, title: "Milk", body: "Four bottles" });
  expect(await screen.findByText("Four bottles")).toBeDefined();
});

it("shows the error of a failed refresh above the note", async () => {
  await openNote("Milk");

  // Gone without its event: the refresh that an edit elsewhere asks for finds no note.
  server.change({ id: 1, title: "Milk", body: "Two bottles" });
  server.notes = [];

  expect(await screen.findByText("no such note")).toBeDefined();
  expect(screen.getByRole("article")).toBeDefined();
});

it("says that a note being edited changed elsewhere, and reloads it", async () => {
  const user = await openNote("Milk");

  await user.click(screen.getByRole("button", { name: "Edit" }));
  await user.type(screen.getByLabelText("Text"), ", and cream");
  server.change({ id: 1, title: "Milk", body: "Three bottles" });
  await user.click(await screen.findByRole("button", { name: "Reload" }));

  expect(screen.getByLabelText("Text")).toHaveProperty("value", "Three bottles");
  expect(screen.queryByText(/changed elsewhere/)).toBeNull();
});

it("shows the note as it is once a slow save answers after a change elsewhere", async () => {
  const user = await openNote("Milk");

  await user.click(screen.getByRole("button", { name: "Edit" }));
  await user.type(screen.getByLabelText("Text"), ", and cream");

  const answer = server.delayAnswers((request) => request === "PUT /api/notes/1");

  await user.click(screen.getByRole("button", { name: "Save" }));
  await vi.waitFor(() => expect(server.notes[0].body).toBe("Two bottles, and cream"));

  // Another tab changes the note after the save, and its event is read before the save's answer.
  server.change({ id: 1, title: "Milk", body: "Four bottles" });
  expect(await screen.findByText(/changed elsewhere/)).toBeDefined();
  answer();

  expect(await screen.findByText("Four bottles")).toBeDefined();
  expect(screen.queryByText("Two bottles, and cream")).toBeNull();
});

it("pages through the notes, not past the last one", async () => {
  server.notes = Array.from({ length: PAGE_SIZE + 1 }, (_, i) => ({ id: i + 1, title: `Note ${i + 1}`, body: "" }));
  const user = await signInWith("right");
  const next = await noteList().findByRole("button", { name: "Next" });

  await vi.waitFor(() => expect(next).toHaveProperty("disabled", false));

  // While page 2 is on its way, page 1 still shows, and Next waits for it.
  const resume = server.pause();

  await user.click(next);
  expect(next).toHaveProperty("disabled", true);
  resume();

  await user.click(await noteList().findByRole("button", { name: "Note 21" }));
  await user.click(await screen.findByRole("button", { name: "Remove" }));

  // Page 2 came back empty: page 1 is the last one.
  expect(await noteList().findByText("Page 1")).toBeDefined();
  expect(noteList().getByRole("button", { name: "Note 20" })).toBeDefined();
});
