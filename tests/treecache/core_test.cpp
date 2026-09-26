// Standalone test of TreeCacheCore over a node type that is NOT
// paratreet2's Node<Data> — the gate that the Traits contract in
// TreeCacheCore.h is the WHOLE dependency (nothing from Node.h, Particle.h
// or common.h is included). This is the shape a ChaNGa binding takes:
// a foreign node with its own child array, key, parent and a parked-list
// field, plus a traits struct of static inline forwarders.
//
// Scenario (same as test.cpp's): parker threads race to park on a
// placeholder while an installer publishes a node over it with swapIn;
// every parked opaque must come back exactly once, and a park after the
// publication must report AlreadyInstalled.

#include "TreeCacheCore.h"

#include <pthread.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

static const int kBranch = 8;

struct MiniNode {
  uint64_t key;
  MiniNode* parent;
  bool placeholder;
  std::array<std::atomic<MiniNode*>, kBranch> kids;
  std::atomic<void*> parked{nullptr};
  MiniNode(uint64_t k, MiniNode* p, bool ph) : key(k), parent(p), placeholder(ph) {
    for (auto& c : kids) c.store(nullptr);
  }
};

struct MiniTraits {
  using Node = MiniNode;
  using Key = uint64_t;
  static Key key(const Node* n) { return n->key; }
  static Node* parent(const Node* n) { return n->parent; }
  static Node* exchangeChild(Node* p, int which, Node* c) {
    return p->kids[which].exchange(c, std::memory_order_relaxed);
  }
  static std::atomic<void*>& parkedHead(Node* n) { return n->parked; }
};

using Core = TreeCacheCore<MiniTraits>;

static const int kLanes = 8;
static const int kRounds = 200;
static const int kParksPerLane = 64;

struct SharedState {
  Core* cache;
  MiniNode* slot;
  int lane;
  std::vector<uint64_t> parked_attempts;
  std::vector<uint64_t> self_handled;
};

static void* parkerThread(void* arg) {
  auto* st = (SharedState*)arg;
  for (int i = 0; i < kParksPerLane; i++) {
    uint64_t opaque = ((uint64_t)st->lane << 56) | ((uint64_t)i + 1);
    st->parked_attempts.push_back(opaque);
    if (st->cache->park(st->slot, opaque) == Core::ParkResult::AlreadyInstalled) {
      st->self_handled.push_back(opaque);
      for (int j = i + 1; j < kParksPerLane; j++) {
        uint64_t o2 = ((uint64_t)st->lane << 56) | ((uint64_t)j + 1);
        st->parked_attempts.push_back(o2);
        if (st->cache->park(st->slot, o2) != Core::ParkResult::AlreadyInstalled) {
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
    Core cache;
    cache.branch_factor = kBranch;

    // Root (key 1) installed by the client: a non-placeholder, so its
    // parked list is born closed; its children are placeholders.
    std::vector<uint64_t> none;
    auto* root = new MiniNode(1, nullptr, false);
    Core::closeParkedList(root);
    std::vector<MiniNode*> owned{root};
    for (int i = 0; i < kBranch; i++) {
      auto* ph = new MiniNode(uint64_t(kBranch) + i, root, true);
      owned.push_back(ph);
      root->kids[i].store(ph);
    }
    cache.swapIn(root, none); // first publication: displaces a null root
    if (!none.empty() || cache.root != root) {
      fprintf(stderr, "FAIL: root publication\n");
      return 1;
    }

    int which = round % kBranch;
    MiniNode* slot = root->kids[which].load();
    if (!slot || !slot->placeholder) { fprintf(stderr, "FAIL: no placeholder\n"); return 1; }

    pthread_t threads[kLanes];
    SharedState states[kLanes];
    for (int t = 0; t < kLanes; t++) {
      states[t].cache = &cache;
      states[t].slot = slot;
      states[t].lane = t + 1;
      pthread_create(&threads[t], nullptr, parkerThread, &states[t]);
    }

    // The installer builds the replacement fully (children wired to fresh
    // placeholders), THEN publishes with one swapIn.
    auto* installed = new MiniNode(slot->key, root, false);
    Core::closeParkedList(installed);
    owned.push_back(installed);
    for (int i = 0; i < kBranch; i++) {
      auto* ph = new MiniNode(installed->key * kBranch + i, installed, true);
      owned.push_back(ph);
      installed->kids[i].store(ph);
    }
    std::vector<uint64_t> drained;
    cache.swapIn(installed, drained);

    for (int t = 0; t < kLanes; t++) pthread_join(threads[t], nullptr);

    uint64_t late = (uint64_t)0xee << 56;
    if (cache.park(root->kids[which].load(), late) != Core::ParkResult::AlreadyInstalled) {
      fprintf(stderr, "FAIL: post-install park was accepted\n");
      return 1;
    }
    // A park on the displaced placeholder itself must also self-handle.
    if (cache.park(slot, late) != Core::ParkResult::AlreadyInstalled) {
      fprintf(stderr, "FAIL: park on the displaced placeholder was accepted\n");
      return 1;
    }

    std::vector<uint64_t> attempts, handled(drained);
    for (auto& st : states) {
      attempts.insert(attempts.end(), st.parked_attempts.begin(), st.parked_attempts.end());
      handled.insert(handled.end(), st.self_handled.begin(), st.self_handled.end());
    }
    std::sort(attempts.begin(), attempts.end());
    std::sort(handled.begin(), handled.end());
    if (attempts != handled) {
      fprintf(stderr, "FAIL round %d: %zu attempts vs %zu handled (drained %zu)\n",
              round, attempts.size(), handled.size(), drained.size());
      return 1;
    }
    if (root->kids[which].load() != installed) {
      fprintf(stderr, "FAIL round %d: install not published\n", round);
      return 1;
    }
    for (auto* n : owned) delete n;
    total_rounds++;
  }
  printf("TREECACHE CORE TEST PASSED: %d rounds, %d lanes x %d parks over a "
         "non-paratreet node type, exactly-once park/install accounting\n",
         total_rounds, kLanes, kParksPerLane);
  return 0;
}
