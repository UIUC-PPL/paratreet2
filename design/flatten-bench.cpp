// Microbench: cost of flattenStealNode-style preorder subtree flatten when
// piece nodes are (a) lane-interleaved individual allocations vs
// (b) per-piece contiguous preorder arenas, vs (c) the contiguous
// end-state (pure range sweep, no pointer chase).
// Mimics FullNode<FragData,8> size/shape: vptr + 8 atomic children +
// bookkeeping + ~112B spatial payload.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

struct Vec3 { double x = 0, y = 0, z = 0; };
struct Payload {                    // ~ SpatialNode<FragData>
  Vec3 lo, hi;                      // OrientedBox corners
  long min_frag = 0, max_frag = 0, n_below = 0;
  int n_particles = -1, depth = 0;
  uint64_t pmin = ~0ull, pmax = 0;
};
struct Node {
  virtual ~Node() {}                // vptr, like Node<Data>
  uint64_t key = 1;
  int n_children = 0;
  Node* parent = nullptr;
  int type = 0, wait_count = -1, tp = -1, cm = -1;
  std::atomic<uint64_t> requested{0};
  std::atomic<Node*> children[8];
  Payload sp;
  Node() { for (auto& c : children) c.store(nullptr, std::memory_order_relaxed); }
};

struct Wire {                       // WireNode-equivalent
  uint64_t key; int32_t parent; int8_t slot; Payload sp;
};

// Build one piece's subtree in preorder into `slots` (pre-allocated node
// storage handed out sequentially), returning number consumed.
static size_t buildPre(Node** slots, size_t n_budget, std::mt19937& rng) {
  if (n_budget == 0) return 0;
  struct Frame { Node* n; };
  size_t used = 0;
  Node* root = slots[used++];
  root->n_children = 8;
  std::vector<Node*> stack{root};
  std::uniform_int_distribution<int> kids(2, 5);
  while (!stack.empty() && used < n_budget) {
    Node* p = stack.back(); stack.pop_back();
    int k = kids(rng);
    for (int i = 0; i < 8 && used < n_budget; i++) {
      if (i >= k) continue;
      Node* c = slots[used++];
      c->parent = p;
      bool leaf = (used + stack.size() * 3 >= n_budget) || (rng() % 3 == 0);
      c->n_children = leaf ? 0 : 8;
      c->sp.n_particles = leaf ? 8 : -1;
      p->children[i].store(c, std::memory_order_relaxed);
      if (!leaf) stack.push_back(c);
    }
  }
  return used;
}

static void flattenRec(Node* n, std::vector<Wire>& out, int32_t parent, int8_t slot) {
  if (!n) return;
  int32_t idx = (int32_t)out.size();
  out.push_back({n->key, parent, slot, n->sp});
  if (n->n_children == 0) return;
  for (int i = 0; i < 8; i++)
    flattenRec(n->children[i].load(std::memory_order_relaxed), out, idx, (int8_t)i);
}

int main() {
  const size_t PIECES = 128, NODES = 4096;   // ~134 MB of Node storage
  std::mt19937 rng(42);
  printf("sizeof(Node)=%zu sizeof(Wire)=%zu total=%.0f MB\n",
         sizeof(Node), sizeof(Wire), PIECES * NODES * sizeof(Node) / 1e6);

  // (a) interleaved: allocate all slots round-robin across pieces, in the
  // order a shared lane pool would hand them out while pieces build
  // concurrently.
  std::vector<std::vector<Node*>> slots_a(PIECES, std::vector<Node*>(NODES));
  for (size_t i = 0; i < NODES; i++)
    for (size_t p = 0; p < PIECES; p++) slots_a[p][i] = new Node();
  // (b) contiguous: one arena per piece, preorder order == address order.
  std::vector<Node*> arenas(PIECES);
  std::vector<std::vector<Node*>> slots_b(PIECES, std::vector<Node*>(NODES));
  for (size_t p = 0; p < PIECES; p++) {
    arenas[p] = new Node[NODES];
    for (size_t i = 0; i < NODES; i++) slots_b[p][i] = &arenas[p][i];
  }
  std::vector<size_t> used_a(PIECES), used_b(PIECES);
  for (size_t p = 0; p < PIECES; p++) {
    std::mt19937 r1(p), r2(p);
    used_a[p] = buildPre(slots_a[p].data(), NODES, r1);
    used_b[p] = buildPre(slots_b[p].data(), NODES, r2);
  }

  auto bench = [&](const char* name, auto&& fn) {
    // warm + measure over all pieces, several rounds
    std::vector<Wire> out;
    out.reserve(NODES);
    size_t total = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int round = 0; round < 5; round++)
      for (size_t p = 0; p < PIECES; p++) { out.clear(); total += fn(p, out); }
    auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("%-28s %8.1f ns/node  (%zu nodes)\n", name, dt / total * 1e9, total);
    return dt;
  };

  bench("interleaved pointer-chase", [&](size_t p, std::vector<Wire>& out) {
    flattenRec(slots_a[p][0], out, -1, -1); return out.size(); });
  bench("contiguous pointer-chase", [&](size_t p, std::vector<Wire>& out) {
    flattenRec(slots_b[p][0], out, -1, -1); return out.size(); });
  bench("contiguous range sweep", [&](size_t p, std::vector<Wire>& out) {
    // end-state: subtree == contiguous range; parent/slot precomputed
    // (here: emitted as stored order without chasing children)
    Node* base = arenas[p];
    size_t n = used_b[p];
    for (size_t i = 0; i < n; i++)
      out.push_back({base[i].key, 0, 0, base[i].sp});
    return n; });
  return 0;
}
