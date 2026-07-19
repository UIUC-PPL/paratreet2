#ifndef UNION_FIND_LIB
#define UNION_FIND_LIB

// Distributed union-find library, imported from UIUC-PPL/unionfind (sibling
// checkout ~/software/clusterFinding/unionfind) on 2026-07-19 for FoF step 4
// (design/step4.md). Changes relative to the sibling:
//  - unionfind-bugfixes.patch applied (set_component recursion -> explicit
//    work queue; zero-initializing default constructor).
//  - AGGREGATION (htram), PROFILING and ANCHOR_ALGO paths stripped
//    (unifdef + removal of the unconditional htram surface): this import is
//    plain point-to-point sends only.
//  - local_union's hard-coded vertexID encoding (chare = vid >> 32,
//    idx = vid & 0xFFFFFFFF) replaced with calls to the registered
//    getLocationFromID, so the app's encoding is authoritative everywhere.
//  - initialize_vertices resets per-run state (parentCache, boss counts) so
//    a library instance can be reused across FoF iterations.
//  - unionFindInitOnePerNode(): creates the library with ONE chare per
//    process (placed on the first PE of each node via UFNodeMap) instead of
//    binding to a client array, and acknowledges wiring completion through a
//    callback (passLibGroupID gained a CkCallback, contributed by every
//    element) so callers can order initialization against later broadcasts.
//  - union_requests(): batched entry wrapping union_request, one message per
//    submitting PE instead of one per edge.

#include "unionFindLib.decl.h"
#include <unordered_map>

/*readonly*/ extern CProxy_UnionFindLib _UfLibProxy;

struct unionFindVertex {
    uint64_t vertexID;
    int64_t parent;
    int64_t process_tip; //used during path compression to store the last node local vertex on the path to the root
    long int componentNumber = -1;
    int64_t componentSize = -1;
    int64_t size = 1;
    std::vector<uint64_t> need_boss_requests; //request queue for processing need_boss requests
    long int findOrAnchorCount = 0;

    void pup(PUP::er &p) {
        p|vertexID;
        p|parent;
        p|process_tip;
        p|componentNumber;
        p|componentSize;
        p|size;
        p|need_boss_requests;
    }
};

struct componentCountMap {
    long int compNum;
    int count;

    void pup(PUP::er &p) {
        p|compNum;
        p|count;
    }
};

typedef struct componentCacheEntry {
    long int compNum;
    int64_t compSize = -1;
    std::vector<long int> requestors;

    void pup(PUP::er &p) {
        p|compNum;
        p|compSize;
        p|requestors;
    }
} cacheEntry;


/* global variables */
/*readonly*/ extern CkGroupID libGroupID;
// declaration for custom reduction
extern CkReduction::reducerType mergeCountMapsReductionType;

// class definition for library chares
class UnionFindLib : public CBase_UnionFindLib {
    unionFindVertex *myVertices;
    int numMyVertices;
    int pathCompressionThreshold = 5;
    int componentPruneThreshold;
    std::pair<int, int> (*getLocationFromID)(uint64_t vid);
    int myLocalNumBosses;
    int totalNumBosses;
    CkCallback postComponentLabelingCb;
    CkCallback postPruningCb;
    std::unordered_map<long int, cacheEntry> parentCache; //maps vertex numbers to component numbers

    public:
    UnionFindLib() : myVertices(nullptr), numMyVertices(0) {}
    UnionFindLib(CkMigrateMessage *m) { }
    void pup(PUP::er &p) {
        CBase_UnionFindLib::pup(p);
    }
    void passLibGroupID(CkGroupID lgid, CProxy_Prefix pla, CkCallback ready);
    static CProxy_UnionFindLib unionFindInit(CkArrayID clientArray, int n);
    // One library chare per PROCESS, element i on the first PE of node i
    // (UFNodeMap). `ready` fires (array reduction) once every element has
    // been constructed and wired (libGroupID/prefix proxies set), so the
    // caller may safely follow with broadcasts that use them.
    static CProxy_UnionFindLib unionFindInitOnePerNode(const CkCallback& ready);
    void registerGetLocationFromID(std::pair<int, int> (*gloc)(uint64_t vid));
    void register_phase_one_cb(CkCallback cb);
    void initialize_vertices(unionFindVertex *appVertices, int numVertices);
    uint64_t get_parent(uint64_t vertexID);
    void boss_send(int chare_index, findBossData data); //sends during boss finding
    void union_request(uint64_t vid1, uint64_t vid2);
    void union_requests(const std::vector<UFEdge>& edges); // batched entry
    void find_boss1(int arrIdx, uint64_t partnerID, uint64_t senderID);
    void find_boss2(int arrIdx, uint64_t boss1ID, uint64_t senderID);
    void local_union(uint64_t vid1, uint64_t vid2);
    void local_path_compression(unionFindVertex *src, uint64_t compressedParent);
    bool check_same_chares(uint64_t v1, uint64_t v2);
    void short_circuit_parent(shortCircuitData scd);
    void compress_path(int arrIdx, uint64_t compressedParent);
    unionFindVertex *return_vertices();

    // functions and data structures for finding connected components

    public:
    void find_components(CkCallback cb);
    void boss_count_prefix_done(int totalCount);
    void start_component_labeling();
    void insertDataNeedBoss(const needBossData & data);
    void insertDataFindBoss(const findBossData & data);
    void need_boss(int arrIdx, uint64_t fromID);
    void add_size(int arrIdx, int64_t delta);
    void set_component(int arrIdx, long int compNum, int64_t compSize);
    void prune_components(int threshold, CkCallback appReturnCb);
    void perform_pruning();
    void report_surviving_components(long *totalData, int numElems);
    int get_total_num_bosses() {
        return totalNumBosses;
    }
};

// library group chare class declarations
class UnionFindLibGroup : public CBase_UnionFindLibGroup {
    bool map_built;
    int* component_count_array;
    int thisPeMessages; //for profiling
    public:
    UnionFindLibGroup() {
        map_built = false;
        thisPeMessages = 0;
    }
    void build_component_count_array(int* totalCounts, int numComponents);
    int get_component_count(long int component_id);
    void increase_message_count();
    void contribute_count();
    void done_profiling(int);
};

// Array map placing element i on the first PE of (SMP) node i: the
// one-chare-per-process layout used by unionFindInitOnePerNode.
class UFNodeMap : public CBase_UFNodeMap {
    public:
    UFNodeMap() {}
    int procNum(int, const CkArrayIndex &idx) {
        return CkNodeFirst(idx.data()[0]);
    }
};

#endif
