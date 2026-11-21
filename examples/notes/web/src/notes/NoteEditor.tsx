import { type FormEvent, useState } from "react";

import type { NewNote } from "../api/generated";
import { ErrorMessage } from "../ErrorMessage";

type Props = {
  note: NewNote;
  saving: boolean;
  error: Error | null;
  onSave: (note: NewNote) => void;
  onCancel: () => void;
};

// The form of a note, new or edited: it keeps its own draft until it is saved, which is read-only while it is saved,
// and says when the note it started from changed elsewhere meanwhile, which saving overwrites.
export function NoteEditor({ note, saving, error, onSave, onCancel }: Props) {
  const [base, setBase] = useState(note);
  const [title, setTitle] = useState(note.title);
  const [body, setBody] = useState(note.body);
  // A note that came to read as the draft, as the echo of this very save does, is no conflict.
  const changedElsewhere = !same(note, base) && !same(note, { title, body });

  function reload() {
    setBase(note);
    setTitle(note.title);
    setBody(note.body);
  }

  function submit(event: FormEvent) {
    event.preventDefault();
    onSave({ title, body });
  }

  return (
    <form className="note-editor" onSubmit={submit}>
      {changedElsewhere && (
        <p role="status">
          This note changed elsewhere: saving overwrites it.{" "}
          <button type="button" onClick={reload}>
            Reload
          </button>
        </p>
      )}
      <label>
        Title
        <input value={title} onChange={(event) => setTitle(event.target.value)} readOnly={saving} required />
      </label>
      <label>
        Text
        <textarea value={body} onChange={(event) => setBody(event.target.value)} readOnly={saving} rows={10} />
      </label>
      <div className="actions">
        <button disabled={saving}>Save</button>
        <button type="button" onClick={onCancel}>
          Cancel
        </button>
      </div>
      <ErrorMessage error={error} />
    </form>
  );
}

function same(note: NewNote, other: NewNote): boolean {
  return note.title === other.title && note.body === other.body;
}
