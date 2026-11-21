import { useQueryClient } from "@tanstack/react-query";
import { useState } from "react";

import { signOut } from "./api/fetch";
import { SignIn } from "./auth/SignIn";
import { NewNote } from "./notes/NewNote";
import { NoteList } from "./notes/NoteList";
import { NoteView } from "./notes/NoteView";

// What the right side shows: a note, the form of a new one, or nothing.
type Selection = { id: number } | "new" | undefined;

export function App() {
  const queryClient = useQueryClient();
  const [signedIn, setSignedIn] = useState(false);
  const [selection, setSelection] = useState<Selection>();

  if (!signedIn) {
    return <SignIn onSignedIn={() => setSignedIn(true)} />;
  }

  function leave() {
    signOut();
    queryClient.clear();
    setSelection(undefined);
    setSignedIn(false);
  }

  function closeNote() {
    setSelection(undefined);
  }

  return (
    <div className="app">
      <header>
        <h1>Notes</h1>
        <button onClick={leave}>Sign out</button>
      </header>
      <NoteList
        selected={typeof selection === "object" ? selection.id : undefined}
        onSelect={(id) => setSelection({ id })}
        onNew={() => setSelection("new")}
      />
      <main>
        {selection === "new" && <NewNote onCreated={(note) => setSelection({ id: note.id })} onCancel={closeNote} />}
        {typeof selection === "object" && <NoteView key={selection.id} id={selection.id} onClosed={closeNote} />}
      </main>
    </div>
  );
}
