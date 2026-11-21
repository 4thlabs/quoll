import { QueryClientProvider } from "@tanstack/react-query";
import { useEffect, useState } from "react";

import { signOut } from "./api/fetch";
import { SignIn } from "./auth/SignIn";
import { NewNote } from "./notes/NewNote";
import { NoteList } from "./notes/NoteList";
import { NoteView } from "./notes/NoteView";
import { createQueryClient } from "./queryClient";

// `expired` when the server refused the token, which a restart of the server forgets.
type Session = "signed-in" | "signed-out" | "expired";

function expire(session: Session): Session {
  return session === "signed-in" ? "expired" : session;
}

export function App() {
  const [session, setSession] = useState<Session>("signed-out");
  const [queryClient] = useState(() => createQueryClient(() => setSession(expire)));

  // The token, and what was read with it, go with the session, whoever ends it.
  useEffect(() => {
    if (session !== "signed-in") {
      signOut();
      queryClient.clear();
    }
  }, [session, queryClient]);

  return (
    <QueryClientProvider client={queryClient}>
      {session === "signed-in" ? (
        <NotesPage onSignOut={() => setSession("signed-out")} onExpired={() => setSession(expire)} />
      ) : (
        <SignIn expired={session === "expired"} onSignedIn={() => setSession("signed-in")} />
      )}
    </QueryClientProvider>
  );
}

// What the right side shows: a note, the form of a new one, or nothing.
type Selection = { id: number } | "new" | undefined;

type NotesPageProps = {
  onSignOut: () => void;
  onExpired: () => void;
};

function NotesPage({ onSignOut, onExpired }: NotesPageProps) {
  const [selection, setSelection] = useState<Selection>();

  function closeNote() {
    setSelection(undefined);
  }

  return (
    <div className="app">
      <header>
        <h1>Notes</h1>
        <button onClick={onSignOut}>Sign out</button>
      </header>
      <NoteList
        selected={typeof selection === "object" ? selection.id : undefined}
        onSelect={(id) => setSelection({ id })}
        onNew={() => setSelection("new")}
      />
      <main>
        {selection === "new" && <NewNote onCreated={(note) => setSelection({ id: note.id })} onCancel={closeNote} />}
        {typeof selection === "object" && (
          <NoteView key={selection.id} id={selection.id} onClosed={closeNote} onExpired={onExpired} />
        )}
      </main>
    </div>
  );
}
