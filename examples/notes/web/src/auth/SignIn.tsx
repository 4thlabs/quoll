import { type FormEvent, useState } from "react";

import { signIn } from "../api/fetch";
import { useAuthToken } from "../api/generated";
import { ErrorMessage } from "../ErrorMessage";

export function SignIn({ onSignedIn }: { onSignedIn: () => void }) {
  const [user, setUser] = useState("demo");
  const [password, setPassword] = useState("");
  const token = useAuthToken();

  function submit(event: FormEvent) {
    event.preventDefault();
    token.mutate(
      { data: { user, password } },
      {
        onSuccess: ({ access_token }) => {
          signIn(access_token);
          onSignedIn();
        },
      },
    );
  }

  return (
    <form className="sign-in" onSubmit={submit}>
      <h1>Notes</h1>
      <label>
        User
        <input value={user} onChange={(event) => setUser(event.target.value)} />
      </label>
      <label>
        Password
        <input type="password" value={password} onChange={(event) => setPassword(event.target.value)} />
      </label>
      <p className="hint">The server prints the demo's password when it starts.</p>
      <button disabled={token.isPending}>Sign in</button>
      <ErrorMessage error={token.error} />
    </form>
  );
}
