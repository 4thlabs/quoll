export function ErrorMessage({ error }: { error: Error | null }) {
  if (!error) {
    return null;
  }

  return <p role="alert">{error.message}</p>;
}
