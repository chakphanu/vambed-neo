#!/usr/bin/env bash
# Scan the working tree and git history for data that must not be published:
# home paths, emails, public IPv4 addresses, device paths/serials, ELF binaries,
# and any extra words listed one per line in .local/pii-words (gitignored),
# e.g. personal callsigns or host names.
# Exit 0 = clean, 1 = findings.

set -u
cd "$(dirname "$0")/.." || exit 2

fail=0
report() { echo "pii-check: $1"; fail=1; }

# Tracked + untracked-but-not-ignored files, excluding this script itself
mapfile -t FILES < <(git ls-files -co --exclude-standard | grep -v '^scripts/pii-check\.sh$')

scan() {   # $1 = label, $2 = extended regex
  [ "${#FILES[@]}" -gt 0 ] || return 0
  local hits
  hits=$(grep -nIE -- "$2" "${FILES[@]}" 2>/dev/null | head -20)
  [ -z "$hits" ] || report "$1"$'\n'"$hits"
}

scan "home path" '/home/[a-z_][a-z0-9_-]*|/Users/[A-Za-z]'
scan "device path" '/dev/bus/usb|/dev/serial/by-id|/dev/tty(USB|ACM)[0-9]'
scan "device serial" '[Dd]evice[-_ ]?[Ss]erial[^A-Za-z]*[:=][[:space:]]*["'"'"']?[A-Za-z0-9]{6,}'

# Emails other than the allowed noreply forms
emails=$(grep -noIE -- '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}' "${FILES[@]}" 2>/dev/null \
  | grep -vE '@users\.noreply\.github\.com|noreply@anthropic\.com|@example\.(com|org|net)' | head -20)
[ -z "$emails" ] || report "email"$'\n'"$emails"

# IPv4 outside loopback, private, documentation and special ranges
ips=$(grep -noIE -- '\b([0-9]{1,3}\.){3}[0-9]{1,3}\b' "${FILES[@]}" 2>/dev/null | while IFS= read -r line; do
  ip=${line##*:}
  IFS=. read -r a b c d <<<"$ip"
  [ "$a" -le 255 ] && [ "$b" -le 255 ] && [ "$c" -le 255 ] && [ "$d" -le 255 ] || continue
  case "$a" in
    0|10|127|255) continue ;;
    172) [ "$b" -ge 16 ] && [ "$b" -le 31 ] && continue ;;
    192) { [ "$b" = 168 ] || { [ "$b" = 0 ] && [ "$c" = 2 ]; }; } && continue ;;
    198) { [ "$b" = 51 ] && [ "$c" = 100 ]; } && continue ;;
    203) { [ "$b" = 0 ] && [ "$c" = 113 ]; } && continue ;;
    224|225|226|227|228|229|230|231|232|233|234|235|236|237|238|239) continue ;;
  esac
  echo "$line"
done | head -20)
[ -z "$ips" ] || report "public IPv4"$'\n'"$ips"

# ELF binaries
for f in "${FILES[@]}"; do
  [ -f "$f" ] || continue
  [ "$(head -c 4 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')" = "7f454c46" ] && report "ELF binary: $f"
done

# Local extra words (callsigns, host names): never committed themselves
if [ -f .local/pii-words ]; then
  while IFS= read -r w; do
    [ -n "$w" ] && [ "${w#\#}" = "$w" ] || continue
    hits=$(grep -nIiF -- "$w" "${FILES[@]}" 2>/dev/null | head -5)
    [ -z "$hits" ] || report "local word '$w'"$'\n'"$hits"
    git log --all --format='%H %an %ae %cn %ce %B' 2>/dev/null | grep -qiF -- "$w" \
      && report "local word '$w' in git history"
  done < .local/pii-words
fi

# Git identities must be noreply
bad=$(git log --all --format='%ae%n%ce' 2>/dev/null | sort -u \
  | grep -vE '@users\.noreply\.github\.com$|^noreply@anthropic\.com$')
[ -z "$bad" ] || report "non-noreply git identity"$'\n'"$bad"

[ "$fail" = 0 ] && echo "pii-check: clean (${#FILES[@]} files)"
exit "$fail"
