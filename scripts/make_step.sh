#!/bin/sh
# One quiet make step: run a command, keep its output out of the way, and print
# the command plus everything it wrote only when it fails. `make V=1` bypasses
# this script and echoes the raw commands instead.
#
# usage: make_step.sh TAG LABEL MODE [command...]
#   MODE show   print "TAG LABEL" once the command succeeds
#        quiet  print nothing on success unless the command wrote output
#        hide   print nothing on success (third-party sources we do not fix)
#        say    run nothing; print "TAG LABEL"
# NO_COLOR disables colour; NS_COLOR=1 forces it through pipes.

tag=$1
label=$2
mode=$3
shift 3

paint() {
    # paint FD CODE TEXT
    if [ -z "${NO_COLOR:-}" ] && { [ "${NS_COLOR:-}" = 1 ] || [ -t "$1" ]; }; then
        printf '\033[%sm%s\033[0m' "$2" "$3"
    else
        printf '%s' "$3"
    fi
}

tag_line() {
    # tag_line FD CODE TAG LABEL
    paint "$1" "$2" "$(printf '%-7s' "$3")"
    printf ' %s\n' "$4"
}

case $tag in
    FAIL|ERROR) code='1;31' ;;
    WARN|NOTE) code='1;33' ;;
    OK|DONE|INSTALL) code='1;32' ;;
    LINK|AR|BUILD) code='1;34' ;;
    *) code='1;36' ;;
esac

if [ "$mode" = say ]; then
    tag_line 1 "$code" "$tag" "$label"
    exit 0
fi

out=$("$@" 2>&1)
status=$?

if [ "$status" -ne 0 ]; then
    {
        tag_line 2 '1;31' FAIL "$label"
        paint 2 '2' "\$ $*"
        printf '\n'
        [ -n "$out" ] && printf '%s\n' "$out"
    } >&2
    exit "$status"
fi

if [ -n "$out" ] && [ "$mode" != hide ]; then
    tag_line 1 '1;33' WARN "$label"
    printf '%s\n' "$out"
elif [ "$mode" = show ]; then
    tag_line 1 "$code" "$tag" "$label"
fi
