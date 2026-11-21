import { MutationCache, QueryCache, QueryClient } from "@tanstack/react-query";

import { ApiError, isUnauthorized } from "./api/fetch";

// An error below 500 answers the same when asked again: a missing note, a refused token.
function retryServerErrors(failures: number, error: Error): boolean {
  if (error instanceof ApiError && error.status < 500) {
    return false;
  }

  return failures < 3;
}

// The page's client: `onUnauthorized` hears of every request whose token is refused.
export function createQueryClient(onUnauthorized: () => void): QueryClient {
  function signOutWhenUnauthorized(error: Error) {
    if (isUnauthorized(error)) {
      onUnauthorized();
    }
  }

  return new QueryClient({
    queryCache: new QueryCache({ onError: signOutWhenUnauthorized }),
    mutationCache: new MutationCache({ onError: signOutWhenUnauthorized }),
    defaultOptions: { queries: { retry: retryServerErrors } },
  });
}
