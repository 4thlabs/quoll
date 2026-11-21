// Code of the TypeScript guide that the page does not use, type-checked with it: an `Events` route read with
// `EventSource`, for a same-origin app whose scheme is a cookie, which the browser sends itself.

import { ApiError } from "../api/fetch";
import type { Problem } from "../api/generated";

export type SourceHandlers<T> = {
  // Each event's data, parsed.
  onItem: (item: T) => void;
  // The stream is open, at first or again after a clean end: what changed meanwhile is not in it.
  onOpen?: () => void;
  // The source is closed for good: the problem that ended the stream, or nothing when it could not connect again.
  onEnd?: (error?: ApiError) => void;
};

// Follows an `Events` route until the function it returns is called.
export function followEvents<T>(
  url: string,
  { onItem, onOpen = () => {}, onEnd = () => {} }: SourceHandlers<T>,
): () => void {
  const source = new EventSource(url);

  source.onopen = () => onOpen();

  // The items, which are events with no name.
  source.onmessage = (event) => onItem(JSON.parse(event.data) as T);

  // `onmessage` sees no named event: the stream failed after its 200, and its data is the problem.
  source.addEventListener("problem", (event) => {
    const problem: Problem = JSON.parse(event.data);

    source.close();
    onEnd(new ApiError(problem.status ?? 500, problem));
  });

  // After a clean end the source connects again on its own, and goes on only when that is answered with a 200: any
  // other answer closes it, a 5xx of a proxy while the server restarts among them.
  source.onerror = () => {
    if (source.readyState === EventSource.CLOSED) {
      onEnd();
    }
  };

  return () => source.close();
}
