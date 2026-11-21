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

// The form of a note, new or edited: it keeps its own draft until it is saved.
export function NoteEditor({ note, saving, error, onSave, onCancel }: Props) {
  const [title, setTitle] = useState(note.title);
  const [body, setBody] = useState(note.body);

  function submit(event: FormEvent) {
    event.preventDefault();
    onSave({ title, body });
  }

  return (
    <form className="note-editor" onSubmit={submit}>
      <label>
        Title
        <input value={title} onChange={(event) => setTitle(event.target.value)} required />
      </label>
      <label>
        Text
        <textarea value={body} onChange={(event) => setBody(event.target.value)} rows={10} />
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
