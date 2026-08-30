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

