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
  const create = useNotesCreate();

  function save(note: Draft) {
    create.mutate(
      { data: note },
      {
        onSuccess: (created) => {
          queryClient.invalidateQueries({ queryKey: getNotesSearchQueryKey() });
          onCreated(created);
        },
      },
    );
  }

  return <NoteEditor note={EMPTY} saving={create.isPending} error={create.error} onSave={save} onCancel={onCancel} />;
}
