#!/bin/sh
# Build and install every nscode project that has an ns.mod.
#
#   nscode_apps.sh build   NS BINDIR   build each app and copy it beside NS
#   nscode_apps.sh install NS ROOT     the same into ROOT/bin, with its source
#                                      under ROOT/share/nscode/<project>
#
# An app is what `ns build` writes for its manifest: bin/<name>.app on Darwin,
# bin/<name>.AppImage or bin/<name> elsewhere. A project that keeps an
# executable launcher named after its manifest at its root (ns_tex) installs
# that launcher instead of building. `ns <word>` finds either one by the name
# ns_<word> or <word> beside ns.
set -eu
mode=${1:?mode}
ns=${2:?ns executable}
dest=${3:?destination}
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)

case "$mode" in
build) bindir=$dest ;;
install) bindir=$dest/bin ;;
*) echo "nscode_apps: unknown mode $mode" >&2; exit 2 ;;
esac
mkdir -p "$bindir"

manifest_name() {
    sed -n 's/^name[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$1" | head -n 1
}

# Replace $2 with a copy of $1, through a temporary name so a running copy is
# never left half written.
replace() {
    rm -rf "$2.new"
    cp -R "$1" "$2.new"
    rm -rf "$2"
    mv "$2.new" "$2"
}

for manifest in "$repo"/nscode/*/ns.mod; do
    [ -f "$manifest" ] || continue
    project=$(dirname "$manifest")
    dir=$(basename "$project")
    name=$(manifest_name "$manifest")
    [ -n "$name" ] || name=$dir
    if [ -f "$project/$name" ] && [ -x "$project/$name" ]; then
        replace "$project/$name" "$bindir/$name"
    else
        printf '  BUILD   nscode/%s\n' "$dir"
        "$ns" build "$project" >/dev/null
        found=0
        for artifact in "$project/bin/$name.app" "$project/bin/$name.AppImage" "$project/bin/$name"; do
            # bin/<name> may instead be a directory the app keeps state in.
            if [ -d "$artifact" ] && [ "${artifact%.app}" = "$artifact" ]; then
                continue
            fi
            if [ -e "$artifact" ]; then
                replace "$artifact" "$bindir/$(basename "$artifact")"
                found=1
                break
            fi
        done
        if [ "$found" -eq 0 ]; then
            echo "nscode_apps: nscode/$dir built no bin/$name app" >&2
            exit 1
        fi
    fi
    if [ "$mode" = install ]; then
        # The source too, for `ns <word>` on a host without the compiled app
        # and for tools that locate their project (ns_tex).
        share=$dest/share/nscode/$dir
        rm -rf "$share"
        mkdir -p "$share"
        (cd "$project" && tar -cf - --exclude ./bin --exclude ./test .) | (cd "$share" && tar -xf -)
    fi
done
