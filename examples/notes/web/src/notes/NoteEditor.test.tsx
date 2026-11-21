import { cleanup, render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, expect, it } from "vitest";

import type { NewNote } from "../api/generated";
import { NoteEditor } from "./NoteEditor";

afterEach(cleanup);

function editor(note: NewNote) {
  return <NoteEditor note={note} saving={false} error={null} onSave={() => {}} onCancel={() => {}} />;
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
