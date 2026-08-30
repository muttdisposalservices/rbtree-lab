8/29/2026:
    Asked for a walkthrough of how the tree struct should be made and the minimum number of parameters necessary for it.
    Questioned reply about tree-node key ownerships and how the tree owns the keys without direct data structure storage.
    Clarified the process to free a node and its values in memory.
    Drafted a skeleton rbtree.c with function stubs following include.h.
    Rejected in favor of a NIL sentinel instead of NULL handling to simplify logic.
    
    Added dummy test_rbtree.c and fuzz.c main functions to allow for make to reflect actual compilation instead of lack of logic for undefined functions.
    
    Compacted for future reference.
    
    Ran code review to set a baseline.
    
    vv Claude insert.
        Reviewed findings: confirmed a Makefile bug where make/asan/memcheck can
        silently reuse a stale ASan-instrumented binary (deferred, TODO added);
        left TODO notes for the strdup/rb_malloc routing conflict and the
        missing rb_destroy needed for memcheck. Pausing here as a clean
        stopping point before implementing real M1 logic.
        `make asan` currently fails on this machine with "cannot find
        /usr/lib64/libasan.so.6.0.0" -- traced to libasan/libubsan not
        being installed on this shared server (packages exist in the
        AppStream repo, just not installed; no sudo access to fix).
        Environment gap, not a code regression -- flagged to course staff.


    ADDITION:
    Tried to resolve issues with 'make asan' via static libraries, but Claude's search returned no available ASan or UBSan libraries on the machine.

    Asked for good and bad integrations of rb_create listed in include/rbtree.h and src/rbtree.c with walkthroughs and a focus on memory checks/allocations.
    Clarified what value_free is with respect to the necessary functions. Also asked if assignment operations have risk of failure, as well as if there was any extra need for pre-definition checks on nil.
    
    Rejected recommendation to implement 'memset(nil, 0, sizeof *nil)' after malloc checks in rb_create because I prefer to see explicit parameter/variable definitions, not clever one-line workarounds that have odd convention later on.
    
    Checked case handling for value_free outside of rb_create.

    Implemented minimal-diff rb_create and removed '[[maybe unused]]' from 'rb_malloc' and 'rb_free' wrappers (previously used to help gcc compile without issues). 'make test' and 'make memcheck' pass.

    Requested table-driven test cases for rb_create. Pushed for rb_malloc and rb_free to be used, and Claude recommended weak linkage instead of static for non-test builds, so for future-proofing this was approved. Added tests/fault_malloc.c to test faulty malloc calls. Added a minimal rb_destroy stub to allow tests to pass without compromising scope. Also requested these rb_create tests not overwrite main() in tests/test_rbtree.c and just add a helper test function. Also modified Makefile to include tests/fault_malloc.c.
    Tests pass, including:
        create_null_value_free
        create_nonnull_value_free
        create_fail_first_alloc
        create_fail_second_alloc
        create_independent_instances
        create_destroy_null_safe

8/30/26:
    Asked to be quizzed from beginner-medium-hard on memory allocation and ownership multiple times. Pushed into implementing rb_insert tests. Ran adversarial review in a new context against for the following results:
        Added a count-free function instead of noop_free to count the number of improperly-invoked frees.
        Add rb_validate assertions. This causes make test to fail until implementation, but this is deliberate.
        Add comments clarifying stale case-numbering and fail-at magic-number coupling.
    