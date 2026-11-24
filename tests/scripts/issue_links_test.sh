#!/usr/bin/env bash
# Tests scripts/issue-links.sh against a fake `gh`, so that no network or login is needed.
# shellcheck disable=SC2001
set -uo pipefail

script="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/issue-links.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
failures=0

cat >"$work/gh" <<'FAKE'
#!/usr/bin/env bash
# Answers the calls of issue-links.sh from the environment: FAKE_GH_JSON is the reply to a query
# of blockers or relations, FAKE_GH_ERROR makes a mutation fail with that message.
case "$1 $2" in
"repo view") echo "acme widgets"; exit 0 ;;
esac
number=""
query=""
while [ $# -gt 0 ]; do
    case "$1" in
    -F) number="${2#number=}"; shift ;;
    -f) case "$2" in query=*) query="${2#query=}" ;; esac; shift ;;
    esac
    shift
done
case "$query" in
*"addBlockedBy"* | *"addSubIssue"*)
    echo "$query" >>"$FAKE_GH_LOG"
    if [ -n "${FAKE_GH_ERROR:-}" ]; then echo "$FAKE_GH_ERROR" >&2; exit 1; fi
    echo '{}' ;;
*"{ id } } }"* | *"issue(number: \$number) { id }"*) echo "NODE_$number" ;;
*) cat "$FAKE_GH_JSON" ;;
esac
FAKE
chmod +x "$work/gh"
export PATH="$work:$PATH" FAKE_GH_LOG="$work/log" FAKE_GH_JSON="$work/reply.json"

check() { # name, expected exit status, expected output (substring), command...
    local name="$1" status="$2" expected="$3" output code
    shift 3
    output=$("$@" 2>&1)
    code=$?
    if [ "$code" -ne "$status" ] || ! grep -qF -- "$expected" <<<"$output"; then
        echo "FAIL $name: exit $code (wanted $status), output:"
        sed 's/^/    /' <<<"$output"
        failures=$((failures + 1))
    fi
}

check "help is not an error" 0 "blocked-by <issue> <blocker>" "$script" --help
check "no command is a usage error" 2 "scripts/issue-links.sh blockers" "$script"
check "an unknown command is a usage error" 2 "scripts/issue-links.sh show" "$script" frobnicate
check "blocked-by needs a blocker" 2 "scripts/issue-links.sh sub-issue" "$script" blocked-by 5
check "a number is required" 1 "not an issue number: abc" "$script" blockers abc

echo '{"data":{"repository":{"issue":{"blockedBy":{"nodes":[]}}}}}' >"$work/reply.json"
check "a hash before the number is fine" 0 "" "$script" blockers '#7'

cat >"$work/reply.json" <<'JSON'
{"data":{"repository":{"issue":{"blockedBy":{"nodes":[
 {"number":3,"title":"Open one","state":"OPEN"},
 {"number":4,"title":"Closed one","state":"CLOSED"},
 {"number":5,"title":"Another open","state":"OPEN"}]}}}}}
JSON
check "blockers lists the open ones" 0 "#3 Open one" "$script" blockers 9
output=$("$script" blockers 9)
[ "$output" = $'#3 Open one\n#5 Another open' ] || { echo "FAIL blockers output: $output"; failures=$((failures + 1)); }

echo '{"data":{"repository":{"issue":{"blockedBy":{"nodes":[{"number":4,"title":"Closed","state":"CLOSED"}]}}}}}' >"$work/reply.json"
[ -z "$("$script" blockers 9)" ] || { echo "FAIL blockers prints for closed blockers only"; failures=$((failures + 1)); }

: >"$work/log"
check "blocked-by reports each blocker" 0 "#9 is blocked by #4" "$script" blocked-by 9 3 4
[ "$(grep -c addBlockedBy "$work/log")" -eq 2 ] || { echo "FAIL blocked-by makes one call per blocker"; failures=$((failures + 1)); }
check "sub-issue reports each child" 0 "#8 is a sub-issue of #2" "$script" sub-issue 2 7 8
check "a relation that exists is not an error" 0 "#9 is blocked by #3" env FAKE_GH_ERROR="Issue is already blocked by #3" "$script" blocked-by 9 3
check "another failure is an error" 1 "Resource not accessible" env FAKE_GH_ERROR="Resource not accessible" "$script" blocked-by 9 3

cat >"$work/reply.json" <<'JSON'
{"data":{"repository":{"issue":{"number":2,"title":"Parent","state":"OPEN","parent":null,
 "subIssuesSummary":{"total":2,"completed":1},
 "subIssues":{"nodes":[{"number":7,"title":"Done","state":"CLOSED"},{"number":8,"title":"Todo","state":"OPEN"}]},
 "blockedBy":{"nodes":[{"number":1,"title":"First","state":"OPEN"}]},
 "blocking":{"nodes":[{"number":9,"title":"Later","state":"OPEN"}]}}}}}
JSON
check "show prints the progress" 0 "sub-issues: 1/2 done" "$script" show 2
check "show prints the children" 0 "[x] #7 Done" "$script" show 2
check "show prints the open child" 0 "[ ] #8 Todo" "$script" show 2
check "show prints the blockers" 0 "blocked by: #1 First [OPEN]" "$script" show 2
check "show prints what it blocks" 0 "blocks: #9 Later [OPEN]" "$script" show 2

if [ "$failures" -ne 0 ]; then
    echo "$failures failure(s)"
    exit 1
fi
echo "issue-links.sh: all checks passed"
