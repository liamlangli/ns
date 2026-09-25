#!/bin/sh

# `ns patch` writes an eval or emu target's over-the-air patch, and
# `ns run --patch` installs a published one before it runs (doc/patch.md). The
# patch is served by ns itself, through the http module's static file server.

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

ns=$1
patch_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ns-patch-test.XXXXXX")
server_pid=
cleanup() {
    if [ -n "$server_pid" ]; then kill "$server_pid" 2>/dev/null || true; fi
    rm -rf "$patch_tmp"
}
trap cleanup EXIT HUP INT TERM

port=$((20000 + $$ % 20000))
url="http://127.0.0.1:$port/site/demo.nsapp"
export NS_PATCH_DIR="$patch_tmp/cache"

mkdir -p "$patch_tmp/demo/src" "$patch_tmp/demo/res"
printf '%s\n' \
    'schema = "ns.mod/v2"' \
    'name = "demo"' \
    'version = "1.0.0"' \
    'type = "cli"' \
    'source = "src"' \
    'entry = "main.ns"' \
    'target = "eval"' \
    'assets = ["res"]' \
    "patch = \"$url\"" > "$patch_tmp/demo/ns.mod"
write_main() {
    printf '%s\n' \
        'use std' \
        'use os' \
        '' \
        'fn main() {' \
        "    print(\`$1 patch={os_patch_version()} {os_read_file(\"res/note.txt\")}\\n\`)" \
        '}' > "$patch_tmp/demo/src/main.ns"
}
write_main old
printf 'shipped' > "$patch_tmp/demo/res/note.txt"
# The client is the project as it was shipped.
cp -R "$patch_tmp/demo" "$patch_tmp/client"

write_main new
printf 'patched' > "$patch_tmp/demo/res/note.txt"
out=$(cd "$patch_tmp/demo" && "$ns" patch)
printf '%s\n' "$out" | grep -q 'demo patch 1 (eval)'
test -f "$patch_tmp/demo/bin/demo_patch/demo.nsapp"
test "$(ls "$patch_tmp/demo/bin/demo_patch" | grep -c '\.nsbundle$')" -eq 2
out=$(cd "$patch_tmp/demo" && "$ns" patch)
printf '%s\n' "$out" | grep -q 'unchanged; still patch 1'

# Publish it and let ns serve it.
mkdir -p "$patch_tmp/www/site"
cp "$patch_tmp/demo/bin/demo_patch/"* "$patch_tmp/www/site/"
printf 'use http\nfn main() {\n    let r = http_serve_static(%s, "%s")\n}\n' "$port" "$patch_tmp/www" > "$patch_tmp/serve.ns"
"$ns" run "$patch_tmp/serve.ns" > "$patch_tmp/serve.log" 2>&1 &
server_pid=$!
tries=0
until (cd "$patch_tmp/client" && "$ns" run --patch) > "$patch_tmp/run.log" 2>&1 && grep -q 'new patch=1 patched' "$patch_tmp/run.log"; do
    tries=$((tries + 1))
    if [ "$tries" -ge 50 ]; then
        cat "$patch_tmp/run.log" "$patch_tmp/serve.log" >&2
        exit 1
    fi
    sleep 0.1
done
grep -q 'installing patch 1' "$patch_tmp/run.log"

# Without --patch the project source runs as it is.
(cd "$patch_tmp/client" && "$ns" run) | grep -q '^old patch=0 shipped$'

# Offline, the installed patch keeps running.
(cd "$patch_tmp/client" && NS_PATCH_URL=http://127.0.0.1:1/demo.nsapp "$ns" run --patch) | grep -q '^new patch=1 patched$'

# Only interpreted targets take patches.
sed 's/^target = "eval"$/target = "exec"/' "$patch_tmp/demo/ns.mod" > "$patch_tmp/demo/ns.mod.exec"
cp "$patch_tmp/demo/ns.mod.exec" "$patch_tmp/demo/ns.mod"
if (cd "$patch_tmp/demo" && "$ns" patch) > "$patch_tmp/exec.log" 2>&1; then
    echo "ns patch accepted an exec target" >&2
    exit 1
fi
grep -q 'patches carry interpreted code' "$patch_tmp/exec.log"

# An emu patch carries the ns_cpu image.
sed 's/^target = "exec"$/target = "emu"/' "$patch_tmp/demo/ns.mod.exec" > "$patch_tmp/demo/ns.mod"
(cd "$patch_tmp/demo" && "$ns" patch) | grep -q 'demo patch 2 (emu)'
