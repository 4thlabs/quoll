import { defineConfig } from "orval";

export default defineConfig({
  notes: {
    input: "./openapi.json",
    output: {
      target: "./src/api/generated.ts",
      client: "react-query",
      httpClient: "fetch",
      override: {
        mutator: { path: "./src/api/fetch.ts", name: "apiFetch" },
        // The hooks' data is the body, which the mutator returns, not orval's `{ data, status, headers }`.
        fetch: { includeHttpResponseReturnType: false },
      },
    },
  },
});
