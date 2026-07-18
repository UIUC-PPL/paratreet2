#include "Writer.h"
#include "TipsyFile.h"

Writer::Writer(std::string of, int n_particles)
  : output_file(of), total_particles(n_particles)
{
}

void Writer::receive(std::vector<Particle> ps, Real time, int iter)
{
  // Accumulate received particles
  particles.insert(particles.end(), ps.begin(), ps.end());
  time_ = time;
  iter_ = iter;
}

void Writer::write(CkCallback cb)
{
  // Received expected number of particles, sort the particles
  std::sort(particles.begin(), particles.end(),
            [](const Particle& left, const Particle& right) {
              return left.order < right.order;
            });
  do_write();
  cur_dim = (cur_dim + 1) % 3;
  if (thisIndex != CkNumPes() - 1) thisProxy[thisIndex + 1].write(cb);
  else if (cur_dim == 0) cb.send();
  else thisProxy[0].write(cb);
}

void Writer::do_write()
{
  // Write particle accelerations and other quantities to output file
  FILE *fp;
  FILE *fpDen;
  FILE *fpPres;
  FILE *fpGrp;
  if (thisIndex == 0 && cur_dim == 0) {
    fp = CmiFopen((output_file+".acc").c_str(), "w");
    fprintf(fp, "%d\n", total_particles);
    fpDen = CmiFopen((output_file+".den").c_str(), "w");
    fprintf(fpDen, "%d\n", total_particles);
    fpPres = CmiFopen((output_file+".pres").c_str(), "w");
    fprintf(fpPres, "%d\n", total_particles);
    #ifdef FOF
    fpGrp = CmiFopen((output_file+".grp").c_str(), "w");  // grp = group number for FoF connected component
    fprintf(fpGrp, "%d\n", total_particles);
    #endif // FOF
  } else {
      fp = CmiFopen((output_file+".acc").c_str(), "a");
      fpDen = CmiFopen((output_file+".den").c_str(), "a");
      fpPres = CmiFopen((output_file+".pres").c_str(), "a");
      #ifdef FOF
      fpGrp = CmiFopen((output_file+".grp").c_str(), "a");
      #endif // FOF
  }
  CkAssert(fp);
  CkAssert(fpDen);
  CkAssert(fpPres);
  CkAssert(fpGrp);

  for (const auto& particle : particles) {
    Real outval;
    //if (cur_dim == 0) outval = particle.acceleration.x;
    //else if (cur_dim == 1) outval = particle.acceleration.y;
    //else if (cur_dim == 2) outval = particle.acceleration.z;
    fprintf(fp, "%.14g\n", outval);
    if (cur_dim == 0) {
        fprintf(fpDen, "%.14g\n", particle.density);
        const double gammam1 = 5./3. - 1.0;
        //fprintf(fpPres, "%.14g\n", gammam1*particle.u*particle.density);
        #ifdef FOF
        fprintf(fpGrp, "%ld\n", particle.group_number);
        #endif // FOF
    }
  }

  int result = CmiFclose(fp);
  CkAssert(result == 0);
  result = CmiFclose(fpDen);
  CkAssert(result == 0);
  result = CmiFclose(fpPres);
  CkAssert(result == 0);
  #ifdef FOF
  result = CmiFclose(fpGrp);
  CkAssert(result == 0);
  #endif // FOF
}

TipsyWriter::TipsyWriter(std::string of, BoundingBox b)
  : output_file(of), box(b)
{
}

void TipsyWriter::receive(std::vector<Particle> ps, Real time, int iter)
{
  // Accumulate received particles
  particles.insert(particles.end(), ps.begin(), ps.end());
  time_ = time;
  iter_ = iter;
}

void TipsyWriter::write(int prefix_count, CkCallback cb) {
  // Received expected number of particles, sort the particles
  std::sort(particles.begin(), particles.end(),
            [](const Particle& left, const Particle& right) {
              return left.order < right.order;
            });

  int new_prefix_count = prefix_count + particles.size();
  do_write(prefix_count);
  if (thisIndex != CkNumPes() - 1) thisProxy[thisIndex + 1].write(new_prefix_count, cb);
  else cb.send();
}

void TipsyWriter::do_write(int prefix_count)
{
  Tipsy::header tipsyHeader;

  tipsyHeader.time = time_;
  tipsyHeader.nbodies = box.n_particles;
  tipsyHeader.nsph = box.n_sph;
  tipsyHeader.ndark = box.n_dark;
  tipsyHeader.nstar = box.n_star;

  bool use_double = sizeof(Real) == 8;

  auto output_filename = output_file + "." + std::to_string(iter_) + ".tipsy";

  if (thisIndex == 0) CmiFopen(output_filename.c_str(), "w");

  Tipsy::TipsyWriter w(output_filename, tipsyHeader, false, use_double, use_double);

  if(thisIndex == 0) w.writeHeader();

  if(!w.seekParticleNum(prefix_count)) {
    CkPrintf("seeking %d particles for total %d gas %d dark %d star %d\n",
      prefix_count, box.n_particles, box.n_sph, box.n_dark, box.n_star);
    CkAbort("bad seek");
  }

  for (const auto& p : particles) {
    if (p.isGas()) {
      Tipsy::gas_particle_t<Real, Real> gp;
      gp.mass = p.mass;
      gp.pos = p.position;
      gp.vel = p.velocity; // dvFac = 1
      if(!w.putNextGasParticle_t(gp)) {
        CkError("[%d] Write gas failed, errno %d: %s\n", CkMyPe(), errno, strerror(errno));
        CkAbort("Bad Write");
      }
    }
    else if (p.isDark()) {
      Tipsy::dark_particle_t<Real, Real> dp;
      dp.mass = p.mass;
      dp.pos = p.position;
      dp.vel = p.velocity; // dvFac = 1
      if(!w.putNextDarkParticle_t(dp)) {
        CkError("[%d] Write dark failed, errno %d: %s\n", CkMyPe(), errno, strerror(errno));
        CkAbort("Bad Write");
      }
    }
    else if (p.isStar()) {
      Tipsy::star_particle_t<Real, Real> sp;
      sp.mass = p.mass;
      sp.pos = p.position;
      sp.vel = p.velocity; // dvFac = 1
      if(!w.putNextStarParticle_t(sp)) {
        CkError("[%d] Write star failed, errno %d: %s\n", CkMyPe(), errno, strerror(errno));
        CkAbort("Bad Write");
      }
    }

  }
}



