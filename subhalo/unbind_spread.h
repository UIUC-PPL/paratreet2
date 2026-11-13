// unbind_spread.h -- unbind groups whose particles stay on several
// processes (design/subhalo-checks.md, "Step 2", part 2c).
//
// A spread group is unbound by the rules of unbind_groups.h, but its
// particles stay where they are. One Driver keeps every spread group's
// state: its frame, its counts, its capped sample and, once its bound set is
// at or below the cap, the bound set itself. One Holder per process keeps
// that process's particles of every spread group. They work in rounds: the
// driver writes one command for all holders, every holder answers with one
// report, and the driver reads the reports in holder order. The transport is
// the caller's (FoF3: a broadcast and a reduction; unbind --spread: a loop
// in one process).
//
// Rounds:
//   origin  each holder reports, per group, its count and its lowest-id
//           particle; the group's lowest-id particle is its origin
//   pack    positions become offsets from the origin (nearest image), as
//           makeGroups makes them; holders report their extents, their sums
//           for the first frame and their particles whose sampling key is at
//           most a threshold (below)
//   pass    the driver sends each active group's sources: the sample, the
//           sample's change, the bound set (at or below the cap) or the
//           particles the last pass removed. Holders evaluate their bound
//           particles, remove the unbound ones, and report their counts, the
//           frame sums of what they kept, and which sample members (above
//           the cap) or which particles (at or below it) they removed
//   fetch   after a pass: the particles that may join the sample (the alive
//           ones after the last one taken, in sampling order, whose key is at
//           most a threshold), or, when a group falls to the cap, its bound
//           set
//
// Thresholds. The sampling key is a hash of the id, so the keys of the A
// alive particles after the last one taken are spread evenly over the keys
// above it. To find the next `need` of them the driver asks for every alive
// particle whose key is at most the key below which 1.2 need + 64 are
// expected, and asks again above that key in the few cases where fewer than
// `need` come back. So each fetch moves about 1.2 times what it needs, in
// total over all holders, whatever their number; asking each holder for its
// next `need` would move that many from every holder.
//   centre  each holder's K bound particles of lowest energy (one for a
//           group that is not refined)
//   refine  each holder's sums over its bound particles for the candidates
//   final   the results, kept by the holders
//
// What differs from unbind_groups.h on one process: the frame and the bound
// mass are sums of the holders' sums, added in holder order, and the frame
// is taken relative to the origin rather than to the first bound particle;
// the refinement's sums are the holders' sums added in holder order.
// Everything else is the same: the sample and its order, each particle's sum
// over a list of sources, the removal, the stopping rule, the switch to the
// exact rules at the cap, the subtraction and the stale-frame count. So the
// result depends on the layout only through the rounding of those sums, and
// is the same every time on a given layout. With one holder the sums are
// unbind_groups.h's own, and so are the bits, except in a group whose
// lowest-id particle is removed while the group is still being unbound (the
// frame's reference particle then differs).
//
// Needs unbind_groups.h (Kokkos); no Charm header.

#ifndef SUBHALO_UNBIND_SPREAD_H_
#define SUBHALO_UNBIND_SPREAD_H_

#include "unbind_groups.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace unbind {
namespace spread {

// ---- messages ---------------------------------------------------------------

class Out {
 public:
  template <class T>
  void put(const T& v) {
    const char* p = reinterpret_cast<const char*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
  }
  template <class T>
  void putVec(const std::vector<T>& v) {
    put<int64_t>((int64_t)v.size());
    const char* p = reinterpret_cast<const char*>(v.data());
    b.insert(b.end(), p, p + v.size() * sizeof(T));
  }
  std::vector<char> b;
};

class In {
 public:
  In(const char* p, size_t n) : p_(p), e_(p + n) {}
  template <class T>
  T get() {
    need(sizeof(T));
    T v;
    std::memcpy(&v, p_, sizeof(T));
    p_ += sizeof(T);
    return v;
  }
  template <class T>
  std::vector<T> getVec() {
    const int64_t n = get<int64_t>();
    if (n < 0) bad();
    need((size_t)n * sizeof(T));
    std::vector<T> v((size_t)n);
    if (n > 0) std::memcpy(v.data(), p_, (size_t)n * sizeof(T));
    p_ += (size_t)n * sizeof(T);
    return v;
  }
  bool atEnd() const { return p_ == e_; }

 private:
  void need(size_t k) {
    if ((size_t)(e_ - p_) < k) bad();
  }
  [[noreturn]] static void bad() {
    fprintf(stderr, "unbind::spread: a message is shorter than its contents\n");
    abort();
  }
  const char* p_;
  const char* e_;
};

// A source particle: id, offset from the group's origin, mass.
struct Src {
  int64_t pid;
  double x[3];
  double m;
};

// A candidate for the most bound particle.
struct Cand {
  int64_t pid;
  double x[3], v[3];
  double m, e;
};

enum Kind : int32_t { kOrigin = 1, kPack, kPass, kFetch, kCentre, kRefine, kFinal, kEnd };
enum State : uint8_t { Done, Full, Subtract, SampleFull, SampleUpdate };  // as unbindGroups
enum Fetch : uint8_t { kFetchNone = 0, kFetchSample = 1, kFetchBound = 2 };

// The sampling order of unbindGroups: sampleKey(id), ties by id.
inline bool keyLess(int64_t a, int64_t b) {
  const uint64_t ka = sampleKey(a), kb = sampleKey(b);
  return ka != kb ? ka < kb : a < b;
}

// A position in the sampling order: every particle at or before (key, id)
// has been looked at. (0, -1) is before every particle (ids are >= 0).
struct KeyPos {
  uint64_t key = 0;
  int64_t id = -1;
};
inline bool atOrBefore(int64_t id, const KeyPos& f) {
  const uint64_t k = sampleKey(id);
  return k != f.key ? k < f.key : id <= f.id;
}

// The key below which `want` of `avail` keys spread evenly above `from` are
// expected (the whole range if want >= avail).
inline uint64_t keyThreshold(uint64_t from, int64_t avail, double want) {
  if (avail <= 0 || want >= (double)avail) return ~0ull;
  const long double span = (long double)(~0ull - from);
  const long double t = (long double)from + span * ((long double)want / (long double)avail);
  return t >= (long double)~0ull ? ~0ull : (uint64_t)t;
}
inline double fetchWant(int64_t need) { return 1.2 * (double)need + 64.0; }

// ---- the holder -------------------------------------------------------------

// One launch from host rows (x, y, z, w per row, as unbindGroups builds
// them). Returns the kernel's seconds; out gets one sum per target.
using LaunchFn = double (*)(const std::vector<double>& tgt, const std::vector<int>& grp,
                            const std::vector<double>& src, const std::vector<int64_t>& off,
                            std::vector<double>& out, double eps, double box, bool self,
                            int64_t chunk);

template <class Real, class Acc, bool Fast>
double launchRows(const std::vector<double>& tgt, const std::vector<int>& grp,
                  const std::vector<double>& src, const std::vector<int64_t>& off,
                  std::vector<double>& out, double eps, double box, bool self, int64_t chunk) {
  using Kokkos::view_alloc;
  using Kokkos::WithoutInitializing;
  const size_t nt = grp.size(), ns = src.size() / 4;
  out.assign(nt, 0.0);
  if (nt == 0) return 0.0;
  ParticleView<Real> d_tgt(view_alloc(WithoutInitializing, "spread_tgt"), nt);
  ParticleView<Real> d_src(view_alloc(WithoutInitializing, "spread_src"), ns);
  GroupView d_grp(view_alloc(WithoutInitializing, "spread_group"), nt);
  OffsetView d_off(view_alloc(WithoutInitializing, "spread_off"), off.size());
  PotentialView d_phi(view_alloc(WithoutInitializing, "spread_phi"), nt);
  auto h_tgt = Kokkos::create_mirror_view(d_tgt);
  auto h_src = Kokkos::create_mirror_view(d_src);
  for (size_t t = 0; t < nt; t++)
    for (int c = 0; c < 4; c++) h_tgt(t, c) = Real(tgt[4 * t + c]);
  for (size_t s = 0; s < ns; s++)
    for (int c = 0; c < 4; c++) h_src(s, c) = Real(src[4 * s + c]);
  Kokkos::deep_copy(d_tgt, h_tgt);
  Kokkos::deep_copy(d_src, h_src);
  Kokkos::deep_copy(d_grp, Kokkos::View<const int*, Kokkos::HostSpace,
                                        Kokkos::MemoryTraits<Kokkos::Unmanaged>>(grp.data(), nt));
  Kokkos::deep_copy(d_off, Kokkos::View<const int64_t*, Kokkos::HostSpace,
                                        Kokkos::MemoryTraits<Kokkos::Unmanaged>>(off.data(),
                                                                                 off.size()));
  const double ks =
      chunk > 0 ? potentialSplit<Real, Acc, Fast>(
                      d_tgt, splitPlan(grp.data(), (int64_t)nt, off.data(), chunk), d_src, d_off,
                      d_phi, eps, box, self)
                : potential<Real, Acc, Fast>(d_tgt, d_grp, d_src, d_off, d_phi, eps, box, self);
  Kokkos::deep_copy(Kokkos::View<double*, Kokkos::HostSpace,
                                 Kokkos::MemoryTraits<Kokkos::Unmanaged>>(out.data(), nt),
                    d_phi);
  return ks;
}

// kernel 0 f64, 1 f32 summed in f64, 2 f32 summed in f32 (unbindGroups'
// three forms), each with or without the rsqrt form.
inline LaunchFn launchFor(int kernel, bool rsqrt) {
  switch (kernel) {
    case 0: return rsqrt ? &launchRows<double, double, true> : &launchRows<double, double, false>;
    case 1: return rsqrt ? &launchRows<float, double, true> : &launchRows<float, double, false>;
    default: return rsqrt ? &launchRows<float, float, true> : &launchRows<float, float, false>;
  }
}

class Holder {
 public:
  // This holder's particles of every spread group, sorted by (label, id):
  // label and id >= 0, pos comoving (any image), vel physical peculiar.
  Holder(int64_t n, const int64_t* label, const int64_t* id, const double* pos,
         const double* vel, const double* mass, const Params& P, LaunchFn launch)
      : P_(P), launch_(launch) {
    pid_.assign(id, id + n);
    pos_.assign(pos, pos + 3 * n);
    vel_.assign(vel, vel + 3 * n);
    m_.assign(mass, mass + n);
    alive_.assign((size_t)n, 1);
    in_sample_.assign((size_t)n, 0);
    phi_s_.assign((size_t)n, 0.0);
    energy_.assign((size_t)n, 0.0);
    refined_.assign((size_t)n, std::numeric_limits<double>::quiet_NaN());
    most_.assign((size_t)n, 0);
    for (int64_t b = 0; b < n;) {
      int64_t e = b + 1;
      while (e < n && label[e] == label[b]) e++;
      if (e - b > 0) {
        LG g;
        g.label = label[b];
        g.first = b;
        g.count = e - b;
        g_.push_back(std::move(g));
      }
      b = e;
    }
    for (int64_t i = 1; i < n; i++)
      if (label[i] < label[i - 1] || (label[i] == label[i - 1] && id[i] <= id[i - 1])) {
        fprintf(stderr, "unbind::spread::Holder: the input is not sorted by (label, id)\n");
        abort();
      }
  }

  std::vector<char> step(const char* cmd, size_t len) {
    Kokkos::Timer t;
    In in(cmd, len);
    const int32_t kind = in.get<int32_t>();
    Out out;
    out.put<int32_t>(kind);
    switch (kind) {
      case kOrigin: origin(out); break;
      case kPack: pack(in, out); break;
      case kPass: pass(in, out); break;
      case kFetch: fetch(in, out); break;
      case kCentre: centre(in, out); break;
      case kRefine: refine(in, out); break;
      case kFinal: final(in); break;
      case kEnd: break;
      default:
        fprintf(stderr, "unbind::spread::Holder: unknown command %d\n", (int)kind);
        abort();
    }
    if (!in.atEnd()) {
      fprintf(stderr, "unbind::spread::Holder: command %d is longer than its contents\n",
              (int)kind);
      abort();
    }
    step_s += t.seconds();
    return out.b;
  }

  // Results, after the final round, in input order.
  struct GroupResult {
    int64_t label, first, count;  // the run of this holder's particles
    int64_t nbound;               // the whole group's; 0 = no sub-halo
    int64_t centre;               // input index of the most bound particle if here; -1
    int passes;
    uint8_t refined_group, stale_frame;
  };
  std::vector<GroupResult> results() const {
    std::vector<GroupResult> r;
    for (const LG& g : g_) {
      GroupResult x;
      x.label = g.label;
      x.first = g.first;
      x.count = g.count;
      x.nbound = g.nbound;
      x.centre = g.centre;
      x.passes = g.passes;
      x.refined_group = g.refined_group;
      x.stale_frame = g.stale;
      r.push_back(x);
    }
    return r;
  }
  uint8_t bound(int64_t i) const { return alive_[i]; }  // after the final round
  double energy(int64_t i) const { return energy_[i]; }
  double refined(int64_t i) const { return refined_[i]; }
  uint8_t mostBound(int64_t i) const { return most_[i]; }
  int64_t size() const { return (int64_t)m_.size(); }
  int64_t groups() const { return (int64_t)g_.size(); }

  double kernel_s = 0, step_s = 0;
  int64_t launches = 0;

 private:
  struct LG {
    int64_t label = -1, first = 0, count = 0;
    bool wrap = false;
    std::vector<int64_t> bound, removed;  // input indices, id order
    std::vector<int64_t> korder;          // input indices in sampling order
    size_t kpos = 0;                      // korder before kpos: at or before the frontier
    int64_t nbound = 0, centre = -1;
    int passes = 0;
    uint8_t refined_group = 0, stale = 0;
  };

  LG* find(int64_t label) {
    auto it = std::lower_bound(g_.begin(), g_.end(), label,
                               [](const LG& g, int64_t l) { return g.label < l; });
    return it != g_.end() && it->label == label ? &*it : nullptr;
  }
  // Input index of particle `pid` of group g; -1 if not here.
  int64_t local(const LG& g, int64_t pid) const {
    const int64_t* b = &pid_[g.first];
    const int64_t* e = b + g.count;
    const int64_t* it = std::lower_bound(b, e, pid);
    return it != e && *it == pid ? g.first + (it - b) : -1;
  }
  void row(std::vector<double>& rows, const double* x, double w) const {
    rows.insert(rows.end(), {x[0], x[1], x[2], w});
  }
  void rowOf(std::vector<double>& rows, int64_t i, double w) const {
    row(rows, &pos_[3 * i], w);
  }
  double run(const std::vector<double>& tgt, const std::vector<int>& grp,
             const std::vector<double>& src, const std::vector<int64_t>& off, bool wrap,
             bool self, std::vector<double>& out, int64_t chunk = 0) {
    const double ks = launch_(tgt, grp, src, off, out, P_.eps, wrap ? P_.box : 0.0, self, chunk);
    kernel_s += ks;
    launches += grp.empty() ? 0 : 1;
    return ks;
  }
  double kinetic(int64_t i, const detail::Frame& f, bool wrap) const {
    double dv[3];
    detail::relativeVelocity(&pos_[3 * i], &vel_[3 * i], f, P_.H * P_.a, wrap, P_.box, dv);
    return 0.5 * (dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
  }

  void origin(Out& out) {
    out.put<int64_t>((int64_t)g_.size());
    for (const LG& g : g_) {
      out.put<int64_t>(g.label);
      out.put<int64_t>(g.count);
      out.put<int64_t>(pid_[g.first]);  // the lowest id here: the input is in id order
      for (int d = 0; d < 3; d++) out.put<double>(pos_[3 * g.first + d]);
    }
  }

  void pack(In& in, Out& out) {
    const int64_t n = in.get<int64_t>();
    std::vector<std::pair<LG*, std::vector<double>>> list;
    std::vector<uint64_t> tkey;
    for (int64_t k = 0; k < n; k++) {
      const int64_t label = in.get<int64_t>();
      std::vector<double> o(3);
      for (int d = 0; d < 3; d++) o[d] = in.get<double>();
      const uint64_t t = in.get<uint64_t>();
      if (LG* g = find(label)) {
        list.push_back({g, o});
        tkey.push_back(t);
      }
    }
    out.put<int64_t>((int64_t)list.size());
    for (size_t li = 0; li < list.size(); li++) {
      auto& e = list[li];
      LG& g = *e.first;
      const double* ref = e.second.data();
      double lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0}, msum = 0, mx[3] = {0, 0, 0},
             mv[3] = {0, 0, 0};
      for (int64_t i = g.first; i < g.first + g.count; i++) {
        const double m = m_[i];
        msum += m;
        for (int d = 0; d < 3; d++) {
          double x = pos_[3 * i + d] - ref[d];   // makeGroups' offsets
          if (P_.box > 0) x = nearestImage(x, P_.box);
          pos_[3 * i + d] = x;
          lo[d] = std::min(lo[d], x);
          hi[d] = std::max(hi[d], x);
          mx[d] += x * m;   // meanFrame from the origin, whose offset is 0
          mv[d] += vel_[3 * i + d] * m;
        }
      }
      g.bound.resize((size_t)g.count);
      for (int64_t k = 0; k < g.count; k++) g.bound[k] = g.first + k;
      g.korder = g.bound;
      std::sort(g.korder.begin(), g.korder.end(),
                [&](int64_t x, int64_t y) { return keyLess(pid_[x], pid_[y]); });
      g.kpos = 0;
      std::vector<Src> cand;   // every particle whose key is at most the threshold
      if (P_.cap > 0)
        for (int64_t k = 0; k < g.count && sampleKey(pid_[g.korder[k]]) <= tkey[li]; k++)
          cand.push_back(src(g.korder[k]));
      out.put<int64_t>(g.label);
      for (int d = 0; d < 3; d++) out.put<double>(lo[d]);
      for (int d = 0; d < 3; d++) out.put<double>(hi[d]);
      out.put<double>(msum);
      for (int d = 0; d < 3; d++) out.put<double>(mx[d]);
      for (int d = 0; d < 3; d++) out.put<double>(mv[d]);
      out.putVec(cand);
    }
  }

  Src src(int64_t i) const {
    Src s;
    s.pid = pid_[i];
    for (int d = 0; d < 3; d++) s.x[d] = pos_[3 * i + d];
    s.m = m_[i];
    return s;
  }

  struct PassItem {
    LG* g;
    uint8_t state;
    bool wrap;
    detail::Frame frame, old_frame;
    double scale;
    std::vector<Src> a, b;
  };

  void pass(In& in, Out& out) {
    const int64_t n = in.get<int64_t>();
    std::vector<PassItem> items;
    for (int64_t k = 0; k < n; k++) {
      PassItem it;
      const int64_t label = in.get<int64_t>();
      it.state = in.get<uint8_t>();
      it.wrap = in.get<uint8_t>() != 0;
      it.frame = in.get<detail::Frame>();
      it.old_frame = in.get<detail::Frame>();
      it.scale = in.get<double>();
      it.a = in.getVec<Src>();
      it.b = in.getVec<Src>();
      it.g = find(label);
      if (it.g) {
        it.g->wrap = it.wrap;
        items.push_back(std::move(it));
      }
    }
    // In-sample marks first (unbindGroups: 1 for a member, 2 for one that
    // joined at the last removal and whose own term is not yet cancelled).
    for (PassItem& it : items) {
      LG& g = *it.g;
      if (it.state == SampleFull) {
        for (int64_t i : g.bound) in_sample_[i] = 0;
        for (const Src& s : it.a) {
          const int64_t i = local(g, s.pid);
          if (i >= 0) in_sample_[i] = 1;
        }
      } else if (it.state == SampleUpdate) {
        for (const Src& s : it.a) {
          const int64_t i = local(g, s.pid);
          if (i >= 0) in_sample_[i] = 2;
        }
        for (const Src& s : it.b) {
          const int64_t i = local(g, s.pid);
          if (i >= 0) in_sample_[i] = 0;
        }
      }
    }
    // One launch per kind, as unbindGroups makes them.
    for (uint8_t kind : {(uint8_t)SampleFull, (uint8_t)SampleUpdate, (uint8_t)Full,
                         (uint8_t)Subtract}) {
      std::vector<double> tgt, srcs, res;
      std::vector<int> grp;
      std::vector<int64_t> off;
      std::vector<PassItem*> used;
      bool wrap = false;
      for (PassItem& it : items) {
        if (it.state != kind) continue;
        LG& g = *it.g;
        if (g.bound.empty()) continue;
        if (kind == SampleUpdate && it.a.empty() && it.b.empty()) continue;
        off.push_back((int64_t)srcs.size() / 4);
        for (int64_t i : g.bound) {
          double w = 0;
          if (kind == SampleFull) w = in_sample_[i] ? m_[i] : 0.0;
          if (kind == SampleUpdate) w = in_sample_[i] == 2 ? m_[i] : 0.0;
          if (kind == Full || kind == Subtract) w = m_[i];
          rowOf(tgt, i, w);
          grp.push_back((int)used.size());
        }
        if (kind == SampleUpdate) {
          for (const Src& s : it.a) row(srcs, s.x, s.m);
          for (const Src& s : it.b) row(srcs, s.x, -s.m);
        } else {
          for (const Src& s : it.a) row(srcs, s.x, s.m);
        }
        wrap = wrap || it.wrap;
        used.push_back(&it);
      }
      if (used.empty()) continue;
      off.push_back((int64_t)srcs.size() / 4);
      run(tgt, grp, srcs, off, wrap, kind != Subtract, res);
      size_t t = 0;
      for (PassItem* it : used) {
        LG& g = *it->g;
        if (kind == SampleFull) {
          for (int64_t i : g.bound) phi_s_[i] = res[t++];
        } else if (kind == SampleUpdate) {
          for (int64_t i : g.bound) phi_s_[i] += res[t++];
        } else if (kind == Full) {
          for (int64_t i : g.bound)
            energy_[i] = kinetic(i, it->frame, it->wrap) + it->scale * res[t++];
        } else {
          double shift[3];
          detail::relativeVelocity(it->old_frame.x, it->old_frame.v, it->frame, P_.H * P_.a,
                                   it->wrap, P_.box, shift);
          const double dK =
              0.5 * (shift[0] * shift[0] + shift[1] * shift[1] + shift[2] * shift[2]);
          for (int64_t i : g.bound) {
            double dv[3];
            detail::relativeVelocity(&pos_[3 * i], &vel_[3 * i], it->old_frame, P_.H * P_.a,
                                     it->wrap, P_.box, dv);
            energy_[i] += dv[0] * shift[0] + dv[1] * shift[1] + dv[2] * shift[2] + dK -
                          it->scale * res[t++];
          }
        }
      }
    }
    // Sampled groups: energies from the sums over the sample.
    for (PassItem& it : items) {
      if (it.state != SampleFull && it.state != SampleUpdate) continue;
      LG& g = *it.g;
      for (int64_t i : g.bound) energy_[i] = kinetic(i, it.frame, it.wrap) + it.scale * phi_s_[i];
      if (it.state == SampleUpdate)
        for (const Src& s : it.a) {
          const int64_t i = local(g, s.pid);
          if (i >= 0) in_sample_[i] = 1;
        }
    }
    // Removal (stable: the bound list stays in id order) and the report.
    out.put<int64_t>((int64_t)items.size());
    for (PassItem& it : items) {
      LG& g = *it.g;
      g.passes++;
      std::vector<int64_t> kept, gone;
      std::vector<int64_t> removed_pids;
      for (int64_t i : g.bound) {
        if (energy_[i] < 0) {
          kept.push_back(i);
        } else {
          gone.push_back(i);
          alive_[i] = 0;
          const bool sampled = it.state == SampleFull || it.state == SampleUpdate;
          if (!sampled || in_sample_[i]) removed_pids.push_back(pid_[i]);
        }
      }
      g.bound.swap(kept);
      g.removed.swap(gone);
      double msum = 0, mx[3] = {0, 0, 0}, mv[3] = {0, 0, 0};
      for (int64_t i : g.bound) {
        const double m = m_[i];
        msum += m;
        for (int d = 0; d < 3; d++) {
          mx[d] += pos_[3 * i + d] * m;
          mv[d] += vel_[3 * i + d] * m;
        }
      }
      out.put<int64_t>(g.label);
      out.put<int64_t>((int64_t)g.bound.size());
      out.put<double>(msum);
      for (int d = 0; d < 3; d++) out.put<double>(mx[d]);
      for (int d = 0; d < 3; d++) out.put<double>(mv[d]);
      out.putVec(removed_pids);
    }
  }

  void fetch(In& in, Out& out) {
    const int64_t n = in.get<int64_t>();
    std::vector<std::pair<LG*, std::vector<Src>>> res;
    for (int64_t k = 0; k < n; k++) {
      const int64_t label = in.get<int64_t>();
      const uint8_t what = in.get<uint8_t>();
      const KeyPos frontier = in.get<KeyPos>();
      const uint64_t tkey = in.get<uint64_t>();
      LG* g = find(label);
      if (!g) continue;
      std::vector<Src> list;
      if (what == kFetchSample) {
        while (g->kpos < g->korder.size() && atOrBefore(pid_[g->korder[g->kpos]], frontier))
          g->kpos++;
        for (size_t k2 = g->kpos;
             k2 < g->korder.size() && sampleKey(pid_[g->korder[k2]]) <= tkey; k2++)
          if (alive_[g->korder[k2]]) list.push_back(src(g->korder[k2]));
      } else {
        for (int64_t i : g->bound) list.push_back(src(i));
      }
      res.push_back({g, std::move(list)});
    }
    out.put<int64_t>((int64_t)res.size());
    for (auto& e : res) {
      out.put<int64_t>(e.first->label);
      out.putVec(e.second);
    }
  }

  void centre(In& in, Out& out) {
    const int64_t n = in.get<int64_t>();
    std::vector<std::pair<LG*, std::vector<Cand>>> res;
    for (int64_t k = 0; k < n; k++) {
      const int64_t label = in.get<int64_t>();
      const int64_t want = in.get<int64_t>();
      LG* g = find(label);
      if (!g) continue;
      std::vector<int64_t> c = g->bound;
      const size_t m = (size_t)std::min<int64_t>(want, (int64_t)c.size());
      std::partial_sort(c.begin(), c.begin() + m, c.end(), [&](int64_t x, int64_t y) {
        return energy_[x] != energy_[y] ? energy_[x] < energy_[y] : pid_[x] < pid_[y];
      });
      std::vector<Cand> list;
      for (size_t r = 0; r < m; r++) {
        const int64_t i = c[r];
        Cand q;
        q.pid = pid_[i];
        for (int d = 0; d < 3; d++) {
          q.x[d] = pos_[3 * i + d];
          q.v[d] = vel_[3 * i + d];
        }
        q.m = m_[i];
        q.e = energy_[i];
        list.push_back(q);
      }
      res.push_back({g, std::move(list)});
    }
    out.put<int64_t>((int64_t)res.size());
    for (auto& e : res) {
      out.put<int64_t>(e.first->label);
      out.putVec(e.second);
    }
  }

  void refine(In& in, Out& out) {
    const int64_t n = in.get<int64_t>();
    std::vector<double> tgt, srcs, res;
    std::vector<int> grp;
    std::vector<int64_t> off;
    std::vector<std::pair<LG*, size_t>> used;  // group, candidates
    std::vector<std::pair<int64_t, size_t>> empty;  // label, candidates: no bound particle here
    bool wrap = false;
    for (int64_t k = 0; k < n; k++) {
      const int64_t label = in.get<int64_t>();
      const bool w = in.get<uint8_t>() != 0;
      const std::vector<Src> cand = in.getVec<Src>();
      LG* g = find(label);
      if (!g) continue;
      if (g->bound.empty()) {
        empty.push_back({label, cand.size()});
        continue;
      }
      off.push_back((int64_t)srcs.size() / 4);
      for (const Src& s : cand) {
        const int64_t i = local(*g, s.pid);
        row(tgt, s.x, i >= 0 && alive_[i] ? s.m : 0.0);  // its own term is here
        grp.push_back((int)used.size());
      }
      for (int64_t i : g->bound) rowOf(srcs, i, m_[i]);
      wrap = wrap || w;
      used.push_back({g, cand.size()});
    }
    if (!used.empty()) {
      off.push_back((int64_t)srcs.size() / 4);
      run(tgt, grp, srcs, off, wrap, true, res, P_.refine_chunk);
    }
    out.put<int64_t>((int64_t)(used.size() + empty.size()));
    size_t t = 0;
    for (auto& e : used) {
      out.put<int64_t>(e.first->label);
      out.putVec(std::vector<double>(res.begin() + t, res.begin() + t + e.second));
      t += e.second;
    }
    for (auto& e : empty) {
      out.put<int64_t>(e.first);
      out.putVec(std::vector<double>(e.second, 0.0));
    }
  }

  void final(In& in) {
    const int64_t n = in.get<int64_t>();
    for (int64_t k = 0; k < n; k++) {
      const int64_t label = in.get<int64_t>();
      const int64_t nbound = in.get<int64_t>();
      const int passes = in.get<int32_t>();
      const uint8_t stale = in.get<uint8_t>();
      const uint8_t refined_group = in.get<uint8_t>();
      const int64_t centre = in.get<int64_t>();
      const std::vector<int64_t> cpid = in.getVec<int64_t>();
      const std::vector<double> cref = in.getVec<double>();
      LG* g = find(label);
      if (!g) continue;
      if (passes != g->passes) {
        fprintf(stderr, "unbind::spread::Holder: group %lld: %d passes here, %d at the driver\n",
                (long long)label, g->passes, passes);
        abort();
      }
      g->nbound = nbound;
      g->stale = stale;
      g->refined_group = refined_group;
      if (nbound == 0)
        for (int64_t i = g->first; i < g->first + g->count; i++) alive_[i] = 0;
      for (size_t r = 0; r < cpid.size(); r++) {
        const int64_t i = local(*g, cpid[r]);
        if (i >= 0) refined_[i] = cref[r];
      }
      if (nbound > 0) {
        const int64_t i = local(*g, centre);
        if (i >= 0) {
          most_[i] = 1;
          g->centre = i;
        }
      }
    }
  }

  Params P_;
  LaunchFn launch_;
  std::vector<LG> g_;
  std::vector<int64_t> pid_;
  std::vector<double> pos_, vel_, m_, phi_s_, energy_, refined_;
  std::vector<uint8_t> alive_, in_sample_, most_;
};

// ---- the driver -------------------------------------------------------------

struct DriverStats {
  int64_t groups = 0, sampled = 0, refined = 0, subhaloes = 0, bound = 0, stale = 0;
  int rounds = 0, passes_max = 0;
  double pairs_first = 0, pairs_later = 0, pairs_refine = 0;
  double bytes_out = 0, bytes_in = 0;   // commands sent, reports read
  int64_t refetches = 0;                // fetches repeated because too few came back
};

class Driver {
 public:
  explicit Driver(const Params& P) : P_(P) {}

  std::vector<char> start() {
    phase_ = kOrigin;
    Out o;
    o.put<int32_t>(kOrigin);
    return sent(o);
  }

  // The reports of the last round, in holder order; returns the next command.
  std::vector<char> next(const std::vector<std::vector<char>>& reports) {
    for (const auto& r : reports) stats.bytes_in += (double)r.size();
    std::vector<In> in;
    for (const auto& r : reports) {
      in.emplace_back(r.data(), r.size());
      const int32_t kind = in.back().get<int32_t>();
      if (kind != phase_) {
        fprintf(stderr, "unbind::spread::Driver: a report of round %d in round %d\n", (int)kind,
                (int)phase_);
        abort();
      }
    }
    switch (phase_) {
      case kOrigin: readOrigin(in); return packCmd();
      case kPack: readPack(in); return afterPass();
      case kPass: readPass(in); return afterPass();
      case kFetch: readFetch(in); return afterPass();
      case kCentre: return readCentre(in);
      case kRefine: readRefine(in); return finalCmd();
      case kFinal: {
        phase_ = kEnd;
        Out o;
        o.put<int32_t>(kEnd);
        return sent(o);
      }
      default:
        fprintf(stderr, "unbind::spread::Driver: next() after the end\n");
        abort();
    }
  }

  bool done() const { return phase_ == kEnd; }
  DriverStats stats;

  // Per group, after the end (label order).
  struct GroupResult {
    int64_t label, n, nbound, centre;  // centre: the most bound particle's id; -1
    int passes;
    uint8_t refined_group, stale_frame, wide;
    int holders, first_holder;         // how many holders have its particles; the first
    double bound_mass;
    double x[3], v[3];  // frame of the bound set: x in the holders' input
                        // coordinates (the origin's image), v as input
  };
  std::vector<GroupResult> results() const {
    std::vector<GroupResult> r;
    for (const DG& g : g_) {
      GroupResult x;
      x.label = g.label;
      x.n = g.n;
      x.nbound = g.nb;
      x.centre = g.nb > 0 ? g.centre : -1;
      x.passes = g.passes;
      x.refined_group = g.nb > 0 ? g.refined_group : 0;
      x.stale_frame = g.stale;
      x.wide = (uint8_t)g.wrap;
      x.holders = g.holders;
      x.first_holder = g.first_holder;
      x.bound_mass = g.nb > 0 ? g.mb : 0.0;
      for (int d = 0; d < 3; d++) {
        x.x[d] = g.nb > 0 ? g.origin[d] + g.frame.x[d] : 0.0;
        x.v[d] = g.nb > 0 ? g.frame.v[d] : 0.0;
      }
      r.push_back(x);
    }
    return r;
  }

 private:
  struct DG {
    int64_t label = -1, n = 0, origin_pid = std::numeric_limits<int64_t>::max();
    double origin[3] = {0, 0, 0};
    int holders = 0, first_holder = -1;
    bool wrap = false;
    uint8_t state = Full;
    detail::Frame frame{}, old_frame{};
    double f = 1, mb = 0;
    int64_t nb = 0, nlast = 0;
    int passes = 0;
    uint8_t subtracted = 0, stale = 0, refined_group = 0;
    std::vector<Src> sample, added, dropped;  // the sample in unbindGroups' order
    KeyPos frontier;                          // the sample is taken up to here
    uint64_t tkey = ~0ull;                    // the fetch's key threshold
    int64_t avail = 0;                        // alive particles after the frontier
    bool building = false;                    // the first sample is not complete yet
    std::vector<Src> boundset, removedset;    // at or below the cap, id order
    uint8_t fetch = kFetchNone;
    int64_t need = 0;
    std::vector<Src> fetched;
    // pass bookkeeping: what this pass was and the sums that came back
    bool active = false;
    int64_t kept = 0;
    double msum = 0, mx[3] = {0, 0, 0}, mv[3] = {0, 0, 0};
    std::vector<int64_t> removed_pids;
    std::vector<Cand> cand;
    int64_t want = 0;
    std::vector<double> rsum;
    std::vector<int64_t> cpid;
    std::vector<double> cref;
    int64_t centre = -1;
  };

  DG& at(int64_t label) {
    auto it = std::lower_bound(g_.begin(), g_.end(), label,
                               [](const DG& g, int64_t l) { return g.label < l; });
    if (it == g_.end() || it->label != label) {
      fprintf(stderr, "unbind::spread::Driver: a report names group %lld, which is not spread\n",
              (long long)label);
      abort();
    }
    return *it;
  }

  std::vector<char> sent(Out& o) {
    stats.rounds++;
    stats.bytes_out += (double)o.b.size();
    return std::move(o.b);
  }

  void readOrigin(std::vector<In>& in) {
    std::vector<DG> seen;
    for (size_t h = 0; h < in.size(); h++) {
      In& r = in[h];
      const int64_t n = r.get<int64_t>();
      for (int64_t k = 0; k < n; k++) {
        DG g;
        g.label = r.get<int64_t>();
        g.n = r.get<int64_t>();
        g.origin_pid = r.get<int64_t>();
        for (int d = 0; d < 3; d++) g.origin[d] = r.get<double>();
        g.holders = 1;
        g.first_holder = (int)h;
        seen.push_back(g);
      }
    }
    std::stable_sort(seen.begin(), seen.end(),
                     [](const DG& a, const DG& b) { return a.label < b.label; });
    for (const DG& s : seen) {
      if (g_.empty() || g_.back().label != s.label) {
        g_.push_back(s);
        continue;
      }
      DG& g = g_.back();
      g.n += s.n;
      g.holders++;
      if (s.origin_pid < g.origin_pid) {
        g.origin_pid = s.origin_pid;
        for (int d = 0; d < 3; d++) g.origin[d] = s.origin[d];
      }
    }
    stats.groups = (int64_t)g_.size();
  }

  std::vector<char> packCmd() {
    phase_ = kPack;
    Out o;
    o.put<int32_t>(kPack);
    o.put<int64_t>((int64_t)g_.size());
    for (DG& g : g_) {
      o.put<int64_t>(g.label);
      for (int d = 0; d < 3; d++) o.put<double>(g.origin[d]);
      g.tkey = keyThreshold(0, g.n, fetchWant(P_.cap));
      o.put<uint64_t>(g.tkey);
    }
    return sent(o);
  }

  void readPack(std::vector<In>& in) {
    for (DG& g : g_) {
      g.msum = 0;
      for (int d = 0; d < 3; d++) g.mx[d] = g.mv[d] = 0;
      g.fetched.clear();
    }
    std::vector<std::vector<double>> lo(g_.size(), std::vector<double>(3, 0.0)),
        hi(g_.size(), std::vector<double>(3, 0.0));
    for (In& r : in) {
      const int64_t n = r.get<int64_t>();
      for (int64_t k = 0; k < n; k++) {
        DG& g = at(r.get<int64_t>());
        const size_t gi = &g - g_.data();
        for (int d = 0; d < 3; d++) lo[gi][d] = std::min(lo[gi][d], r.get<double>());
        for (int d = 0; d < 3; d++) hi[gi][d] = std::max(hi[gi][d], r.get<double>());
        g.msum += r.get<double>();
        for (int d = 0; d < 3; d++) g.mx[d] += r.get<double>();
        for (int d = 0; d < 3; d++) g.mv[d] += r.get<double>();
        const std::vector<Src> c = r.getVec<Src>();
        g.fetched.insert(g.fetched.end(), c.begin(), c.end());
      }
    }
    for (size_t gi = 0; gi < g_.size(); gi++) {
      DG& g = g_[gi];
      g.wrap = P_.box > 0 && (hi[gi][0] - lo[gi][0] >= 0.5 * P_.box ||
                              hi[gi][1] - lo[gi][1] >= 0.5 * P_.box ||
                              hi[gi][2] - lo[gi][2] >= 0.5 * P_.box);
      for (int d = 0; d < 3; d++) {
        g.frame.x[d] = g.mx[d] / g.msum + 0.0;
        g.frame.v[d] = g.mv[d] / g.msum;
      }
      g.old_frame = g.frame;
      g.nb = g.nlast = g.n;
      g.mb = g.msum;
      g.active = true;
      if (P_.cap > 0 && g.n > P_.cap) {
        g.state = SampleFull;
        g.sample.clear();
        g.frontier = KeyPos();
        g.avail = g.n;
        g.need = P_.cap;
        g.building = true;
        takeSample(g);   // the first sample, or as much of it as came back
        stats.sampled++;
      } else {
        g.state = Full;       // at or below the cap from the start
        g.fetch = kFetchBound;
      }
      g.fetched.clear();
      g.added.clear();
      g.dropped.clear();
    }
  }

  // After a pass (or the pack): fetch what the next pass needs, or run it,
  // or go on to the centres.
  std::vector<char> afterPass() {
    bool any = false;
    for (const DG& g : g_) any = any || (g.active && g.fetch != kFetchNone);
    if (!any) return passOrCentre();
    phase_ = kFetch;
    Out o;
    o.put<int32_t>(kFetch);
    int64_t n = 0;
    for (const DG& g : g_) n += g.active && g.fetch != kFetchNone;
    o.put<int64_t>(n);
    for (const DG& g : g_) {
      if (!(g.active && g.fetch != kFetchNone)) continue;
      o.put<int64_t>(g.label);
      o.put<uint8_t>(g.fetch);
      o.put<KeyPos>(g.frontier);
      o.put<uint64_t>(g.tkey);
    }
    return sent(o);
  }

  void readFetch(std::vector<In>& in) {
    for (DG& g : g_) g.fetched.clear();
    for (In& r : in) {
      const int64_t n = r.get<int64_t>();
      for (int64_t k = 0; k < n; k++) {
        DG& g = at(r.get<int64_t>());
        const std::vector<Src> c = r.getVec<Src>();
        g.fetched.insert(g.fetched.end(), c.begin(), c.end());
      }
    }
    for (DG& g : g_) {
      if (!(g.active && g.fetch != kFetchNone)) continue;
      if (g.fetch == kFetchBound) {
        std::sort(g.fetched.begin(), g.fetched.end(),
                  [](const Src& a, const Src& b) { return a.pid < b.pid; });
        if ((int64_t)g.fetched.size() != g.nb) {
          fprintf(stderr, "unbind::spread::Driver: group %lld: %zu bound particles fetched, "
                          "%lld expected\n", (long long)g.label, g.fetched.size(),
                  (long long)g.nb);
          abort();
        }
        g.boundset.swap(g.fetched);
        g.removedset.clear();
        g.fetch = kFetchNone;
      } else {
        takeSample(g);
      }
      g.fetched.clear();
    }
  }

  // The particles that came back for the sample (every alive one after the
  // frontier with key at most g.tkey, from all holders): take the next
  // g.need of them in sampling order. If fewer came back, take them all,
  // move the frontier to the threshold and ask again above it.
  void takeSample(DG& g) {
    std::sort(g.fetched.begin(), g.fetched.end(),
              [](const Src& a, const Src& b) { return keyLess(a.pid, b.pid); });
    const int64_t take = std::min<int64_t>(g.need, (int64_t)g.fetched.size());
    g.sample.insert(g.sample.end(), g.fetched.begin(), g.fetched.begin() + take);
    if (!g.building) g.added.insert(g.added.end(), g.fetched.begin(), g.fetched.begin() + take);
    g.need -= take;
    g.avail -= (int64_t)g.fetched.size();
    g.fetched.clear();
    if (g.need > 0) {
      if (g.tkey == ~0ull) {
        fprintf(stderr, "unbind::spread::Driver: group %lld: %lld more particles needed for the "
                        "sample, none left\n", (long long)g.label, (long long)g.need);
        abort();
      }
      g.frontier = KeyPos{g.tkey, std::numeric_limits<int64_t>::max()};
      g.tkey = keyThreshold(g.tkey, std::max<int64_t>(g.avail, g.need), fetchWant(g.need));
      g.fetch = kFetchSample;
      stats.refetches++;
      return;
    }
    const Src& last = g.sample.back();
    g.frontier = KeyPos{sampleKey(last.pid), last.pid};
    g.fetch = kFetchNone;
    if (g.building) {
      g.building = false;
      double ms = 0;
      for (const Src& s : g.sample) ms += s.m;
      g.f = g.mb / ms;
      g.state = SampleFull;
    } else {
      finishSample(g);
    }
  }

  // unbindGroups after it has rebuilt a sample: f, and the next state.
  void finishSample(DG& g) {
    double ms = 0;
    for (const Src& s : g.sample) ms += s.m;
    g.f = g.mb / ms;
    g.state = 2 * (int64_t)g.dropped.size() < P_.cap ? SampleUpdate : SampleFull;
  }

  std::vector<char> passOrCentre() {
    bool any = false;
    for (const DG& g : g_) any = any || g.active;
    if (!any) return centreCmd();
    phase_ = kPass;
    Out o;
    o.put<int32_t>(kPass);
    int64_t n = 0;
    for (const DG& g : g_) n += g.active;
    o.put<int64_t>(n);
    const double Ga = P_.G / P_.a;
    const bool first = !passed_;
    passed_ = true;
    static const std::vector<Src> none;
    for (const DG& g : g_) {
      if (!g.active) continue;
      o.put<int64_t>(g.label);
      o.put<uint8_t>(g.state);
      o.put<uint8_t>(g.wrap ? 1 : 0);
      o.put<detail::Frame>(g.frame);
      o.put<detail::Frame>(g.old_frame);
      const bool sampled = g.state == SampleFull || g.state == SampleUpdate;
      o.put<double>(sampled ? Ga * g.f : Ga);
      const std::vector<Src>* a = &none;
      const std::vector<Src>* b = &none;
      double pairs = 0;
      switch (g.state) {
        case SampleFull: a = &g.sample; pairs = (double)g.nb * (double)g.sample.size(); break;
        case SampleUpdate:
          a = &g.added;
          b = &g.dropped;
          pairs = (double)g.nb * (double)(g.added.size() + g.dropped.size());
          break;
        case Full: a = &g.boundset; pairs = (double)g.nb * (double)g.nb; break;
        default: a = &g.removedset; pairs = (double)g.nb * (double)g.removedset.size(); break;
      }
      if (first) stats.pairs_first += pairs;
      else stats.pairs_later += pairs;
      o.putVec(*a);
      o.putVec(*b);
    }
    return sent(o);
  }

  void readPass(std::vector<In>& in) {
    for (DG& g : g_) {
      g.kept = 0;
      g.msum = 0;
      for (int d = 0; d < 3; d++) g.mx[d] = g.mv[d] = 0;
      g.removed_pids.clear();
    }
    for (In& r : in) {
      const int64_t n = r.get<int64_t>();
      for (int64_t k = 0; k < n; k++) {
        DG& g = at(r.get<int64_t>());
        g.kept += r.get<int64_t>();
        g.msum += r.get<double>();
        for (int d = 0; d < 3; d++) g.mx[d] += r.get<double>();
        for (int d = 0; d < 3; d++) g.mv[d] += r.get<double>();
        const std::vector<int64_t> p = r.getVec<int64_t>();
        g.removed_pids.insert(g.removed_pids.end(), p.begin(), p.end());
      }
    }
    const float keep = (float)P_.bound_fraction;
    for (DG& g : g_) {
      if (!g.active) continue;
      // unbindGroups' "remove the unbound particles and decide what each
      // group does next", from the holders' sums.
      g.passes++;
      g.nlast = g.nb;
      g.nb = g.kept;
      if (g.nb < P_.min_bound || g.nb == 0) {
        g.nb = 0;
        g.state = Done;
        g.active = false;
        continue;
      }
      const int64_t removed = g.nlast - g.nb;
      const bool subtract_next = removed < g.nb;
      if (subtract_next) g.old_frame = g.frame;
      for (int d = 0; d < 3; d++) {
        g.frame.x[d] = g.mx[d] / g.msum + 0.0;
        g.frame.v[d] = g.mv[d] / g.msum;
      }
      g.mb = g.msum;
      const bool sampled = g.state == SampleFull || g.state == SampleUpdate;
      if (!sampled) {   // the bound set is here: take the removed ones out
        std::unordered_set<int64_t> gone(g.removed_pids.begin(), g.removed_pids.end());
        std::vector<Src> keepset;
        g.removedset.clear();
        for (const Src& s : g.boundset) (gone.count(s.pid) ? g.removedset : keepset).push_back(s);
        g.boundset.swap(keepset);
        if ((int64_t)g.boundset.size() != g.nb) {
          fprintf(stderr, "unbind::spread::Driver: group %lld: bound set %zu, holders %lld\n",
                  (long long)g.label, g.boundset.size(), (long long)g.nb);
          abort();
        }
      }
      if ((float)g.nb >= (float)g.nlast * keep) {
        g.state = Done;
        g.active = false;
        continue;
      }
      if (sampled) {
        if (g.nb <= P_.cap) {
          g.state = Full;   // at or below the cap: the exact rules from here on
          g.fetch = kFetchBound;
          continue;
        }
        std::unordered_set<int64_t> gone(g.removed_pids.begin(), g.removed_pids.end());
        g.added.clear();
        g.dropped.clear();
        size_t w = 0;
        for (const Src& s : g.sample) {
          if (gone.count(s.pid)) g.dropped.push_back(s);
          else g.sample[w++] = s;
        }
        g.sample.resize(w);
        g.need = P_.cap - (int64_t)g.sample.size();
        // Every alive particle at or before the frontier is a sample member.
        g.avail = g.nb - (int64_t)g.sample.size();
        if (g.need > 0) {
          g.tkey = keyThreshold(g.frontier.key, g.avail, fetchWant(g.need));
          g.fetch = kFetchSample;
        } else {
          finishSample(g);
        }
        continue;
      }
      if (!subtract_next && g.subtracted) g.stale = 1;
      if (subtract_next) g.subtracted = 1;
      g.state = subtract_next ? Subtract : Full;
    }
  }

  std::vector<char> centreCmd() {
    phase_ = kCentre;
    Out o;
    o.put<int32_t>(kCentre);
    int64_t n = 0;
    for (const DG& g : g_) n += g.nb > 0;
    o.put<int64_t>(n);
    for (DG& g : g_) {
      if (g.nb == 0) continue;
      g.refined_group = P_.refine > 0 && P_.cap > 0 && g.nb > P_.cap;
      g.want = g.refined_group ? std::min(P_.refine, g.nb) : 1;
      o.put<int64_t>(g.label);
      o.put<int64_t>(g.want);
    }
    return sent(o);
  }

  std::vector<char> readCentre(std::vector<In>& in) {
    for (DG& g : g_) g.cand.clear();
    for (In& r : in) {
      const int64_t n = r.get<int64_t>();
      for (int64_t k = 0; k < n; k++) {
        DG& g = at(r.get<int64_t>());
        const std::vector<Cand> c = r.getVec<Cand>();
        g.cand.insert(g.cand.end(), c.begin(), c.end());
      }
    }
    bool any = false;
    for (DG& g : g_) {
      if (g.nb == 0) continue;
      std::sort(g.cand.begin(), g.cand.end(), [](const Cand& a, const Cand& b) {
        return a.e != b.e ? a.e < b.e : a.pid < b.pid;
      });
      if ((int64_t)g.cand.size() < g.want) {
        fprintf(stderr, "unbind::spread::Driver: group %lld: %zu candidates, %lld wanted\n",
                (long long)g.label, g.cand.size(), (long long)g.want);
        abort();
      }
      g.cand.resize((size_t)g.want);
      if (!g.refined_group) g.centre = g.cand[0].pid;
      any = any || g.refined_group;
    }
    if (!any) return finalCmd();
    phase_ = kRefine;
    Out o;
    o.put<int32_t>(kRefine);
    int64_t n = 0;
    for (const DG& g : g_) n += g.nb > 0 && g.refined_group;
    o.put<int64_t>(n);
    for (DG& g : g_) {
      if (!(g.nb > 0 && g.refined_group)) continue;
      o.put<int64_t>(g.label);
      o.put<uint8_t>(g.wrap ? 1 : 0);
      std::vector<Src> s;
      for (const Cand& c : g.cand) s.push_back(Src{c.pid, {c.x[0], c.x[1], c.x[2]}, c.m});
      o.putVec(s);
      stats.pairs_refine += (double)g.cand.size() * (double)g.nb;
    }
    stats.refined = n;
    return sent(o);
  }

  void readRefine(std::vector<In>& in) {
    for (DG& g : g_) g.rsum.assign(g.refined_group && g.nb > 0 ? g.cand.size() : 0, 0.0);
    for (In& r : in) {
      const int64_t n = r.get<int64_t>();
      for (int64_t k = 0; k < n; k++) {
        DG& g = at(r.get<int64_t>());
        const std::vector<double> s = r.getVec<double>();
        if (s.size() != g.rsum.size()) {
          fprintf(stderr, "unbind::spread::Driver: group %lld: %zu sums for %zu candidates\n",
                  (long long)g.label, s.size(), g.rsum.size());
          abort();
        }
        for (size_t t = 0; t < s.size(); t++) g.rsum[t] += s[t];
      }
    }
    const double Ga = P_.G / P_.a, Ha = P_.H * P_.a;
    for (DG& g : g_) {
      if (!(g.nb > 0 && g.refined_group)) continue;
      g.cpid.clear();
      g.cref.clear();
      int64_t best = -1;
      double best_e = 0;
      for (size_t r = 0; r < g.cand.size(); r++) {
        const Cand& c = g.cand[r];
        double dv[3];
        detail::relativeVelocity(c.x, c.v, g.frame, Ha, g.wrap, P_.box, dv);
        const double e = 0.5 * (dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]) + Ga * g.rsum[r];
        g.cpid.push_back(c.pid);
        g.cref.push_back(e);
        if (best < 0 || e < best_e || (e == best_e && c.pid < best)) {
          best = c.pid;
          best_e = e;
        }
      }
      g.centre = best;
    }
  }

  std::vector<char> finalCmd() {
    phase_ = kFinal;
    Out o;
    o.put<int32_t>(kFinal);
    o.put<int64_t>((int64_t)g_.size());
    for (const DG& g : g_) {
      o.put<int64_t>(g.label);
      o.put<int64_t>(g.nb);
      o.put<int32_t>(g.passes);
      o.put<uint8_t>(g.stale);
      o.put<uint8_t>(g.nb > 0 ? g.refined_group : 0);
      o.put<int64_t>(g.nb > 0 ? g.centre : -1);
      o.putVec(g.nb > 0 && g.refined_group ? g.cpid : std::vector<int64_t>());
      o.putVec(g.nb > 0 && g.refined_group ? g.cref : std::vector<double>());
      stats.subhaloes += g.nb > 0;
      stats.bound += g.nb;
      stats.stale += g.stale;
      stats.passes_max = std::max(stats.passes_max, g.passes);
    }
    return sent(o);
  }

  Params P_;
  std::vector<DG> g_;
  int32_t phase_ = 0;
  bool passed_ = false;
};

// One process, H holders: the rounds as a loop (unbind --spread, and the
// checks). Each holder is given its particles; returns the driver's stats.
inline DriverStats runInProcess(Driver& D, std::vector<Holder*>& holders) {
  std::vector<char> cmd = D.start();
  while (!D.done()) {
    std::vector<std::vector<char>> reports;
    for (Holder* h : holders) reports.push_back(h->step(cmd.data(), cmd.size()));
    cmd = D.next(reports);
  }
  for (Holder* h : holders) h->step(cmd.data(), cmd.size());   // the end
  return D.stats;
}

}  // namespace spread
}  // namespace unbind

#endif  // SUBHALO_UNBIND_SPREAD_H_
