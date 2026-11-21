import { useQueryClient } from "@tanstack/react-query";

import { getNotesSearchQueryKey, type NewNote as Draft, type Note, useNotesCreate } from "../api/generated";
import { NoteEditor } from "./NoteEditor";

const EMPTY: Draft = { title: "", body: "" };

type Props = {
  onCreated: (note: Note) => void;
  onCancel: () => void;
};

export function NewNote({ onCreated, onCancel }: Props) {
  const queryClient = useQueryClient();
  // The list follows a new note even when the form is gone before the answer, which `mutate`'s callbacks miss.
  const create = useNotesCreate({
    mutation: { onSuccess: () => queryClient.invalidateQueries({ queryKey: getNotesSearchQueryKey() }) },
  });

  function save(note: Draft) {
    create.mutate({ data: note }, { onSuccess: onCreated });
  }

  return <NoteEditor note={EMPTY} saving={create.isPending} error={create.error} onSave={save} onCancel={onCancel} />;
}
