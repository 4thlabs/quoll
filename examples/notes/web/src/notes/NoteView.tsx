import { useQueryClient } from "@tanstack/react-query";
import { useEffect, useEffectEvent, useState } from "react";

import { apiEvents } from "../api/fetch";
import {
  getNotesEventsUrl,
  getNotesGetQueryKey,
  getNotesSearchQueryKey,
  type NewNote,
  type Note,
  type NoteEvent,
  useNotesGet,
  useNotesRemove,
  useNotesUpdate,
} from "../api/generated";
import { ErrorMessage } from "../ErrorMessage";
import { NoteEditor } from "./NoteEditor";

type Props = {
  id: number;
  onClosed: () => void;
};

export function NoteView({ id, onClosed }: Props) {
  const queryClient = useQueryClient();
  const { data: note, error, refetch } = useNotesGet(id);
  const update = useNotesUpdate();
  const remove = useNotesRemove();
  const [editing, setEditing] = useState(false);

  function listChanged() {
    queryClient.invalidateQueries({ queryKey: getNotesSearchQueryKey() });
  }

  // Another tab or the command line changed the note: read it again, or close it once it is removed.
  const changed = useEffectEvent((event: NoteEvent) => {
    listChanged();

    if (isRemoved(event)) {
      onClosed();
    } else {
      refetch();
    }
  });

  useEffect(() => apiEvents<NoteEvent>(getNotesEventsUrl(id), changed), [id]);

  if (error) {
    return <ErrorMessage error={error} />;
  }

  if (!note) {
    return <p>Loading</p>;
  }

  function save(changes: NewNote) {
    update.mutate(
      { id, data: changes },
      {
        onSuccess: (saved) => {
          queryClient.setQueryData(getNotesGetQueryKey(id), saved);
          listChanged();
          setEditing(false);
        },
      },
    );
  }

  function removeNote() {
    remove.mutate(
      { id },
      {
        onSuccess: () => {
          listChanged();
          onClosed();
        },
      },
    );
  }

  if (editing) {
    return (
      <NoteEditor
        note={note}
        saving={update.isPending}
        error={update.error}
        onSave={save}
        onCancel={() => setEditing(false)}
      />
    );
  }

  return <NoteText note={note} error={remove.error} onEdit={() => setEditing(true)} onRemove={removeNote} />;
}

// orval types the `edited` event as the plain `Note` it holds, without its `kind`.
function isRemoved(event: NoteEvent): event is Extract<NoteEvent, { kind: "removed" }> {
  return "kind" in event && event.kind === "removed";
}

type NoteTextProps = {
  note: Note;
  error: Error | null;
  onEdit: () => void;
  onRemove: () => void;
};

function NoteText({ note, error, onEdit, onRemove }: NoteTextProps) {
  return (
    <article className="note">
      <h2>{note.title}</h2>
      <p className="body">{note.body}</p>
      <div className="actions">
        <button onClick={onEdit}>Edit</button>
        <button onClick={onRemove}>Remove</button>
      </div>
      <ErrorMessage error={error} />
    </article>
  );
}
