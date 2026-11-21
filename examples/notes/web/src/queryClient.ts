import { MutationCache, QueryCache, QueryClient } from "@tanstack/react-query";

import { ApiError, isExpired } from "./api/fetch";

// An error below 500 answers the same when asked again: a missing note, a refused token.
function retryServerErrors(failures: number, error: Error): boolean {
  if (error instanceof ApiError && error.status < 500) {
    return false;
  }

  return failures < 3;
}

// The page's client: `onExpired` hears of every request whose token, the one the page holds, is refused.
export function createQueryClient(onExpired: () => void): QueryClient {
  function signOutWhenExpired(error: Error) {
    if (isExpired(error)) {
      onExpired();
    }
  }

  return new QueryClient({
    queryCache: new QueryCache({ onError: signOutWhenExpired }),
    mutationCache: new MutationCache({ onError: signOutWhenExpired }),
    defaultOptions: { queries: { retry: retryServerErrors } },
  });
}
