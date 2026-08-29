# rbtree-lab: project rules
## Commands
- Build & unit tests: ‘make test‘
- Sanitizers: ‘make asan‘ Valgrind: ‘make memcheck‘
- A change is DONE only when all three pass. Always run them; show output.
- Route all allocations in src/rbtree.c through two four-line wrappers for rb_malloc and rb_free, then forward to malloc and free.
## Hard constraints
- NEVER modify include/rbtree.h. It is the graded contract.
- Use include/rbtree.h as a strict reference for allocations and ownership such; every function must follow the rules outlined there.
- Check every allocation. malloc can return NULL; a NULL return must
leave the tree unchanged and return the documented error code.
- NEVER weaken, skip, or delete a test to make the suite pass. If a test
looks wrong, stop and explain why instead.
## Style
- C23. -Wall -Wextra -Werror must stay clean. No VLAs.
- Error handling: goto-cleanup pattern for multi-allocation functions.
- Prefer the smallest diff that passes. Do not refactor unrelated code.
- Every non-obvious loop gets a one-line invariant comment.
## Workflow
- For any multi-file or algorithmic change: propose a plan and wait for
approval before editing.
- Commit only from a green state; message format "M<n>: <what>".
- After commit, offer to run adversarial review with /code-review against the previous commits. Wait for approval.
## Assignment Details
- This is Milestone 1, so <n> is 1 for all commits here.
- Focus on rb_create, rb_insert(with rebalancing), rb_find, rb_foreach (in-order traversal), rb_validate.