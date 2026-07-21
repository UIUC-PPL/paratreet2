# CacheManager concurrency: lock-free by design — do not add locks

Status: architectural invariant (recorded 2026-07-21). The `CacheManager`
(`src/CacheManager.h`) is a **nodegroup**: every worker PE of a process shares
one branch and runs its entry methods concurrently on separate pthreads. Its
throughput depends on being **lock-free on the hot path**. This is a
deliberate design choice to preserve, not an accident.

## The invariant

1. **Concurrent cache fills are lock-free**, via atomic child-pointer exchange
   (`Node::children` are `std::atomic<Node*>`; `Node::exchangeChild` uses
   `std::atomic::exchange`) plus an atomic per-PE `requested` bitmask
   (`Node::requested`, `std::atomic<unsigned long long>`). Many PEs grow the
   cached tree simultaneously with no mutex.
2. **Reads are lockless.** Traversals (`Traverser.h`, on Partition chares)
   read `node->data` and particles with no lock, relying on cached `Data`
   being immutable-after-fill.
3. **The only mutex is `maps_lock`, and it is narrow.** It guards solely the
   top-level registry inserts (`local_tps`, `leaf_lookup`) — the
   std::map/unordered_map structure is not self-thread-safe. It does not, and
   must not, cover the data path.

## Why you must not add locks

Reaching for a lock to "make the cache thread-safe" is self-defeating on two
counts:

- A broad/data-path lock **serializes the very concurrency that is the point**
  — the parallel fills and lockless reads across a node's PEs.
- Because readers are lockless, a mutex on a **writer cannot serialize against
  them** anyway. It buys nothing and costs throughput. It is false comfort.

## The correct tools instead

- **Narrow atomics** for concurrent structural updates (child exchange,
  request bitmask) — as the fill path already does.
- **Phase separation** for any post-build *mutation* of cached `Data`. Stock
  ParaTreeT never mutates cached Data; the FoF/annotate work does
  (`Subtree::upwardPass`, `callPerLeafFn` → `CacheManager::refreshSubtreeCopy`).
  Such a mutation is safe only when it runs in a phase that is
  **quiescence-separated** from any traversal reading it: do the mutation,
  `CkWaitQD()` (also the cross-PE memory barrier), *then* load the cache and
  traverse. `examples/annotate`'s `preTraversalFn` is the reference pattern;
  `refreshSubtreeCopy` and `Subtree::refreshCopies` carry the contract in
  their comments.

## If you think you need a lock

You probably need phase separation instead. If a genuinely new concurrent
*writer* is required on the hot path, extend the atomic scheme (narrow,
lock-free) — do not introduce a mutex on the data path. Revisit this note and
the `CONCURRENCY DESIGN` block atop `CacheManager` before changing any of it.
