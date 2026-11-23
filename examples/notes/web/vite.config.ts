import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

export default defineConfig({
  plugins: [react()],
  // `.vite/manifest.json` names the files Vite hashed, which the server's `.page(..)` keeps a year.
  build: { manifest: true },
  server: {
    proxy: {
      // The server's `url` in the workspace's Cargo.toml, `[[workspace.metadata.quoll.server]]`: the two change together.
      "/api": "http://127.0.0.1:8080",
    },
  },
});
