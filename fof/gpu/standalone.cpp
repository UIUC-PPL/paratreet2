// Standalone gate for the device library: a PLAIN main(), NO Charm
// headers, NO Charm libraries at link — the same discipline
// tests/treecache applies to the passive cache core. If this fails, the
// problem is Kokkos/HIP, not the Charm integration; that separation is
// the whole point of running it first (design/phase1-gpu.md stage 0).
//
//   ./fof-device-test [n_particles]

#include "FoFDevice.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

int main(int argc, char** argv) {
  const size_t n = (argc > 1) ? std::strtoul(argv[1], nullptr, 10) : 1000000;

  fofgpu::Device dev;
  dev.init(-1);  // standalone: let Kokkos pick; the Charm path passes hapiGetDevice()

  fofgpu::DeviceInfo di = dev.info();
  std::printf("backend=%s device_backend=%d visible=%d name=\"%s\" mem=%.1f GB\n",
              di.backend, (int)di.is_device_backend, di.n_visible, di.name,
              di.total_global_mem / 1073741824.0);

  dev.resize(n);
  float* pos = dev.hostPositions();
  long* order = dev.hostOrders();

  // Deterministic pseudo-particle field, cheap to reproduce on the host.
  double h_sum = 0;
  float lo[3] = {std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max()};
  float hi[3] = {-std::numeric_limits<float>::max(),
                 -std::numeric_limits<float>::max(),
                 -std::numeric_limits<float>::max()};
  for (size_t i = 0; i < n; i++) {
    const float x = static_cast<float>(std::sin(0.001 * i) * 100.0);
    const float y = static_cast<float>(std::cos(0.002 * i) * 50.0);
    const float z = static_cast<float>((i % 1000) * 0.25);
    pos[3 * i] = x;
    pos[3 * i + 1] = y;
    pos[3 * i + 2] = z;
    order[i] = static_cast<long>(i) * 3 + 7;
    h_sum += (double)x + (double)y + (double)z;
    const float v[3] = {x, y, z};
    for (int a = 0; a < 3; a++) {
      if (v[a] < lo[a]) lo[a] = v[a];
      if (v[a] > hi[a]) hi[a] = v[a];
    }
  }

  fofgpu::RoundTripStats st;
  dev.roundTrip(&st);

  std::printf("n=%ld upload=%.3f ms kernel=%.3f ms download=%.3f ms\n", st.n,
              st.t_upload * 1e3, st.t_kernel * 1e3, st.t_download * 1e3);
  std::printf("device bbox = [%g %g %g] .. [%g %g %g]\n", st.box_lo[0],
              st.box_lo[1], st.box_lo[2], st.box_hi[0], st.box_hi[1],
              st.box_hi[2]);

  int failures = 0;

  const long* label = dev.hostLabels();
  for (size_t i = 0; i < n; i++) {
    if (label[i] != order[i]) {
      std::printf("FAIL round trip: label[%zu]=%ld order=%ld\n", i, label[i],
                  order[i]);
      failures++;
      if (failures > 4) break;
    }
  }

  for (int a = 0; a < 3; a++) {
    if (st.box_lo[a] != lo[a] || st.box_hi[a] != hi[a]) {
      std::printf("FAIL bbox axis %d: device [%g,%g] host [%g,%g]\n", a,
                  st.box_lo[a], st.box_hi[a], lo[a], hi[a]);
      failures++;
    }
  }

  // The checksum is a double sum over a nondeterministic reduction tree,
  // so it is compared with a tolerance scaled to the magnitude summed —
  // unlike the bbox and the labels, which must match exactly.
  const double tol = 1e-9 * std::fabs(h_sum) * std::sqrt((double)n);
  if (std::fabs(st.checksum - h_sum) > tol + 1e-6) {
    std::printf("FAIL checksum: device %.9g host %.9g (tol %.3g)\n",
                st.checksum, h_sum, tol);
    failures++;
  }

  dev.finalize();

  if (failures) {
    std::printf("FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("OK\n");
  return 0;
}
