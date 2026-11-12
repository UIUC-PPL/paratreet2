#!/usr/bin/env python3
"""The particle each group's first frame starts from in an HBT-HERONS run,
as "group id, particle id" lines for `unbind --first-origin` and
`unbindspec --first-origin`.

  first_origins.py <snapshot.hdf5> <HBT-HERONS log> <out.txt>

The log must come from a build with subhalo/hbt_firstparticle.patch, which
prints one line "FIRSTPARTICLE <group size> <particle id>" per FoF group.
The particle matters only for a group at least half the box wide
(design/subhalo-checks.md, "The first frame of a wide group depends on its
first particle"). It depends on the run (number of MPI ranks, C++ library),
so a reference file and its origins must come from the same run.

mpirun can print two ranks' lines into each other. Only a complete
"FIRSTPARTICLE size id" that ends a line is used, and only if that
particle's group has that size; the groups left without an origin are
counted. numpy and HDF5's h5dump (H5DUMP in the environment, default
`h5dump`).
"""
import os
import re
import subprocess
import sys
import tempfile

import numpy as np

H5DUMP = os.environ.get("H5DUMP", "h5dump")
snap, log, out = sys.argv[1:4]
with tempfile.NamedTemporaryFile(suffix=".raw", delete=False) as f:
    tmp = f.name
subprocess.run([H5DUMP, "-d", "/PartType1/FOFGroupIDs", "-b", "LE", "-o", tmp, snap],
               check=True, stdout=subprocess.DEVNULL)
fof = np.fromfile(tmp, dtype="<i8")
os.unlink(tmp)
attr = subprocess.run([H5DUMP, "-a", "/Parameters/FOF:group_id_default", snap],
                      check=True, stdout=subprocess.PIPE).stdout.decode()
null = int(float(re.search(r"\(0\):\s*\"?([-+0-9.eE]+)", attr).group(1)))
size = np.bincount(fof[fof >= 0])
if 0 <= null < len(size):
    size[null] = 0            # particles in no group

n = torn = 0
with open(out, "w") as o:
    o.write("# group id, particle id: the first particle of HBT-HERONS's list for the group\n")
    for line in open(log):
        if "FIRSTPARTICLE" not in line:
            continue
        m = re.search(r"FIRSTPARTICLE (\d+) (\d+)\n$", line)
        g = int(fof[int(m.group(2)) - 1]) if m and 0 < int(m.group(2)) <= len(fof) else -1
        if g < 0 or size[g] != int(m.group(1)):
            torn += 1
            continue
        o.write(f"{g} {m.group(2)}\n")
        n += 1
        size[g] = -size[g]    # seen
left = np.flatnonzero(size >= 20)
print(f"first_origins: {n} groups written to {out}; {torn} torn lines skipped; "
      f"{len(left)} groups of 20 or more without an origin, the largest of "
      f"{int(size[left].max()) if len(left) else 0} particles")
