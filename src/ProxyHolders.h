#ifndef PARATREET_PROXYHOLDERS_H_
#define PARATREET_PROXYHOLDERS_H_

#include "paratreet.decl.h"

template <typename Data>
class CProxy_TreePiece;

template <typename Data>
struct TPHolder {
  CProxy_TreePiece<Data> proxy;
  TPHolder(CProxy_TreePiece<Data> proxy_) : proxy(proxy_) {}
  TPHolder() {}
  void pup(PUP::er& p) {
    p | proxy;
  }
};

template <typename Data>
class CProxy_Partition;

template <typename Data>
struct PPHolder {
  CProxy_Partition<Data> proxy;
  PPHolder(CProxy_Partition<Data> proxy_) : proxy(proxy_) {}
  PPHolder() {}
  void pup(PUP::er& p) {
    p | proxy;
  }
};

template <typename Data>
class CProxy_TreeCanopy;

template <typename Data>
struct TCHolder {
  CProxy_TreeCanopy<Data> proxy;
  TCHolder(CProxy_TreeCanopy<Data> proxy_) : proxy(proxy_) {}
  TCHolder() {}
  void pup(PUP::er& p) {
    p | proxy;
  }
};

template <typename Data>
class CProxy_Driver;

template <typename Data>
struct DPHolder {
  CProxy_Driver<Data> proxy;
  DPHolder(CProxy_Driver<Data> proxy_) : proxy(proxy_) {}
  DPHolder() {}
  void pup(PUP::er& p) {
    p | proxy;
  }
};

template <typename Data>
class CProxy_CacheManager;


template <typename Data>
struct ProxyPack {
  CProxy_Driver<Data> driver;
  CProxy_TreePiece<Data> treepiece;
  CProxy_Partition<Data> partition;
  CProxy_CacheManager<Data> cache;
  ProxyPack(CProxy_Driver<Data> d, CProxy_TreePiece<Data> s, CProxy_Partition<Data> p, CProxy_CacheManager<Data> c)
    : driver(d), treepiece(s), partition(p), cache(c) {}
  ProxyPack() {}
  void pup(PUP::er& p) {
    p | driver;
    p | treepiece;
    p | partition;
    p | cache;
  }
};


#endif // PARATREET_PROXYHOLDERS_H_
