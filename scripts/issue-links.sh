#!/usr/bin/env bash
# Sets and reads the relations between the issues of this repository: GitHub's sub-issues (a parent
# and its children) and dependencies (an issue that is blocked by others). `gh` has no commands for
# them, so this wraps the GraphQL fields and mutations, which take issue numbers here.
#
#   scripts/issue-links.sh blocked-by <issue> <blocker>...   <issue> waits for each blocker to close
#   scripts/issue-links.sh sub-issue <parent> <child>...      each child becomes a sub-issue of <parent>
#   scripts/issue-links.sh blockers <issue>                   the open blockers, one "#N title" per line
#   scripts/issue-links.sh show <issue>                       the parent, the sub-issues and the blockers
#
# Setting a relation that is there is not an error.
# The GraphQL documents hold `$variables` that GraphQL, not the shell, expands.
# shellcheck disable=SC2016
set -euo pipefail

usage() {
    sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit "${1:-2}"
}

die() {
    echo "issue-links: $*" >&2
    exit 1
}

command -v gh >/dev/null || die "gh is not installed"
command -v jq >/dev/null || die "jq is not installed"

owner=""
name=""
load_repository() {
    [ -n "$owner" ] && return
    read -r owner name < <(gh repo view --json owner,name --jq '.owner.login + " " + .name') ||
        die "cannot read the repository; is gh logged in (gh auth status)?"
}

number_of() {
    [[ "${1:-}" =~ ^#?([0-9]+)$ ]] || die "not an issue number: ${1:-}"
    echo "${BASH_REMATCH[1]}"
}

# The node id of an issue, which the mutations take.
node_id() {
    gh api graphql -f owner="$owner" -f name="$name" -F number="$1" \
        -f query='query($owner: String!, $name: String!, $number: Int!) {
            repository(owner: $owner, name: $name) { issue(number: $number) { id } } }' \
        --jq '.data.repository.issue.id' || die "no issue #$1"
}

mutate() {
    local mutation="$1" output
    shift
    if ! output=$(gh api graphql -f query="$mutation" "$@" 2>&1); then
        # A relation that exists is what was asked for.
        if grep -qiE 'already|duplicate' <<<"$output"; then
            return 0
        fi
        die "$output"
    fi
}

query_issue() {
    local fields="$1" number="$2"
    gh api graphql -f owner="$owner" -f name="$name" -F number="$number" \
        -f query="query(\$owner: String!, \$name: String!, \$number: Int!) {
            repository(owner: \$owner, name: \$name) { issue(number: \$number) { $fields } } }"
}

[ $# -ge 1 ] || usage
command_name="$1"
shift
case "$command_name" in
-h | --help | help) usage 0 ;;
blocked-by)
    [ $# -ge 2 ] || usage
    load_repository
    issue=$(number_of "$1")
    shift
    issue_id=$(node_id "$issue")
    for blocker in "$@"; do
        blocker=$(number_of "$blocker")
        mutate 'mutation($issue: ID!, $blocker: ID!) {
                addBlockedBy(input: {issueId: $issue, blockingIssueId: $blocker}) { clientMutationId } }' \
            -f issue="$issue_id" -f blocker="$(node_id "$blocker")"
        echo "#$issue is blocked by #$blocker"
    done
    ;;
sub-issue)
    [ $# -ge 2 ] || usage
    load_repository
    parent=$(number_of "$1")
    shift
    parent_id=$(node_id "$parent")
    for child in "$@"; do
        child=$(number_of "$child")
        mutate 'mutation($parent: ID!, $child: ID!) {
                addSubIssue(input: {issueId: $parent, subIssueId: $child}) { clientMutationId } }' \
            -f parent="$parent_id" -f child="$(node_id "$child")"
        echo "#$child is a sub-issue of #$parent"
    done
    ;;
blockers)
    [ $# -eq 1 ] || usage
    load_repository
    query_issue 'blockedBy(first: 100) { nodes { number title state } }' "$(number_of "$1")" |
        jq -r '.data.repository.issue.blockedBy.nodes[] | select(.state == "OPEN") | "#\(.number) \(.title)"'
    ;;
show)
    [ $# -eq 1 ] || usage
    load_repository
    query_issue 'number title state parent { number title }
            subIssuesSummary { total completed }
            subIssues(first: 100) { nodes { number title state } }
            blockedBy(first: 100) { nodes { number title state } }
            blocking(first: 100) { nodes { number title state } }' "$(number_of "$1")" |
        jq -r '.data.repository.issue as $i
            | "#\($i.number) \($i.title) [\($i.state)]",
              (if $i.parent then "parent: #\($i.parent.number) \($i.parent.title)" else empty end),
              (if $i.subIssuesSummary.total > 0
                  then "sub-issues: \($i.subIssuesSummary.completed)/\($i.subIssuesSummary.total) done" else empty end),
              ($i.subIssues.nodes[] | "  \(if .state == "CLOSED" then "[x]" else "[ ]" end) #\(.number) \(.title)"),
              ($i.blockedBy.nodes[] | "blocked by: #\(.number) \(.title) [\(.state)]"),
              ($i.blocking.nodes[] | "blocks: #\(.number) \(.title) [\(.state)]")'
    ;;
*) usage ;;
esac
