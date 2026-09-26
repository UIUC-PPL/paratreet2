# treecache — the process-shared tree cache core

`TreeCacheCore.h` is the node-type-independent part of paratreet2's
lock-free SMP tree cache (design: `../design/smp-cache-extraction.md`,
section 2). It is header-only plain C++ over `<atomic>`, with no Charm++
dependency and no messaging. One instance is shared by all worker
threads of a process. It owns three things:

- the root of the process-shared tree,
- the placeholder **park / install** contract (`park`,
  `closeParkedList`; a placeholder's parked-waiter list is closed with a
  sentinel exactly once at install, so a late `park` gets
  `AlreadyInstalled` and no wakeup is lost),
- atomic **publication** of an installed node in place of its placeholder
  (`swapIn`, which drains and returns the parked waiters exactly once).

Everything about what a node *is* (its type, allocation, payload, how a
partial subtree is built from a reply) belongs to the client and reaches
the core only through the `Traits` parameter, four static inline
forwarders: `key`, `parent`, `exchangeChild`, `parkedHead`. The contract
is documented at the top of the header. Requirements on the client's
node: the root has key 1 and child `i` of key `k` has key
`k * branch_factor + i`; the parked-list head is a field on the node.

Clients:

- paratreet2: `../src/TreeCache.h` (`TreeCache<Data>` derives from
  `TreeCacheCore<NodeTraits<Data>>`), driven by the `CacheManager`
  nodegroup.
- ChaNGa: binding over `Tree::GenericTreeNode` (in progress, 2026-09).

Test: `make test` here builds `core_test.cpp` with only this directory on
the include path and runs the park/install race harness over a foreign
node type. paratreet2's own binding is tested in `../tests/treecache`.
