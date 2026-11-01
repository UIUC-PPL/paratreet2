// Stand-in host for the FoF3 plug-in (design/plugin-interface.md, stage B).
// It owns its particles the way a simulation would: its own chare array,
// each element holding a contiguous slice of the input file (file order,
// not space), placed on a scrambled PE. It includes only the plug-in API,
// no paratreet headers, stages its particles, runs FoF through the plug-in
// and prints a label digest that must equal FoF3's FOF3STAT label_digest
// for the same file and linking length. -r repeats the whole cycle to
// check that consecutive calls start clean.
//
// -c <maxlevel> is stage C: the host first decomposes its particles the
// way a simulation would, into an adaptive octree of its own universe cube
// (a cell splits while it holds more than N / (8 PEs) particles, down to
// maxlevel), deals the leaves to pieces in contiguous count-balanced runs
// (-s: scrambled instead), and stages each leaf with stageCell, so FoF
// keeps them in place.
//
// -m <n> drops groups smaller than n particles (their label is -1); -K
// asks for the halo catalog and prints its summary (FOFHOST catalog),
// comparable with FoF3's FOF3STAT catalog for the same -m.
//
//   fofhost -f <tipsy> [-b factor | -L length] [-P period] [-l leaf]
//           [-k pieces per PE] [-r repeats] [-c maxlevel [-s]] [-m n] [-K]

#include "fofhost.decl.h"
#include "FoFPluginAPI.h"
#include "TipsyFile.h"

#include <unistd.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <vector>

/* readonly */ int peanoKey;
/* readonly */ CProxy_HostPiece hostPieces;

class HostMain : public CBase_HostMain {
  fofplugin::Params params_;
  int repeats_ = 1;
  int cell_level_ = -1;
  bool scramble_ = false;
  int n_pieces_ = 0;
  long n_total_ = 0;

 public:
  HostMain(CkArgMsg* m) {
    std::string file;
    int pieces_per_pe = 2;
    int c;
    while ((c = getopt(m->argc, m->argv, "f:b:L:P:l:k:r:c:sm:K")) != -1) {
      switch (c) {
        case 'f': file = optarg; break;
        case 'b': params_.b_factor = atof(optarg); break;
        case 'L': params_.linking_length = atof(optarg); break;
        case 'P': params_.period = atof(optarg); break;
        case 'l': params_.leaf_size = atoi(optarg); break;
        case 'k': pieces_per_pe = atoi(optarg); break;
        case 'r': repeats_ = atoi(optarg); break;
        case 'c': cell_level_ = atoi(optarg); break;
        case 's': scramble_ = true; break;
        case 'm': params_.min_group_size = atol(optarg); break;
        case 'K': params_.catalog = true; break;
        default: CkAbort("fofhost: unknown option\n");
      }
    }
    if (file.empty()) CkAbort("fofhost: -f <tipsy file> is required\n");
    delete m;
    peanoKey = 3;

    Tipsy::TipsyReader r(file);
    if (!r.status()) CkAbort("fofhost: cannot open %s\n", file.c_str());
    const long n_total = r.getHeader().nbodies;
    const int n_pieces = pieces_per_pe * CkNumPes();
    n_pieces_ = n_pieces;
    n_total_ = n_total;
    CkPrintf("FOFHOST file %s n %ld pieces %d leaf %d b_factor %g L %g "
             "period %g repeats %d cell_level %d\n", file.c_str(), n_total,
             n_pieces, params_.leaf_size, params_.b_factor,
             params_.linking_length, params_.period, repeats_, cell_level_);

    // Scrambled placement: piece i (file slice i) on PE (i * 7919 + 13) % P,
    // unrelated to where FoF3's own Readers would put that slice.
    hostPieces = CProxy_HostPiece::ckNew();
    for (int i = 0; i < n_pieces; i++)
      hostPieces[i].insert(file, n_total, n_pieces,
                           (int)(((long)i * 7919 + 13) % CkNumPes()));
    hostPieces.doneInserting();
    fofplugin::whenReady(CkCallback(CkIndex_HostMain::go(), thisProxy));
  }

  void go() {
    if (cell_level_ >= 0) {
      // The host's own decomposition: a cube around its particles, then an
      // adaptive octree of it, each leaf owned by one piece.
      CkReductionMsg* msg = nullptr;
      hostPieces.bbox(CkCallbackResumeThread((void*&)msg));
      const double* b = (const double*)msg->getData();
      double lo[3] = {b[0], b[1], b[2]}, hi[3] = {-b[3], -b[4], -b[5]};
      delete msg;
      double side = 0;
      for (int d = 0; d < 3; d++) side = std::max(side, hi[d] - lo[d]);
      side *= 1.0 + 1e-5;  // strictly inside, also after rounding to float
      for (int d = 0; d < 3; d++) lo[d] = 0.5 * (lo[d] + hi[d]) - 0.5 * side;
      double t0 = CkWallTimer();
      // Adaptive octree from a histogram at the finest level.
      const int L = cell_level_;
      const long m = 1L << L;
      hostPieces.histogram(lo[0], lo[1], lo[2], side, L,
                           CkCallbackResumeThread((void*&)msg));
      std::vector<std::vector<long>> cnt(L + 1);
      cnt[L].assign((const long*)msg->getData(),
                    (const long*)msg->getData() + m * m * m);
      delete msg;
      for (int l = L - 1; l >= 0; l--) {
        const long n = 1L << l;
        cnt[l].assign(n * n * n, 0);
        for (long x = 0; x < 2 * n; x++)
          for (long y = 0; y < 2 * n; y++)
            for (long z = 0; z < 2 * n; z++)
              cnt[l][((x / 2) * n + y / 2) * n + z / 2] +=
                  cnt[l + 1][(x * 2 * n + y) * 2 * n + z];
      }
      const long thresh = std::max(1L, n_total_ / (8L * CkNumPes()));
      std::vector<int> leaf_desc;      // level, ix, iy, iz per leaf
      std::vector<long> leaf_n;
      std::vector<int> fine_to_leaf(m * m * m, -1);
      std::vector<std::array<long, 4>> todo = {{0, 0, 0, 0}};
      while (!todo.empty()) {
        const auto c = todo.back();
        todo.pop_back();
        const long l = c[0], n = 1L << l;
        const long k = cnt[l][(c[1] * n + c[2]) * n + c[3]];
        if (k == 0) continue;
        if (k > thresh && l < L) {
          for (int j = 7; j >= 0; j--)  // pop order 0..7
            todo.push_back({l + 1, 2 * c[1] + (j >> 2), 2 * c[2] + ((j >> 1) & 1),
                            2 * c[3] + (j & 1)});
          continue;
        }
        const int id = (int)leaf_n.size();
        leaf_desc.insert(leaf_desc.end(), {(int)l, (int)c[1], (int)c[2], (int)c[3]});
        leaf_n.push_back(k);
        const long s = 1L << (L - l);
        for (long x = c[1] * s; x < (c[1] + 1) * s; x++)
          for (long y = c[2] * s; y < (c[2] + 1) * s; y++)
            for (long z = c[3] * s; z < (c[3] + 1) * s; z++)
              fine_to_leaf[(x * m + y) * m + z] = id;
      }
      // Leaves to pieces: contiguous runs of ~N/pieces particles in the
      // octree order above, or scrambled.
      std::vector<int> leaf_owner(leaf_n.size());
      long before = 0;
      for (size_t i = 0; i < leaf_n.size(); i++) {
        leaf_owner[i] = scramble_
            ? (int)(((uint64_t)i * 7919 + 13) % n_pieces_)
            : (int)std::min<long>(n_pieces_ - 1, before * n_pieces_ / n_total_);
        before += leaf_n[i];
      }
      hostPieces.makeCells(lo[0], lo[1], lo[2], side, L, fine_to_leaf, leaf_desc,
                           leaf_owner, CkCallbackResumeThread());
      CkWaitQD();
      CkPrintf("FOFHOST cells: maxlevel %d leaves %zu threshold %ld %s universe "
               "lo (%g %g %g) side %.9g host decomposition %.3f s\n", L,
               leaf_n.size(), thresh, scramble_ ? "scrambled" : "balanced",
               lo[0], lo[1], lo[2], side, CkWallTimer() - t0);
      params_.host_cells = true;
      for (int d = 0; d < 3; d++) params_.universe_lo[d] = lo[d];
      params_.universe_side = side;
    }
    for (int rep = 0; rep < repeats_; rep++) {
      double t0 = CkWallTimer();
      hostPieces.stage(CkCallbackResumeThread());
      double t1 = CkWallTimer();
      fofplugin::run(params_, CkCallbackResumeThread());
      double t2 = CkWallTimer();
      CkReductionMsg* msg = nullptr;
      hostPieces.digest(rep, CkCallbackResumeThread((void*&)msg));
      const auto* d = (const unsigned long long*)msg->getData();
      CkPrintf("FOFHOST label_digest: n %llu digest %016llx unlabeled %llu "
               "(rep %d)\n", d[0], d[1], d[2], rep);
      if (params_.catalog)
        CkPrintf("FOFHOST catalog: min_size %ld groups %llu particles %llu "
                 "digest %016llx (rep %d)\n", std::max(1L, params_.min_group_size),
                 d[3], d[4], d[5], rep);
      CkPrintf("FOFHOST time_s: stage %.3f fof %.3f (rep %d)\n", t1 - t0,
               t2 - t1, rep);
      delete msg;
      hostPieces.clearStaged(CkCallbackResumeThread());
    }
    CkExit();
  }
};

class HostPiece : public CBase_HostPiece {
  std::vector<fofplugin::Particle> parts_;
  size_t first_ = 0;
  // Stage C: the particles this piece owns, grouped by leaf cell, and every
  // leaf's (level, ix, iy, iz).
  std::map<int, std::vector<fofplugin::Particle>> cells_;
  std::vector<int> leaf_desc_;
  int n_pieces_ = 0;

 public:
  HostPiece(std::string file, long n_total, int n_pieces) : n_pieces_(n_pieces) {
    const long start = n_total * thisIndex / n_pieces;
    const long end = n_total * (thisIndex + 1) / n_pieces;
    Tipsy::TipsyReader r(file);
    const Tipsy::header h = r.getHeader();
    if (!r.seekParticleNum(start)) CkAbort("fofhost: seek failed\n");
    parts_.resize(end - start);
    Tipsy::gas_particle gp;
    Tipsy::dark_particle dp;
    Tipsy::star_particle sp;
    for (long i = start; i < end; i++) {
      fofplugin::Particle& p = parts_[i - start];
      Vector3D<float> pos, vel;
      float mass;
      if (i < h.nsph) {
        if (!r.getNextGasParticle(gp)) CkAbort("fofhost: read failed\n");
        pos = gp.pos; vel = gp.vel; mass = gp.mass;
      } else if (i < h.nsph + h.ndark) {
        if (!r.getNextDarkParticle(dp)) CkAbort("fofhost: read failed\n");
        pos = dp.pos; vel = dp.vel; mass = dp.mass;
      } else {
        if (!r.getNextStarParticle(sp)) CkAbort("fofhost: read failed\n");
        pos = sp.pos; vel = sp.vel; mass = sp.mass;
      }
      p.x = pos.x; p.y = pos.y; p.z = pos.z;
      p.vx = vel.x; p.vy = vel.y; p.vz = vel.z;
      p.mass = mass;
      p.id = (uint64_t)i;  // the file index, as FoF3's Reader assigns
    }
  }
  HostPiece(CkMigrateMessage*) {}

  void stage(const CkCallback& cb) {
    if (leaf_desc_.empty()) {
      first_ = fofplugin::stage(parts_.data(), parts_.size());
    } else {
      // One stageCell per owned leaf; their staging indices are
      // consecutive, in the order parts_ now holds the particles.
      parts_.clear();
      bool first = true;
      for (auto& kv : cells_) {
        const int* d = &leaf_desc_[4 * kv.first];
        size_t at = fofplugin::stageCell(kv.second.data(), kv.second.size(),
                                         d[0], d[1], d[2], d[3]);
        if (first) { first_ = at; first = false; }
        parts_.insert(parts_.end(), kv.second.begin(), kv.second.end());
      }
    }
    contribute(cb);
  }

  void bbox(const CkCallback& cb) {
    double v[6] = {1e300, 1e300, 1e300, 1e300, 1e300, 1e300};
    for (const auto& p : parts_) {
      v[0] = std::min(v[0], p.x); v[1] = std::min(v[1], p.y); v[2] = std::min(v[2], p.z);
      v[3] = std::min(v[3], -p.x); v[4] = std::min(v[4], -p.y); v[5] = std::min(v[5], -p.z);
    }
    contribute(6 * sizeof(double), v, CkReduction::min_double, cb);
  }

  // Particles per cell of the finest level.
  void histogram(double lo_x, double lo_y, double lo_z, double side, int level,
                 const CkCallback& cb) {
    const long m = 1L << level;
    std::vector<long> h(m * m * m, 0);
    for (const auto& p : parts_) h[fineCell(p, lo_x, lo_y, lo_z, side, level)]++;
    contribute(h.size() * sizeof(long), h.data(), CkReduction::sum_long, cb);
  }

  // Send every particle to the piece that owns its leaf.
  void makeCells(double lo_x, double lo_y, double lo_z, double side, int level,
                 const std::vector<int>& fine_to_leaf,
                 const std::vector<int>& leaf_desc,
                 const std::vector<int>& leaf_owner, const CkCallback& cb) {
    leaf_desc_ = leaf_desc;
    std::map<int, std::vector<uint32_t>> cells;
    std::map<int, std::vector<uint64_t>> ids;
    std::map<int, std::vector<double>> vals;
    for (const auto& p : parts_) {
      const int leaf = fine_to_leaf[fineCell(p, lo_x, lo_y, lo_z, side, level)];
      CkEnforce(leaf >= 0);
      const int owner = leaf_owner[leaf];
      cells[owner].push_back((uint32_t)leaf);
      ids[owner].push_back(p.id);
      auto& v = vals[owner];
      v.insert(v.end(), {p.x, p.y, p.z, p.vx, p.vy, p.vz, p.mass});
    }
    parts_.clear();
    for (auto& kv : cells)
      thisProxy[kv.first].receiveCellParticles(kv.second, ids[kv.first], vals[kv.first]);
    contribute(cb);
  }

  static long fineCell(const fofplugin::Particle& p, double lo_x, double lo_y,
                       double lo_z, double side, int level) {
    const long m = 1L << level;
    const double w = side / m;
    auto coord = [&](double x, double lo) {
      return std::max(0L, std::min(m - 1, (long)std::floor((x - lo) / w)));
    };
    return (coord(p.x, lo_x) * m + coord(p.y, lo_y)) * m + coord(p.z, lo_z);
  }

  void receiveCellParticles(const std::vector<uint32_t>& cells,
                            const std::vector<uint64_t>& ids,
                            const std::vector<double>& vals) {
    for (size_t i = 0; i < cells.size(); i++) {
      fofplugin::Particle p;
      const double* v = &vals[7 * i];
      p.x = v[0]; p.y = v[1]; p.z = v[2];
      p.vx = v[3]; p.vy = v[4]; p.vz = v[5]; p.mass = v[6];
      p.id = ids[i];
      cells_[(int)cells[i]].push_back(p);
    }
  }

  // Count, wrapping digest sum, particles left unlabeled, and this PE's
  // catalog rows (groups, their particles, digest of (id, n)), counted by
  // the first piece on each PE only.
  void digest(int rep, const CkCallback& cb) {
    size_t n = 0;
    const long* labels = fofplugin::labels(n);
    unsigned long long v[6] = {0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < parts_.size(); i++) {
      const long label = labels[first_ + i];
      v[0]++;
      v[1] += fofplugin::labelDigestTerm((long)parts_[i].id, label);
      if (label < 0) v[2]++;
    }
    static thread_local int counted_rep = -1;
    if (counted_rep != rep) {
      counted_rep = rep;
      size_t ng = 0;
      const fofplugin::Group* g = fofplugin::groups(ng);
      for (size_t i = 0; i < ng; i++) {
        v[3]++;
        v[4] += g[i].n;
        v[5] += fofplugin::labelDigestTerm((long)g[i].id, (long)g[i].n);
      }
    }
    contribute(6 * sizeof(unsigned long long), v, CkReduction::sum_ulong_long, cb);
  }

  void clearStaged(const CkCallback& cb) {
    fofplugin::clear();
    contribute(cb);
  }
};

#include "fofhost.def.h"
