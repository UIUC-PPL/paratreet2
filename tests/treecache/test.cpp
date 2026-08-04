// Standalone concurrency test for the passive TreeCache core
// (design/smp-cache-extraction.md phase 3). Compiled by a PLAIN C++
// compiler with NO Charm headers on the include path and NO Charm
// libraries at link — the gate that the core makes no runtime calls
// (common.h supplies plain-C fallbacks for the assert/abort macros when
// __CHARMC__ is absent, and every serialization method is compiled out
// under the same guard the utility structures already use).
//
// Scenario, repeated over every child slot and for many rounds: worker
// threads race to park opaque values on a placeholder while an installer
// thread concurrently installs a subtree over it. The contract under
// test: every parked opaque is handed back EXACTLY ONCE — either in
// install's drained list or as AlreadyInstalled to its own parker —
// and the tree ends consistent (the installed node is reachable and
// correctly typed).

#include "TreeCache.h"

#include <pthread.h>
#include <algorithm>
#include <cstdio>
#include <vector>

// SFC::makeKey (reached through Particle.C) reads the application-defined
// peanoKey readonly; the test is not an application, so define it here.
int peanoKey = 0;

// Minimal node payload: the Data concept as TreeCache itself needs it
// (default-constructible, accumulable; no bounding box required by the
// cache — that is a framework/application concern).
struct TestData {
  int filler = 0;
  TestData() = default;
  TestData(const Particle*, int, int) {}
  const TestData& operator+=(const TestData&) { return *this; }
};

static const int kLanes = 8;        // parking threads (lanes 1..8; installer uses lane 0)
static const int kRounds = 200;
static const int kParksPerLane = 64;

struct SharedState {
  TreeCache<TestData>* cache;
  Node<TestData>* slot;
  int lane;
  Key slot_key;
  // Outputs: opaques this lane parked, and those returned to it as
  // AlreadyInstalled (self-handled).
  std::vector<uint64_t> parked_attempts;
  std::vector<uint64_t> self_handled;
};

static void* parkerThread(void* arg) {
  auto* st = (SharedState*)arg;
  for (int i = 0; i < kParksPerLane; i++) {
    // Unique opaque per (lane, attempt): mirrors the caller-side encoding
    // (lane in high bits), distinct low bits so the exactly-once check
    // can account for every value individually.
    uint64_t opaque = ((uint64_t)st->lane << 56) | ((uint64_t)i + 1);
    st->parked_attempts.push_back(opaque);
    if (st->cache->park(st->slot, opaque) ==
        TreeCache<TestData>::ParkResult::AlreadyInstalled) {
      st->self_handled.push_back(opaque);
      // Once installed, every later park must also say AlreadyInstalled.
      for (int j = i + 1; j < kParksPerLane; j++) {
        uint64_t o2 = ((uint64_t)st->lane << 56) | ((uint64_t)j + 1);
        st->parked_attempts.push_back(o2);
        if (st->cache->park(st->slot, o2) !=
            TreeCache<TestData>::ParkResult::AlreadyInstalled) {
          fprintf(stderr, "FAIL: park succeeded after AlreadyInstalled\n");
          exit(1);
        }
        st->self_handled.push_back(o2);
      }
      break;
    }
  }
  return nullptr;
}

int main() {
  int total_rounds = 0;
  for (int round = 0; round < kRounds; round++) {
    TreeCache<TestData> cache;
    cache.init(kLanes + 1, /*shared=*/true, /*branch_factor=*/8,
               /*pool_elem_size=*/128);

    // Install the root as a boundary node; insertNode wires 8
    // RemoteAboveTPKey placeholder children under it.
    std::vector<uint64_t> none;
    SpatialNode<TestData> root_sn(/*depth=*/0, /*n_particles=*/-1);
    std::pair<Key, SpatialNode<TestData>> root_pair(Key(1), root_sn);
    cache.installBoundary(/*lane=*/0, root_pair, none);
    if (!none.empty()) {
      fprintf(stderr, "FAIL: root install drained %zu waiters\n", none.size());
      return 1;
    }

    // Pick a different child slot each round.
    Key slot_key = Key(8) + (round % 8);
    Node<TestData>* slot = cache.root->getDescendant(slot_key);
    if (!slot) { fprintf(stderr, "FAIL: no placeholder\n"); return 1; }

    // Launch parkers, then install over the placeholder mid-race.
    pthread_t threads[kLanes];
    SharedState states[kLanes];
    for (int t = 0; t < kLanes; t++) {
      states[t].cache = &cache;
      states[t].slot = slot;
      states[t].lane = t + 1;
      states[t].slot_key = slot_key;
      pthread_create(&threads[t], nullptr, parkerThread, &states[t]);
    }

    // One-node partial subtree (an internal node: n_particles = -1, so no
    // Particle construction happens anywhere in this test).
    SpatialNode<TestData> sn(/*depth=*/1, /*n_particles=*/-1);
    std::pair<Key, SpatialNode<TestData>> nodes[1] = {{slot_key, sn}};
    std::vector<uint64_t> drained;
    cache.installSubtree(/*lane=*/0, nullptr, 0, nodes, 1,
                         /*cm_index=*/0, /*tp_index=*/0,
                         /*add_to_tps=*/false, drained);

    for (int t = 0; t < kLanes; t++) pthread_join(threads[t], nullptr);

    // Late parks after install must all self-handle.
    uint64_t late = (uint64_t)0xee << 56;
    if (cache.park(cache.root->getDescendant(slot_key), late) !=
        TreeCache<TestData>::ParkResult::AlreadyInstalled) {
      fprintf(stderr, "FAIL: post-install park was accepted\n");
      return 1;
    }

    // Exactly-once accounting: attempts == drained + self_handled, as
    // SETS (the park-side consecutive-duplicate suppression only fires
    // for equal opaques, and every attempt here is unique).
    std::vector<uint64_t> attempts, handled(drained);
    for (auto& st : states) {
      attempts.insert(attempts.end(), st.parked_attempts.begin(),
                      st.parked_attempts.end());
      handled.insert(handled.end(), st.self_handled.begin(),
                     st.self_handled.end());
    }
    std::sort(attempts.begin(), attempts.end());
    std::sort(handled.begin(), handled.end());
    if (attempts != handled) {
      fprintf(stderr,
              "FAIL round %d: %zu attempts vs %zu handled (drained %zu)\n",
              round, attempts.size(), handled.size(), drained.size());
      return 1;
    }

    // Tree consistency: the installed node replaced the placeholder.
    Node<TestData>* installed = cache.root->getDescendant(slot_key);
    if (installed == slot ||
        installed->type != Node<TestData>::Type::CachedRemote) {
      fprintf(stderr, "FAIL round %d: install not published\n", round);
      return 1;
    }
    total_rounds++;
  }
  printf("TREECACHE TEST PASSED: %d rounds, %d lanes x %d parks, "
         "exactly-once park/install accounting\n",
         total_rounds, kLanes, kParksPerLane);
  return 0;
}
