import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      // The server's `url` in the workspace's Cargo.toml, `[[workspace.metadata.quoll.server]]`: the two change together.
      "/api": {
        target: "http://127.0.0.1:8080",
        // Vite's proxy holds the head of a stream until its first event, and keeps the stream open once the server
        // drops it, as it stops: the page would see it neither start nor end.
        configure: (proxy) => {
          proxy.on("proxyRes", (upstream, _request, response) => {
            if (upstream.headers["content-type"]?.startsWith("text/event-stream")) {
              queueMicrotask(() => response.flushHeaders()); // once the proxy has written the head
            }

            upstream.on("close", () => {
              if (!upstream.complete) {
                response.destroy();
              }
            });
          });
        },
      },
    },
  },
});
