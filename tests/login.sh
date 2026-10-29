#!/usr/bin/env bash
# Run in the privileged builder container with qemu-arm binfmt registered.
# Tests a disposable rootfs copy; no host keys or login state enter the image.
set -euo pipefail
SOURCE=${1:-/work/build/rootfs}
T=$(mktemp -d)
R=$T/rootfs
sshd_pid=
mounted=()
cleanup() {
    if [ -n "$sshd_pid" ]; then kill "$sshd_pid" 2>/dev/null || true; wait "$sshd_pid" 2>/dev/null || true; fi
    local i
    for ((i=${#mounted[@]}-1; i>=0; i--)); do umount "${mounted[i]}"; done
    rm -rf "$T"
}
trap cleanup EXIT
cp -a "$SOURCE" "$R"
python3 - "$R" <<'PY'
import crypt
import pathlib
import sys
r = pathlib.Path(sys.argv[1])
users = [line.split(':') for line in (r/'etc/passwd').read_text().splitlines()]
by_name = {u[0]: u for u in users}
shadow = dict(line.split(':', 1) for line in (r/'etc/shadow').read_text().splitlines())
passwords = {name: value.split(':')[0] for name, value in shadow.items()}
assert users[0][0] == 'OmniCore', 'UID 0 must resolve to OmniCore'
assert by_name['OmniCore'][2:4] == ['0', '0']
assert by_name['OmniCore'][5:7] == ['/root', '/bin/bash']
assert crypt.crypt('root', passwords['OmniCore']) == passwords['OmniCore']
assert by_name['root'][2] == '0' and by_name['root'][6] == '/usr/sbin/nologin'
assert passwords['root'].startswith(('!', '*'))
assert 'OmniCore-SDR' not in by_name and 'analog' not in by_name
unlocked = [u[0] for u in users if passwords.get(u[0]) and not passwords[u[0]].startswith(('!', '*'))]
assert unlocked == ['OmniCore'], unlocked
print('PASS: sole unlocked account OmniCore, password root, UID/GID 0, /root home, locked root identity')
PY
mkdir -p "$R/run/sshd"
mount -t proc proc "$R/proc"; mounted+=("$R/proc")
mount --bind /dev "$R/dev"; mounted+=("$R/dev")
mount -t devpts devpts "$R/dev/pts"; mounted+=("$R/dev/pts")
chroot "$R" /usr/sbin/visudo -c
test "$(chroot "$R" /usr/bin/whoami)" = OmniCore
chroot "$R" /usr/bin/ssh-keygen -q -t ed25519 -N '' -f /tmp/login-test-key
printf '#!/bin/sh\nprintf "root\\n"\n' > "$R/tmp/login-test-askpass"
chmod 0700 "$R/tmp/login-test-askpass"
chroot "$R" /usr/sbin/sshd -T -h /tmp/login-test-key > "$T/config"
grep -qx 'allowusers OmniCore' "$T/config"
grep -qx 'permitrootlogin yes' "$T/config"
grep -qx 'passwordauthentication yes' "$T/config"
chroot "$R" /usr/sbin/sshd -D -e -p 22222 -h /tmp/login-test-key \
    -o ListenAddress=127.0.0.1 > "$T/server.log" 2>&1 &
sshd_pid=$!
for attempt in 1 2 3 4 5; do
    if grep -q 'Server listening' "$T/server.log"; then break; fi
    sleep 1
done
client() {
    chroot "$R" /usr/bin/env DISPLAY=:0 SSH_ASKPASS_REQUIRE=force \
        SSH_ASKPASS=/tmp/login-test-askpass /usr/bin/ssh -p 22222 \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o PreferredAuthentications=password -o PubkeyAuthentication=no \
        -o NumberOfPasswordPrompts=1 -o ConnectTimeout=5 "$@" < /dev/null
}
client OmniCore@127.0.0.1 'id -un; id -u; printf "%s\n" "$HOME"; sudo -n id -u' > "$T/login.out" 2> "$T/login.err" || {
    cat "$T/login.err" "$T/server.log"; exit 1;
}
printf 'OmniCore\n0\n/root\n0\n' > "$T/expected"
cmp "$T/login.out" "$T/expected"
grep -Fq 'OmniCore SDR' "$T/login.err"
echo 'PASS: actual ARM SSH password login, whoami, UID 0, HOME, sudo and banner'
for user in root OmniCore-SDR omnicore; do
    if client "$user@127.0.0.1" true > "$T/denied.out" 2> "$T/denied.err"; then
        echo "ERROR: unexpected login accepted for $user"; exit 1
    fi
    grep -Fq 'Permission denied' "$T/denied.err"
    echo "PASS: login rejected for $user"
done
