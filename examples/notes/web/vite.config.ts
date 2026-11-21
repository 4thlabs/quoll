import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      // The server's `url` in the workspace's Cargo.toml, `[[workspace.metadata.quoll.server]]`: the two change together.
      "/api": {
        target: "http://127.0.0.1:8080",
        // A stream the server drops, as it stops, ends for the page too: Vite's proxy would keep it open.
        configure: (proxy) => {
          proxy.on("proxyRes", (upstream, _request, response) => {
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
