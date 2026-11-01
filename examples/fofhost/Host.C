// Stand-in host for the FoF3 plug-in (design/plugin-interface.md, stage B).
// It owns its particles the way a simulation would: its own chare array,
// each element holding a contiguous slice of the input file (file order,
// not space), placed on a scrambled PE. It includes only the plug-in API,
// no paratreet headers, stages its particles, runs FoF through the plug-in
// and prints a label digest that must equal FoF3's FOF3STAT label_digest
// for the same file and linking length. -r repeats the whole cycle to
// check that consecutive calls start clean.
//
//   fofhost -f <tipsy> [-b factor | -L length] [-P period] [-l leaf]
//           [-k pieces per PE] [-r repeats]

#include "fofhost.decl.h"
#include "FoFPluginAPI.h"
#include "TipsyFile.h"

#include <unistd.h>
#include <string>
#include <vector>

/* readonly */ int peanoKey;
/* readonly */ CProxy_HostPiece hostPieces;

class HostMain : public CBase_HostMain {
  fofplugin::Params params_;
  int repeats_ = 1;

 public:
  HostMain(CkArgMsg* m) {
    std::string file;
    int pieces_per_pe = 2;
    int c;
    while ((c = getopt(m->argc, m->argv, "f:b:L:P:l:k:r:")) != -1) {
      switch (c) {
        case 'f': file = optarg; break;
        case 'b': params_.b_factor = atof(optarg); break;
        case 'L': params_.linking_length = atof(optarg); break;
        case 'P': params_.period = atof(optarg); break;
        case 'l': params_.leaf_size = atoi(optarg); break;
        case 'k': pieces_per_pe = atoi(optarg); break;
        case 'r': repeats_ = atoi(optarg); break;
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
    CkPrintf("FOFHOST file %s n %ld pieces %d leaf %d b_factor %g L %g "
             "period %g repeats %d\n", file.c_str(), n_total, n_pieces,
             params_.leaf_size, params_.b_factor, params_.linking_length,
             params_.period, repeats_);

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
    for (int rep = 0; rep < repeats_; rep++) {
      double t0 = CkWallTimer();
      hostPieces.stage(CkCallbackResumeThread());
      double t1 = CkWallTimer();
      fofplugin::run(params_, CkCallbackResumeThread());
      double t2 = CkWallTimer();
      CkReductionMsg* msg = nullptr;
      hostPieces.digest(CkCallbackResumeThread((void*&)msg));
      const auto* d = (const unsigned long long*)msg->getData();
      CkPrintf("FOFHOST label_digest: n %llu digest %016llx unlabeled %llu "
               "(rep %d)\n", d[0], d[1], d[2], rep);
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

 public:
  HostPiece(std::string file, long n_total, int n_pieces) {
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
    first_ = fofplugin::stage(parts_.data(), parts_.size());
    contribute(cb);
  }

  // Count, wrapping digest sum, and particles left unlabeled.
  void digest(const CkCallback& cb) {
    size_t n = 0;
    const long* labels = fofplugin::labels(n);
    unsigned long long v[3] = {0, 0, 0};
    for (size_t i = 0; i < parts_.size(); i++) {
      const long label = labels[first_ + i];
      v[0]++;
      v[1] += fofplugin::labelDigestTerm((long)parts_[i].id, label);
      if (label < 0) v[2]++;
    }
    contribute(3 * sizeof(unsigned long long), v, CkReduction::sum_ulong_long, cb);
  }

  void clearStaged(const CkCallback& cb) {
    fofplugin::clear();
    contribute(cb);
  }
};

#include "fofhost.def.h"
