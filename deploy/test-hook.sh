#!/usr/bin/env bash
# Run on Linux: bash deploy/test-hook.sh
set -euo pipefail
hook=$(cd "$(dirname "$0")" && pwd)/post-receive
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
export HOME="$scratch"
mkdir -p "$HOME/loveletter" "$HOME/source/server" "$HOME/bin"
git init -q --initial-branch=main "$HOME/source"
touch "$HOME/source/server/main.cpp"
git -C "$HOME/source" add .
git -C "$HOME/source" -c user.name=Test -c user.email=test@example.invalid commit -qm test
git clone -q --bare "$HOME/source" "$HOME/loveletter.git"
cat > "$HOME/bin/c++" <<'COMPILER'
#!/usr/bin/env bash
[[ "${FAIL_BUILD:-0}" == 0 ]] || exit 1
printf 'built\n' > "${!#}"
COMPILER
chmod +x "$HOME/bin/c++"
printf '#!/bin/sh\nexit 1\n' > "$HOME/bin/systemctl"
chmod +x "$HOME/bin/systemctl"
export PATH="$HOME/bin:$PATH"
cd "$HOME/loveletter.git"
printf 'old new refs/heads/feature\n' | bash "$hook"
test ! -e "$HOME/loveletter/server/out"
printf 'old new refs/heads/main\n' | bash "$hook"
test -x "$HOME/loveletter/server/out"
test "$(cat "$HOME/loveletter/server/out")" = built
printf 'previous\n' > "$HOME/loveletter/server/out"
if printf 'old new refs/heads/main\n' | FAIL_BUILD=1 bash "$hook"; then
    echo 'Expected the compilation failure to be reported' >&2
    exit 1
fi
test "$(cat "$HOME/loveletter/server/out")" = previous
test "$(find "$HOME/loveletter/server" -name 'out.*' | wc -l)" -eq 0
printf 'old 0000000000000000000000000000000000000000 refs/heads/main\n' | bash "$hook"
test "$(cat "$HOME/loveletter/server/out")" = previous
echo 'Hook checks passed'
