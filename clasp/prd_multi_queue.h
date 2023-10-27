
#include <clasp/mt/mutex.h>
#include <clasp/clause.h>
#include <vector>

// DPS Style nogoods exchange

namespace Clasp  { namespace mt { 

// A set of clauses acquired with a certain period of a thread
class PrdClauses {
private:
    typedef condition_variable ConditionVar;
    typedef std::vector<SharedLiterals*> ClauseVec;
    
    int thn;                       // thread number
    uint64    prd;             // period number
    uint64    prd_len;         // period length for adaptive strategy
    bool      terminate_flag;
    size_t    num_literals;    // the number of literals
    ClauseVec bt_clauses;      // a set of binary-ternay
    ClauseVec lg_clauses;      // a set of long nogoods

    // log
    std::vector<uint64> clause_counter;

    int num_exported_threads;           // the number of threads to which these clauses are exported.
    mutex lock_num_exported_threads;    // mutex on the variable "num_exported_threads"

    bool            completed;          // whether the addition of clauses from the thread is finished
    mutex           lock_completed;     // mutex on the variable "completed"
    ConditionVar    is_completed;       // condition variable that says that this set of clauses is completed

public:
    enum Type {
        UNIT = 1,
        BINARY = 2,
        TERNARY = 3,
        LONG = 4
    };
    PrdClauses(int thread_id, uint64 period);

    bool addClause(SharedLiterals* c);

    // When the period of the thread is finished (it means that the addition of clauses is completed),
    // then this method is called by the thread. This method notifies waiting threads to be completed.
    void completeAddition(uint64 prd_len);

    // Wait the addition of clauses to be completed.
    void waitAdditionCompleted(void);
    bool isAdditionCompleted(void);

    // Methods for exportation
    int size(void) const { return bt_clauses.size() + lg_clauses.size(); };
    SharedLiterals* operator [] (int index) const {
        int idx = index - bt_clauses.size();
        if (idx < 0) return bt_clauses[index];
        return lg_clauses[idx]; 
    }

    // When exporting to the specified thread is finished, then this method is called.
    void completeExportation(int thread_id);
    int getNumExportedThreads(void);

    // When solving is finished, then this method is called.
    void terminate();

    // Misc
    uint64  period(void)          const { return prd; }
    uint64  getPrdLenCand(void)   const { return prd_len; }
    uint32  getNumClauses(void)   const { return bt_clauses.size() + lg_clauses.size(); }
    uint32  getNumLiterals(void)  const { return num_literals; }

    // Log
    uint64  getClauseCounter(Type type) const { return clause_counter[type-1]; }
};



// A set of clauses acquired at a certain thread
class PrdClausesQueue {
private:
    int thn;                                // thread number
    int num_threads;                        // the number of threads
    uint64 margin;                          // the margin for import clauses from other threads
    std::vector<uint64>     next_period;  // the next period for exporting to the specified thread
    std::vector<int>          next_index;
    std::vector<PrdClauses *> queue;        // a list of sets of clauses.
    mutex                     rwlock;       // read/write lock of this object.

    double                    last_sync_time; // the time when the last synchronization is finished
    
public:
    PrdClausesQueue(int thread_id, int nb_threads, uint64 margin);
    ~PrdClausesQueue();

    // When the current period of the thread is finished, then this method is called by the thread.
    // This method notifies waiting threads to be completed.
    double completeAddition(uint64 prd_len);

    // When exporting to the specified thread is finished, then this method is called.
    void completeExportation(int thread_id, PrdClauses& prdClauses);

    void waitAdditionCompleted(int prd);

    SharedLiterals* pop(int thread_id, uint64 period);

    // Get a set of clauses which are generated at the specified period.
    PrdClauses* get(int thread, uint64 period);

    // Get a set of own clauses which are generated at the specified period.
    PrdClauses* get(uint64 period);

    // Return the last set of clauses
    PrdClauses& last() { assert(queue.size() > 0); return *queue[queue.size() - 1]; }

    // When solving is finished, then this method is called.
    void terminate();
    
};

// A set of clauses acquired by threads
class PrdClausesQueueMgr {
private:
    struct EstimatedResult {
        EstimatedResult(int n_th=0) : 
            num_threads(n_th), 
            period(UINT64_MAX), 
            thread_id(UINT32_MAX){
            confirmed.resize(1, false);
            };
        
        int num_threads;

        mutex  lock_commit;
        uint64 period;
        uint32 thread_id;

        typedef PodVector<bool>::type BoolVec;
        BoolVec            confirmed;
        uint32             waiting = 0;
        mutex              mAdopt;
        condition_variable cAdopt;
        void enterWait(uint64 prd) {
            if (prd < 0) return;
            // waiting を mutex で保護しないと競合が発生する
            unique_lock<mutex> lock(mAdopt);
            if (waiting < num_threads - 1) 
                waitSync(lock, prd);
            else {
                confirmed[prd] = true; // 一度でも終了同期が終わってしまったらその後に発見される解は全て終了同期しなくなる
                cAdopt.notify_all();
            }
        }

        void waitSync(unique_lock<mutex>& lock, uint64 prd) {
            assert(prd >= 0);
            for (; confirmed.size() < prd + 1;) 
                confirmed.push_back(false);
            for (; !confirmed[prd];) {
                waiting++;
                cAdopt.wait(lock);
                waiting--;
            }
        }

        void terminate() {
            unique_lock<mutex> lock(mAdopt);
            for (size_t i=0; i < confirmed.size(); i++)
                confirmed[i] = true;
            cAdopt.notify_all();
        }
    };
    EstimatedResult estR;
    int num_threads;
    uint64 margin;

    std::vector<PrdClausesQueue *> queues;

public:
    // TODO publicなenumではなく, policyクラスを作ってポインタをPrdClausesQueueに渡す形にする
    enum {
        MARGIN = 20
    };
    PrdClausesQueueMgr(int num_threads=0);
    ~PrdClausesQueueMgr();

    void setNumThreads(int num_threads);
    PrdClausesQueue& get(uint32 thread_id) const;

    bool prepareNextPeriod(uint32 thread_id, uint64 period);

    bool commitResult(uint32 thread_id, uint64 period);

    void terminate();

    void reset(int num_threads, uint64 margin);
};


} } // namespace Clasp::mt::Detail
