# rbtree-lab: project rules
## Commands
- Build & unit tests: ‘make test‘
- Sanitizers: ‘make asan‘ Valgrind: ‘make memcheck‘
- A change is DONE only when all three pass. Always run them; show output.
- Route all allocations in src/rbtree.c through two rb_malloc and rb_free found in src/rb_alloc.c.
## Hard constraints
- NEVER modify include/rbtree.h. It is the graded contract.
- Use include/rbtree.h as a strict reference for allocations and ownership such; every function must follow the rules outlined there.
- Check every allocation. malloc can return NULL; a NULL return must
leave the tree unchanged and return the documented error code.
- NEVER weaken, skip, or delete a test to make the suite pass. If a test
looks wrong, stop and explain why instead.
- 'rb_insert' must 'rb_malloc + memcpy' its own copy 'k'; it must never store 'key' itself or free it
## Style
- C23. -Wall -Wextra -Werror must stay clean. No VLAs.
- Error handling: goto-cleanup pattern for multi-allocation functions.
- Prefer the smallest diff that passes. Do not refactor unrelated code.
- Every non-obvious loop gets a one-line invariant comment.
## Workflow
- For any multi-file or algorithmic change: propose a plan and wait for
approval before editing.
- Never commit to the git repository.