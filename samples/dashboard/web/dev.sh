#!/bin/bash
# Everything the page needs in development, in one command: the dashboard server, then Vite. See README.md.
exec "$(dirname "${BASH_SOURCE[0]}")/../../../tools/vite-plugin/dev.sh" "$(dirname "${BASH_SOURCE[0]}")" quoll_dashboard_server
