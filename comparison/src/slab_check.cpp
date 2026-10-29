#include <cmath>
#include "cosmoio.hpp"
#include <cstdio>
int main(){
  cosmoio::Snapshot a,b; std::string e;
  cosmoio::open_snapshot("/lustre/orion/csc710/scratch/rrao/nctest/100k.nchilada",a,e);
  cosmoio::open_snapshot("/lustre/orion/csc710/scratch/rrao/nctest/100k.tipsy",b,e);
  int64_t N=a.n_total;
  for(int P : {2,4,8,32}){
    int64_t bad=0;
    for(int r=0;r<P;++r){
      int64_t s=(int64_t)((__int128)N*r/P), t=(int64_t)((__int128)N*(r+1)/P), n=t-s;
      std::vector<float> x(3*n), y(3*n);
      if(!cosmoio::read_pos(a,s,n,x.data(),e)){printf("A err %s\n",e.c_str());return 1;}
      if(!cosmoio::read_pos(b,s,n,y.data(),e)){printf("B err %s\n",e.c_str());return 1;}
      for(int64_t i=0;i<3*n;++i) if(x[i]!=y[i]) ++bad;
    }
    printf("P=%2d  differing coords across all slabs: %lld\n",P,(long long)bad);
  }
  // And: does a slab read agree with the same window of a full read?
  std::vector<float> full(3*N); cosmoio::read_pos(a,0,N,full.data(),e);
  int64_t bad2=0;
  for(int r=0;r<8;++r){
    int64_t s=(int64_t)((__int128)N*r/8), t=(int64_t)((__int128)N*(r+1)/8), n=t-s;
    std::vector<float> x(3*n); cosmoio::read_pos(a,s,n,x.data(),e);
    for(int64_t i=0;i<3*n;++i) if(x[i]!=full[3*s+i]) ++bad2;
  }
  printf("nchilada slab-vs-full mismatches: %lld\n",(long long)bad2);
  std::vector<float> fullb(3*N); cosmoio::read_pos(b,0,N,fullb.data(),e);
  int64_t bad3=0;
  for(int r=0;r<8;++r){
    int64_t s=(int64_t)((__int128)N*r/8), t=(int64_t)((__int128)N*(r+1)/8), n=t-s;
    std::vector<float> x(3*n); cosmoio::read_pos(b,s,n,x.data(),e);
    for(int64_t i=0;i<3*n;++i) if(x[i]!=fullb[3*s+i]) ++bad3;
  }
  printf("tipsy    slab-vs-full mismatches: %lld\n",(long long)bad3);
  return 0;
}
