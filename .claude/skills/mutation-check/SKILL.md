---
name: mutation-check
description: Prove that a new test can fail by breaking the behavior on purpose and watching the test catch it. Use after writing tests for new behavior and before opening a pull request.
---

# Prove a test can fail

A test that has never failed may test nothing. For each new behavior, break the code in the way a real bug would and confirm a test fails for the right reason.

## Procedure

1. Commit or stash the finished work so that the tree can be restored with `git checkout -- <file>`. Never mutate a file that holds uncommitted work without a copy.
2. Pick a mutation that removes or inverts the behavior the test claims to cover: delete the guard, flip the comparison, return early, skip the notification, change an off-by-one.
3. Build. **The build must succeed.** A mutation that does not compile (commonly an unused variable under `-Werror`) proves nothing; adjust it until it compiles.
4. Run the specific test binary directly and confirm the new test fails, with a message about the behavior in question, not an unrelated crash.
5. Restore the file (`git checkout -- <file>`), rebuild, and confirm the whole suite passes again. A file restored by moving or copying a saved copy back keeps the copy's older modification time, so the build tool can keep the object compiled from the mutated source; `touch` the file after restoring it.
6. Repeat for each distinct behavior: one mutation per claim, not one for the whole change.

## When the test does not fail

The test is too weak. Common causes: it asserts on a value the mutation did not touch; it uses an input where both versions agree (an ASCII-only string, one cursor, an empty document); a `FakeHost` test never reaches the code because the real host would have sent something else; a real-host test skipped. Strengthen the test, then mutate again.

## Report it

In the pull request, list each mutation and the test that caught it, e.g. "removed the dirty-marker reset in `mark_saved`; `DocumentTest.SaveClearsDirty` fails". Reviewers repeat a few of them.
