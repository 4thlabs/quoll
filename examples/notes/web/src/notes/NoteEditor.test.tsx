import { cleanup, render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, expect, it } from "vitest";

import type { NewNote } from "../api/generated";
import { NoteEditor } from "./NoteEditor";

afterEach(cleanup);

function editor(note: NewNote, saving = false) {
  return <NoteEditor note={note} saving={saving} error={null} onSave={() => {}} onCancel={() => {}} />;
}

it("sees no change elsewhere in a note that reads as the draft", async () => {
  const { rerender } = render(editor({ title: "Milk", body: "Two bottles" }));

  await userEvent.type(screen.getByLabelText("Text"), ", and cream");

  // The echo of this very save, read again before its answer.
  rerender(editor({ title: "Milk", body: "Two bottles, and cream" }));
  expect(screen.queryByText(/changed elsewhere/)).toBeNull();

  rerender(editor({ title: "Milk", body: "Three bottles" }));
  expect(screen.getByText(/changed elsewhere/)).toBeDefined();
});

it("keeps the draft as it is sent while it is saved", async () => {
  render(editor({ title: "Milk", body: "Two bottles" }, true));

  await userEvent.type(screen.getByLabelText("Text"), ", and cream");
  await userEvent.type(screen.getByLabelText("Title"), "!");

  expect(screen.getByLabelText("Text")).toHaveProperty("value", "Two bottles");
  expect(screen.getByLabelText("Title")).toHaveProperty("value", "Milk");
});
