
#include <clasp/prd_multi_queue.h>
#include <clasp/clause.h>
#include <clasp/util/timer.h>

namespace Clasp { namespace mt { 

/////////////////////////////////////////////////////////////////////////////////////////
// PrdClauses
/////////////////////////////////////////////////////////////////////////////////////////
PrdClauses::PrdClauses(int thread_id, uint64_t period) :
        thn(thread_id),
        prd(period),
        prd_len(0),
        num_literals(0),
        num_exported_threads(0),
        completed(false),
        terminate_flag(false)
{
    clause_counter.resize(4, 0);
}

bool PrdClauses::addClause(SharedLiterals* c) {
    assert(!completed);
    
    // LOG
    uint32 stats_idx = c->size()-1;
    if (stats_idx >= 3) stats_idx = 3;
    clause_counter[stats_idx]++;

    if (c->size() <= 3) {
        bt_clauses.push_back(c);
        return true;
    }
    lg_clauses.push_back(c);
    return true;
}

// When the period of the thread is finished (it means that the addition of clauses is completed),
// then this method is called by the thread. This method notifies waiting threads to be completed.
void PrdClauses::completeAddition(uint64_t _prd_len) {
    assert(completed == false);
    lock_guard<mutex> lock(lock_completed);
    completed = true;
    prd_len = _prd_len;
    // Send signals to threads which are waiting to be completed.
    is_completed.notify_all();
}

// Wait the addition of clauses to be completed.
void PrdClauses::waitAdditionCompleted(void) {
    for (unique_lock<mutex> lock(lock_completed); !(completed || terminate_flag); )
        is_completed.wait(lock);
}

bool PrdClauses::isAdditionCompleted(void) {
    lock_guard<mutex> lock(lock_completed);
    return completed;
}

// When exporting to the specified thread is finished, then this method is called.
void PrdClauses::completeExportation(int thread_id) {
    unique_lock<mutex> lock(lock_num_exported_threads);
    num_exported_threads++;
}

int PrdClauses::getNumExportedThreads(void) {
    unique_lock<mutex> lock(lock_num_exported_threads);
    int num = num_exported_threads;
    return num;
}

void PrdClauses::terminate() {
    terminate_flag = true;
    is_completed.notify_all();
}

/////////////////////////////////////////////////////////////////////////////////////////
// PrdClausesQueue
/////////////////////////////////////////////////////////////////////////////////////////
PrdClausesQueue::PrdClausesQueue(int _thn, int _num_threads, uint64 _margin) :
    thn(_thn)
,   num_threads(_num_threads)
,   next_period(std::vector<uint64_t>(_num_threads, 0))
,   next_index(std::vector<int>(_num_threads, 0))
,   last_sync_time(ThreadTime::getTime())
,   margin(_margin)
{
    queue.clear();
    // Add an empty set of clauses to which clauses acquired at period 0 are stored.
    PrdClauses *pcs = new PrdClauses(thn, 0);
    if (!pcs) throw std::runtime_error("could not allocate memory for PrdClauses");
    queue.push_back(pcs);
}

PrdClausesQueue::~PrdClausesQueue()
{
    queue.clear();
}

// When the current period of the thread is finished, then this method is called by the thread.
// This method notifies waiting threads to be completed.
double PrdClausesQueue::completeAddition(uint64_t prd_len)
{
    assert(queue.size() > 0);
    PrdClauses& last = *queue.back();

    // 前回の同期からの時間を計測する
    double t = ThreadTime::getTime() - last_sync_time;
    last_sync_time = ThreadTime::getTime();

    lock_guard<mutex> lock(rwlock);

    // Add an empty set of clauses to which clauses acquired at the next period are stored.
    PrdClauses *pcs = new PrdClauses(thn, last.period() + 1);
    if (!pcs) throw std::runtime_error("could not allocate memory for PrdClauses");
    queue.push_back(pcs);

    // Complete and notify it to all waiting threads
    last.completeAddition(prd_len);

    // Remove a set of clauses that were sent to other threads
    while (queue.size() > 1) {
        PrdClauses* head = queue.front();
        if (head->getNumExportedThreads() != num_threads)
            break;
        queue.erase(queue.begin());
        // delete head;
    }
    return t;
}

// When exporting to the specified thread is finished, then this method is called.
void PrdClausesQueue::completeExportation(int thn, PrdClauses& prdClauses)
{
    assert(next_period[thn] == prdClauses.period());
    next_period[thn]++;
    next_index[thn] = 0;
    prdClauses.completeExportation(thn);
}

void PrdClausesQueue::waitAdditionCompleted(int prd) {
    // prd までの PrdClauses がすべてAdditionが完成するまで待つ
    int p = prd - queue.front()->period();
    for (int i=0; i <= p; i++) {
        PrdClauses *prdClauses = queue[i];
        prdClauses->waitAdditionCompleted();
    }
}

SharedLiterals* PrdClausesQueue::pop(int thread, uint64_t period) {
    if (period < margin) return NULL;
    
    uint64 prd = period - margin;
    uint64_t p = next_period[thread];
    if (prd < p) return NULL;

    if (queue.size() == 0 || p < queue.front()->period())  {
        return NULL;
    }

    size_t index = p - queue.front()->period();
    assert(0 <= index);
    assert(index < queue.size());
    PrdClauses& prdClauses = *queue[index];

    int clause_index = next_index[thread];
    if (prdClauses.size() <= clause_index) {
        completeExportation(thread, prdClauses);
        return pop(thread, prd);
    }
    SharedLiterals* clause = prdClauses[clause_index];
    next_index[thread]++;
    return clause;
}


// Get a set of clauses which are generated at the specified period.
PrdClauses* PrdClausesQueue::get(int thread, uint64_t period) {
    uint64_t p = next_period[thread];
    if (period < p) return NULL;    // 'period' is already exported to 'thn'

    lock_guard<mutex> lock(rwlock);

    if (queue.size() == 0 || p < queue.front()->period())  {
        return NULL;
    }

    size_t index = p - queue.front()->period();
    assert(0 <= index);
    assert(index < queue.size());
    PrdClauses *prdClauses = queue[index];
    
    assert(prdClauses->period() == p);

    return prdClauses;
}

// Get a set of clauses which are generated at the specified period.
PrdClauses* PrdClausesQueue::get(uint64_t period) {
    assert(queue.front()->period() <= period);
    size_t index = period - queue.front()->period();
    assert(0 <= index);
    assert(index < queue.size());
    PrdClauses *prdClauses = queue[index];
    assert(prdClauses->period() == period);
    return prdClauses;
}

void PrdClausesQueue::terminate() {
    for (size_t i=0; i < queue.size(); i++)
        queue[i]->terminate();
}

/////////////////////////////////////////////////////////////////////////////////////////
// PrdClausesQueueMgr
/////////////////////////////////////////////////////////////////////////////////////////
PrdClausesQueueMgr::PrdClausesQueueMgr(int num_threads) {
    setNumThreads(num_threads);
}

void PrdClausesQueueMgr::setNumThreads(int _num_threads) {
    assert(queues.size() == 0);
    for (int i=0; i < _num_threads; i++) {
        PrdClausesQueue *mgr = new PrdClausesQueue(i, _num_threads, margin);
        if (!mgr) throw std::runtime_error("could not allocate memory for PrdClausesQueue");
        queues.push_back(mgr);
    }
    num_threads = _num_threads;
    estR.num_threads = _num_threads;
}

PrdClausesQueueMgr::~PrdClausesQueueMgr() {
    for (size_t i=0; i < queues.size(); i++)
        delete queues[i];
    queues.clear();
}

PrdClausesQueue& PrdClausesQueueMgr::get(uint32_t thread_id) const {
    assert(0 <= thread_id);
    assert(thread_id < queues.size());
    return *queues[thread_id];
}

bool PrdClausesQueueMgr::prepareNextPeriod(uint32 thread_id, uint64 period) {
    if (period < margin) return true;
    uint64 prd = period - margin;
    assert(prd >= 0);

    // wait for all threads to complete the addition of clauses
    for (uint32_t i=1; i < queues.size(); i++) {      	     // i=0 denote the current thread
		uint32_t target = (thread_id + i) % queues.size();   // target thread number from which clauses are imported
		PrdClausesQueue& pcq = get(target);
		pcq.waitAdditionCompleted(prd);
	}

    // 終了同期をとる
    if (estR.period <= prd)
        estR.enterWait(prd);
    
    return true;
}

bool PrdClausesQueueMgr::commitResult(uint32 thread_id, uint64 period) {
    {
        unique_lock<mutex> lock(estR.lock_commit);
        if (period < estR.period || 
            (period == estR.period && thread_id <= estR.thread_id)
        ) {
            estR.period = period;
            estR.thread_id = thread_id;
        }
    }
    get(thread_id).completeAddition(thread_id);

    estR.enterWait(period);

    // todo cond_terminate
    return estR.thread_id == thread_id;
}

void PrdClausesQueueMgr::terminate() {
    for (size_t i=0; i < queues.size(); i++)
        queues[i]->terminate();
    estR.terminate();
}

void PrdClausesQueueMgr::reset(int num_threads, uint64 _margin) {
    for (size_t i=0; i < queues.size(); i++)
        delete queues[i];
    queues.clear();
    margin = _margin;
    setNumThreads(num_threads);
}

} } // end of namespace Clasp::mt