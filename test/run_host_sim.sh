#!/bin/sh
# Runs the whole plugin on a Linux PC with a fake game (see host_sim.c). No PS4 and no SDK needed.
#   test/run_host_sim.sh          run the checks
#   test/run_host_sim.sh serve    afterwards keep the web page up on port 8080 for 40 s (open it in a browser)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# the plugin writes to /data/GoldHEN/dimensions on the PS4: point it at a temp folder here
mkdir -p "$WORK/src" "$WORK/data"
cp "$ROOT"/source/*.c "$ROOT"/source/*.h "$WORK/src/"
sed -i "s#/data/GoldHEN/dimensions#$WORK/data#g" "$WORK/src/main.c"

gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
    -I"$WORK/src" -I"$ROOT/test/host" -o "$WORK/host_sim" "$ROOT/test/host_sim.c" "$WORK/src/portal.c" -lpthread
"$WORK/host_sim" "$@"
