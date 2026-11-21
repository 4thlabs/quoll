import { keepPreviousData } from "@tanstack/react-query";
import { type FormEvent, useEffect, useState } from "react";

import { type NotesSearchParams, useNotesSearch } from "../api/generated";
import { ErrorMessage } from "../ErrorMessage";

// The server's page, as the contract's `NoteFilter` says: a shorter one is the last.
const PAGE_SIZE = 20;

type Props = {
  selected: number | undefined;
  onSelect: (id: number) => void;
  onNew: () => void;
};

export function NoteList({ selected, onSelect, onNew }: Props) {
  const [filter, setFilter] = useState<NotesSearchParams>({});
  const [page, setPage] = useState(1);
  // The page shown stays until the next one arrives, rather than blink.
  const { data, error, isPlaceholderData } = useNotesSearch(
    { ...filter, page },
    { query: { placeholderData: keepPreviousData } },
  );
  const notes = data ?? [];

  // The last notes of the last page went, removed here or elsewhere: the page before is the last one now.
  useEffect(() => {
    if (page > 1 && !isPlaceholderData && data?.length === 0) {
      setPage(page - 1);
    }
  }, [page, isPlaceholderData, data]);

  function search(next: NotesSearchParams) {
    setFilter(next);
    setPage(1);
  }

  return (
    <nav className="note-list">
      <SearchForm onSearch={search} />
      <button onClick={onNew}>New note</button>
      <ErrorMessage error={error} />
      <ul>
        {notes.map((note) => (
          <li key={note.id}>
            <button className={note.id === selected ? "selected" : ""} onClick={() => onSelect(note.id)}>
              {note.title}
            </button>
          </li>
        ))}
      </ul>
      <div className="pages">
        <button disabled={page === 1} onClick={() => setPage(page - 1)}>
          Previous
        </button>
        <span>Page {page}</span>
        <button disabled={notes.length < PAGE_SIZE || isPlaceholderData} onClick={() => setPage(page + 1)}>
          Next
        </button>
      </div>
    </nav>
  );
}

function SearchForm({ onSearch }: { onSearch: (filter: NotesSearchParams) => void }) {
  const [contains, setContains] = useState("");
  const [tag, setTag] = useState("");

  // A key left empty does not narrow the search: it is not sent.
  function submit(event: FormEvent) {
    event.preventDefault();
    onSearch({ contains: contains || undefined, tag: tag || undefined });
  }

  return (
    <form className="search" onSubmit={submit}>
      <input
        aria-label="Find text"
        placeholder="Text"
        maxLength={1000}
        value={contains}
        onChange={(event) => setContains(event.target.value)}
      />
      <input aria-label="Tag" placeholder="#tag" value={tag} onChange={(event) => setTag(event.target.value)} />
      <button>Search</button>
    </form>
  );
}
