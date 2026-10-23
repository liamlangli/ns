#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh lib/src/os.git.build.sh macosx bin/git-deps arm64
flags="-DNS_GIT_EMBEDDED=1 -DNS_DARWIN=1 -Iinclude -Ilib/include -Ibin/git-deps/macosx-arm64/include
    -Wall -Wextra -Werror -Wno-missing-field-initializers -fsanitize=address,undefined -g"
libs="bin/git-deps/macosx-arm64/libns_git.a -framework Security -framework CoreFoundation -liconv -lz"
cc $flags test/os_git_embedded_check.c lib/src/os.git.c $libs -o bin/git-deps/os_git_test
bin/git-deps/os_git_test
cc $flags test/os_ssh_embedded_check.c lib/src/os.ssh.c lib/src/os.git.c $libs -o bin/git-deps/os_ssh_test
# A throwaway unprivileged sshd on loopback accepts one generated key.
work=$(mktemp -d /tmp/ns-sshd-XXXXXX)
trap 'if [ -f "$work/pid" ]; then kill "$(cat "$work/pid")" 2>/dev/null || true; fi; rm -rf "$work"' EXIT
ssh-keygen -q -t ed25519 -N '' -f "$work/host"
ssh-keygen -q -t ed25519 -N '' -f "$work/user"
cp "$work/user.pub" "$work/authorized_keys"
cat > "$work/sshd_config" <<CONFIG
HostKey $work/host
AuthorizedKeysFile $work/authorized_keys
PidFile $work/pid
ListenAddress 127.0.0.1
StrictModes no
UsePAM no
PasswordAuthentication no
KbdInteractiveAuthentication no
CONFIG
port=$((23000 + $$ % 1000))
/usr/sbin/sshd -f "$work/sshd_config" -p "$port" -E "$work/log"
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -f "$work/pid" ] && break; sleep 0.2; done
host=$(ssh-keygen -l -E sha256 -f "$work/host.pub" | awk '{print $2}')
NS_SSH_TEST_PORT=$port NS_SSH_TEST_KEY="$work/user" NS_SSH_TEST_AUTH="$work/authorized_keys" NS_SSH_TEST_HOST=$host bin/git-deps/os_ssh_test
