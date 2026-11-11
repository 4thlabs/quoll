#!/bin/bash
# Everything the page needs in development, in one command: the notes server, then Vite. See README.md.
exec "$(dirname "${BASH_SOURCE[0]}")/../../../tools/vite-plugin/dev.sh" "$(dirname "${BASH_SOURCE[0]}")" quoll_notes_server
