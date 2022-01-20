/* Partitioned Earliest Deadline First (PEDF) non-preemptive scheduler for the
threaded runtime of the C target of Lingua Franca. */

/*************
Copyright (c) 2022, The University of Texas at Dallas.
Copyright (c) 2022, The University of California at Berkeley.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
***************/

/** 
 * Partitioned Earliest Deadline First (GEDF) non-preemptive for the threaded
 * runtime of the C target of Lingua Franca.
 *  
 * @author{Soroush Bateni <soroush@utdallas.edu>}
 * @author{Edward A. Lee <eal@berkeley.edu>}
 * @author{Marten Lohstroh <marten@berkeley.edu>}
 */

#ifndef NUMBER_OF_WORKERS
#define NUMBER_OF_WORKERS 1
#endif // NUMBER_OF_WORKERS

#include "scheduler.h"
#include "../platform.h"
#include "../utils/pqueue_support.h"
#include "../utils/semaphore.h"
#include "../utils/vector.h"
#include "sync_tag_advance.c"
#include <assert.h>

/////////////////// External Variables /////////////////////////
extern lf_mutex_t mutex;


/////////////////// Scheduler Variables and Structs /////////////////////////
/**
 * @brief Queue of triggered reactions at the current tag.
 * 
 */
pqueue_t* reaction_q;

/**
 * @brief Vector used to keep reactions temporarily.
 * 
 */
vector_t transfer_q;

/**
 * @brief Queue of currently executing reactions.
 * 
 * Sorted by index (precedence sort)
 */
pqueue_t* executing_q;

/**
 * @brief Information about one worker thread.
 * 
 * Only reading and writing the 'is_idle' field strictly requires acquiring the
 * 'mutex' in this struct.
 */
typedef struct {
    lf_mutex_t mutex;           // Used by the scheduler to access is_idle
    
    lf_cond_t cond;             // Used by the scheduler to inform a
                                // worker thread that there is more work to do.
    
    pqueue_t* ready_reactions;  // Reactions that are ready to be executed by 
                                // the worker thread. The worker thread does not
                                // need to acquire any mutex lock to read this
                                // and the scheduler does not need to acquire
                                // any mutex lock to write to this as long as
                                // the worker thread is idle.
    
    vector_t output_reactions;  // Reactions produced by the worker after 
                                // executing a reaction. The worker thread does
                                // not need to acquire any mutex lock to read
                                // this and the scheduler does not need to
                                // acquire any mutex lock to write to this as
                                // long as the worker thread is idle.
    
    vector_t done_reactions;    // Reactions that are ran to completion by the 
                                // worker thread. The worker thread does not
                                // need to acquire any mutex lock to read this
                                // and the scheduler does not need to acquire
                                // any mutex lock to write to this as long as
                                // the worker thread is idle.
    
    bool should_stop;           // Indicate to the worker thread that it should exit.
    
    size_t is_idle;             // Indicate to the scheduler that the worker thread 
                                // is idle (0 = busy, > 0 idle). This is the
                                // only attribute of this struct that requires
                                // the mutex lock to be held, both while reading
                                // and writing to it to avoid race condition.
} _lf_sched_thread_info_t;

/**
 * @brief Information about worker threads. @see _lf_sched_thread_info_t.
 * 
 */
_lf_sched_thread_info_t* _lf_sched_threads_info;

/**
 * @brief Number of workers that this scheduler is managing.
 * 
 */
size_t _lf_sched_number_of_workers = 1;

/**
 * @brief Indicate that a thread is already performing scheduling.
 * 
 */
volatile bool _lf_sched_scheduling_in_progress = false;;

/**
 * @brief Index used by the scheduler to balance the distribution of reactions
 * to worker threads.
 * 
 * The maximum of _lf_sched_balancing_index and reaction->worker_affinity is chosen by
 * the scheduler as the starting point in the quest to find the next available
 * worker to assign a reaction to. During the work distribution phase, this
 * index is updated to make sure the scheduler does not assign work to the same
 * worker thread two times in a row. After the work distribution phase, this
 * index is reset to 0.
 * 
 */
int _lf_sched_balancing_index = 0;

///////////////////// Scheduler Runtime API (private) /////////////////////////
/**
 * @brief Ask the scheduler if it is time to stop (and exit).
 * 
 * @param worker_number The worker number of the worker thread asking if it
 * should stop.
 * @return true If the worker thread should stop executing reactions and exit.
 * @return false 
 */
static inline bool _lf_sched_should_stop(size_t worker_number) {
    return _lf_sched_threads_info[worker_number].should_stop;
}

/**
 * @brief Return true if the worker thread 'worker_number' is idle. False otherwise.
 * 
 */
static inline bool _lf_sched_is_worker_idle(size_t worker_number) {
    return (_lf_sched_threads_info[worker_number].is_idle == 1);
}

/**
 * @brief Distribute 'ready_reaction' to the best idle thread.
 * 
 * This will start from 'ready_reaction->worker_affinity' and rotates through
 * workers (only once) until it finds an idle thread to distribute the work to.
 * If successful, it will return true. If it cannot find a worker thread to
 * execute this reaction, it will return false.
 *
 * @param ready_reaction A reaction that is ready to execute.
 * @return true Found a worker thread to execute 'ready_reaction'.
 * @return false Could not find a worker thread to execute 'ready_reaction'.
 */
static inline bool _lf_sched_distribute_ready_reaction(reaction_t* ready_reaction) {
    DEBUG_PRINT("Scheduler: Trying to distribute reaction %s.", ready_reaction->name);
    bool target_thread_found = false;
    // Start with the preferred worker for the ready reaction or the balancing
    // index, whichever is larger.
    size_t worker_id = MAX(ready_reaction->worker_affinity, _lf_sched_balancing_index);
    // Rotate through all the workers once.
    for(size_t i=0; i<_lf_sched_number_of_workers; i++) {
        // Go over all the workers to see if anyone is idle.
        if (_lf_sched_is_worker_idle(worker_id)) {
            // The worker is idle.
            DEBUG_PRINT(
                "Scheduler: Assigning reaction %s to worker %d.",
                ready_reaction->name,
                worker_id);
            // Add the ready reaction to the ready_reaction queue of the idle worker.
            if (!lf_bool_compare_and_swap(&ready_reaction->status, queued, running)) {
                error_print_and_exit("Unexpected reaction status: %d. Expected %d.", 
                    ready_reaction->status,
                    queued);
            }
            if (pqueue_insert(
                _lf_sched_threads_info[worker_id].ready_reactions,
                ready_reaction
            ) != 0) {
                error_print_and_exit("Could not assign reaction to worker %d.", worker_id);
            }
            target_thread_found = true;
            // Push the reaction on the executing queue in order to prevent any
            // reactions that may depend on it from executing before this reaction is finished.
            pqueue_insert(executing_q, ready_reaction);
        }

        worker_id++;
        
        // Rotate through workers in a circular fashion.
        if (worker_id == _lf_sched_number_of_workers) {
            worker_id = 0;
        }

        if (target_thread_found) {
            break;
        }
    }

    // Update the balancing index to be the next worker in line
    // FIXME: Ideally, it's better to set this index to the least idle worker
    // number but that is an expensive operation.
    _lf_sched_balancing_index = worker_id;

    return target_thread_found;
        
}

/**
 * Return true if the first reaction has precedence over the second, false otherwise.
 * @param r1 The first reaction.
 * @param r2 The second reaction.
 */
bool _lf_has_precedence_over(reaction_t* r1, reaction_t* r2) {
    if (LEVEL(r1->index) < LEVEL(r2->index)
            && OVERLAPPING(r1->chain_id, r2->chain_id)) {
        return true;
    }
    return false;
}

/**
 * If the reaction is blocked by a currently executing
 * reaction, return true. Otherwise, return false.
 * A reaction blocks the specified reaction if it has a
 * level less than that of the specified reaction and it also has
 * an overlapping chain ID, meaning that it is (possibly) upstream
 * of the specified reaction.
 * This function assumes the mutex is held because it accesses
 * the executing_q.
 * @param reaction The reaction.
 * @return true if this reaction is blocked, false otherwise.
 */
bool _lf_is_blocked_by_executing_or_blocked_reaction(reaction_t* reaction) {
    if (reaction == NULL) {
        return false;
    }
    // The head of the executing_q has the lowest level of anything
    // on the queue, and that level is also lower than anything on the
    // transfer_q (because reactions on the transfer queue are blocked
    // by reactions on the executing_q). Hence, if the candidate reaction
    // has a level less than or equal to that of the head of the
    // executing_q, then it is executable and we don't need to check
    // the contents of either queue further.
    if (pqueue_size(executing_q) > 0
            && reaction->index <= ((reaction_t*) pqueue_peek(executing_q))->index) {
        return false;
    }

    for (size_t i = 1; i < executing_q->size; i++) {
        reaction_t* running = (reaction_t*) executing_q->d[i];
        if (_lf_has_precedence_over(running, reaction)) {
            DEBUG_PRINT("Reaction %s is blocked by reaction %s.", reaction->name, running->name);
            return true;
        }
    }

    for (size_t i = 0; i < transfer_q.next - transfer_q.start; i++) {
        reaction_t* blocked = (reaction_t*) (transfer_q.start + i);
        if (_lf_has_precedence_over(blocked, reaction)) {
            DEBUG_PRINT("Reaction %s is blocked by blocked reaction %s.", reaction->name, blocked->name);
            return true;
        }
    }
    // NOTE: checks against the transfer_q are not performed in 
    // this function but at its call site (where appropriate).

    // printf("Not blocking for reaction with chainID %llu and level %llu\n", reaction->chain_id, reaction->index);
    // pqueue_dump(executing_q, stdout, executing_q->prt);
    return false;
}

/**
 * @brief Distribute any reaction that is ready to execute to idle worker thread(s).
 * 
 * This assumes that the caller is not holding any thread mutexes.
 * 
 * @return Number of reactions that were successfully distributed to worker threads.
 */ 
static inline int _lf_sched_distribute_ready_reactions_locked() {    
    reaction_t* r;

    // Keep track of the number of reactions distributed
    int reactions_distributed = 0;

    // Find a reaction that is ready to execute.
    while ((r = (reaction_t*)pqueue_pop(reaction_q)) != NULL) {
        // Set the reaction aside if it is blocked, either by another
        // blocked reaction or by a reaction that is currently executing.
        if (!_lf_is_blocked_by_executing_or_blocked_reaction(r)) {
            if (_lf_sched_distribute_ready_reaction(r)){
                // Found a thread to execute r
                reactions_distributed++;
                continue;
            }
            // Couldn't find a thread to execute r.
            DEBUG_PRINT("Scheduler: Could not find an idle thread to execute reaction %s.", r->name);
        }
        // Couldn't execute the reaction. Will have to put it back in the
        // reaction queue.
        vector_push(&transfer_q, (void*)r);
    }

    // Put back the set-aside reactions into the reaction queue.
    reaction_t* reaction_to_transfer = NULL;
    while ((reaction_to_transfer = (reaction_t*)vector_pop(&transfer_q)) != NULL) {
        pqueue_insert(reaction_q, reaction_to_transfer);
    }
    
    // Reset the balancing index since this work distribution round is over.
    _lf_sched_balancing_index = 0;
    return reactions_distributed;
}

/**
 * @brief Transfer the contents of worker thread queues to the actual global queues.
 * 
 * This will transfer worker threads' output reactions to the reaction queue and
 * removes worker threads' done reactions from the executing queue.
 * 
 * This assumes that the caller is not holding any thread mutexes.
 * 
 * @return true If any of the workers were busy.
 * @return false All the workers were idle.
 */
bool _lf_sched_update_queues() {
    bool is_any_worker_busy = false;
    for (int i = 0; i < _lf_sched_number_of_workers; i++) {
        // Check if we have actually assigned work to this worker thread previously.
        reaction_t* reaction_to_add = NULL;
        reaction_t* reaction_to_remove = NULL;
        if (!_lf_sched_is_worker_idle(i)) {
            // Don't touch the queues since the thread is still busy
            DEBUG_PRINT("Scheduler: Worker %d is busy. Won't empty the queues for it.", i);
            is_any_worker_busy = true;
            continue;
        }
        DEBUG_PRINT("Scheduler: Emptying queues of Worker %d.", i);
        // Add output reactions to the reaction queue
        while(
            (reaction_to_add = 
            (reaction_t*)vector_pop(&_lf_sched_threads_info[i].output_reactions))
            != NULL) {
            DEBUG_PRINT(
                "Scheduler: Inserting reaction %s into the reaction queue.",
                reaction_to_add->name
            );
            if (pqueue_insert(reaction_q, reaction_to_add) != 0) {
                error_print_and_exit("Scheduler: Could not properly fill the reaction queue.");
            }
        }

        // Remove done reactions from the executing queue
        while(
            (reaction_to_remove = 
            (reaction_t*)vector_pop(&_lf_sched_threads_info[i].done_reactions))
            != NULL) {
            DEBUG_PRINT(
                "Scheduler: Removing reaction %s from executing queue.",
                reaction_to_remove->name
            );
            if (pqueue_remove(executing_q, reaction_to_remove) != 0) {
                error_print_and_exit("Scheduler: Could not properly clear the executing queue.");
            }
        }
    }
    return is_any_worker_busy;
}

/**
 * @brief If there is work to be done, notify workers individually.
 * 
 * This assumes that the caller is not holding any thread mutexes.
 */
void _lf_sched_notify_workers() {
    for (int i=0; i< _lf_sched_number_of_workers; i++) {
        if (pqueue_size(_lf_sched_threads_info[i].ready_reactions) > 0 &&
            lf_bool_compare_and_swap(&_lf_sched_threads_info[i].is_idle, 1, 0)) {
            DEBUG_PRINT("Notifying worker %d that there is work to do.", i);
            lf_mutex_lock(&_lf_sched_threads_info[i].mutex);
            lf_cond_signal(&_lf_sched_threads_info[i].cond);
            lf_mutex_unlock(&_lf_sched_threads_info[i].mutex);
        }
    }
}

/**
 * @brief Advance tag or distribute reactions to worker threads.
 *
 * Advance tag if there are no reactions in the reaction queue or in progress. If
 * there are such reactions, distribute them to worker threads. As part of its
 * book-keeping, this function will clear the output_reactions and
 * done_reactions queues of all idle worker threads if appropriate.
 * 
 * This function assumes the caller does not hold the 'mutex' lock.
 * 
 * @return should_exit True if the worker thread should exit. False otherwise.
 */
bool _lf_sched_try_advance_tag_and_distribute() {
    bool return_value = false;

    // Executing queue must be empty when this is called.
    assert(pqueue_size(executing_q) != 0);

    lf_mutex_lock(&mutex);
    if (!_lf_sched_update_queues()) {
        if (pqueue_size(reaction_q) == 0
                && pqueue_size(executing_q) == 0) {
            // Nothing more happening at this tag.
            DEBUG_PRINT("Scheduler: Advancing time.");
            // This thread will take charge of advancing tag.
            if (_lf_sched_advance_tag_locked()) {
                DEBUG_PRINT("Scheduler: Reached stop tag.");
                return_value = true;
            }
        }
    }
    
    int reactions_distributed = _lf_sched_distribute_ready_reactions_locked();    
    lf_mutex_unlock(&mutex);

    if (reactions_distributed) {
        _lf_sched_notify_workers();
    }

    // pqueue_dump(executing_q, print_reaction);
    return return_value;
}

/**
 * @brief Signal all worker threads that it is time to stop.
 * 
 */
void _lf_sched_signal_stop() {
    for (int i=0; i < _lf_sched_number_of_workers; i++) {
        lf_mutex_lock(&_lf_sched_threads_info[i].mutex);
        _lf_sched_threads_info[i].should_stop = true;
        lf_cond_signal(&_lf_sched_threads_info[i].cond);
        lf_mutex_unlock(&_lf_sched_threads_info[i].mutex);
    }
}

/**
 * @brief Perform a round of scheduling
 */
void _lf_sched_do_scheduling() {
    if(_lf_sched_try_advance_tag_and_distribute()) {
        _lf_sched_signal_stop();
    }
}

/**
 * @brief Wait until the scheduler assigns work.
 *
 * This will inform the scheduler that this thread is idle via @see
 * '_lf_sched_ask_for_work' and waits until work is handed out by the
 * scheduler to the worker thread 'worker_number' or it's time for the
 * worker thread to stop.
 *
 * @param worker_number The worker number of the worker thread asking for work
 * to be assigned to it.
 */
void _lf_sched_wait_for_work(size_t worker_number) {
    lf_bool_compare_and_swap(&_lf_sched_threads_info[worker_number].is_idle, 0, 1);

    if (lf_bool_compare_and_swap(&_lf_sched_scheduling_in_progress, false, true)) {
        // Ask for more work from the scheduler.
        _lf_sched_do_scheduling();
        lf_bool_compare_and_swap(&_lf_sched_threads_info[worker_number].is_idle, 1, 0);
        lf_bool_compare_and_swap(&_lf_sched_scheduling_in_progress, true, false);
    } else {
        lf_mutex_lock(&_lf_sched_threads_info[worker_number].mutex);
    
        // Check if it is time to stop. If it is, return.
        if (_lf_sched_should_stop(worker_number)) { // Time to stop
            // The thread is going to exit and thus is not idle.
            lf_mutex_unlock(&_lf_sched_threads_info[worker_number].mutex);
            return;
        }
        // If no work has been assigned, wait for the signal from the scheduler
        DEBUG_PRINT("Worker %d: Waiting on work to be handed out.", worker_number);
        lf_cond_wait(&_lf_sched_threads_info[worker_number].cond, &_lf_sched_threads_info[worker_number].mutex);
        lf_mutex_unlock(&_lf_sched_threads_info[worker_number].mutex);
    }
}

///////////////////// Scheduler Init and Destroy API /////////////////////////
/**
 * @brief Initialize the scheduler.
 * 
 * This has to be called before other functions of the scheduler can be used.
 * 
 * @param number_of_workers Indicate how many workers this scheduler will be managing.
 */
void lf_sched_init(size_t number_of_workers) {
    DEBUG_PRINT("Scheduler: Initializing with %d workers", number_of_workers);

    _lf_sched_number_of_workers = number_of_workers;

    // Reaction queue ordered first by deadline, then by level.
    // The index of the reaction holds the deadline in the 48 most significant bits,
    // the level in the 16 least significant bits.
    reaction_q = pqueue_init(INITIAL_REACT_QUEUE_SIZE, in_reverse_order, get_reaction_index,
            get_reaction_position, set_reaction_position, reaction_matches, print_reaction);
    transfer_q = vector_new(INITIAL_REACT_QUEUE_SIZE);
    // Create a queue on which to put reactions that are currently executing.
    executing_q = pqueue_init(_lf_number_of_threads, in_reverse_order, get_reaction_index,
        get_reaction_position, set_reaction_position, reaction_matches, print_reaction);
    
    _lf_sched_threads_info = 
        (_lf_sched_thread_info_t*)malloc(
            sizeof(_lf_sched_thread_info_t) * _lf_sched_number_of_workers);
    
    for (int i=0; i < _lf_sched_number_of_workers; i++) {
        lf_cond_init(&_lf_sched_threads_info[i].cond);
        lf_mutex_init(&_lf_sched_threads_info[i].mutex);
        _lf_sched_threads_info[i].ready_reactions = 
            pqueue_init(
                INITIAL_REACT_QUEUE_SIZE, 
                in_reverse_order, 
                get_reaction_index,
                get_reaction_position, 
                set_reaction_position, 
                reaction_matches, 
                print_reaction
            );
        _lf_sched_threads_info[i].output_reactions = 
            vector_new(INITIAL_REACT_QUEUE_SIZE);
        _lf_sched_threads_info[i].done_reactions = 
            vector_new(INITIAL_REACT_QUEUE_SIZE);
        _lf_sched_threads_info[i].should_stop = false;
        _lf_sched_threads_info[i].is_idle = 0;
    }
}

/**
 * @brief Free the memory used by the scheduler.
 * 
 * This must be called when the scheduler is no longer needed.
 */
void lf_sched_free() {
    for (int i=0; i < _lf_sched_number_of_workers; i++) {
        pqueue_free(_lf_sched_threads_info[i].ready_reactions);
        vector_free(&_lf_sched_threads_info[i].output_reactions);
        vector_free(&_lf_sched_threads_info[i].done_reactions);
    }
    // pqueue_free(reaction_q); FIXME: This might be causing weird memory errors
    vector_free(&transfer_q);
    pqueue_free(executing_q);
    free(_lf_sched_threads_info);
}

///////////////////// Scheduler Worker API (public) /////////////////////////
/**
 * @brief Ask the scheduler for one more reaction.
 * 
 * This function blocks until it can return a ready reaction for worker thread
 * 'worker_number' or it is time for the worker thread to stop and exit (where a
 * NULL value would be returned).
 * 
 * @param worker_number 
 * @return reaction_t* A reaction for the worker to execute. NULL if the calling
 * worker thread should exit.
 */
reaction_t* lf_sched_get_ready_reaction(int worker_number) {
    // Iterate until the stop_tag is reached or reaction queue is empty
    while (!_lf_sched_should_stop(worker_number)) {
        lf_mutex_lock(&_lf_sched_threads_info[worker_number].mutex);
        reaction_t* reaction_to_return = (reaction_t*)pqueue_pop(_lf_sched_threads_info[worker_number].ready_reactions);
        lf_mutex_unlock(&_lf_sched_threads_info[worker_number].mutex);
        
        if (reaction_to_return == NULL && _lf_sched_number_of_workers > 1) {
            // Try to steal
            int index_to_steal = (worker_number + 1) % _lf_sched_number_of_workers;
            lf_mutex_lock(&_lf_sched_threads_info[index_to_steal].mutex);
            reaction_to_return = 
                pqueue_pop(_lf_sched_threads_info[index_to_steal].ready_reactions);
            if (reaction_to_return != NULL) {
                DEBUG_PRINT(
                    "Worker %d: Had nothing on my ready queue. Stole reaction %s from %d", 
                    worker_number,
                    reaction_to_return->name,
                    index_to_steal);
            }
            lf_mutex_unlock(&_lf_sched_threads_info[index_to_steal].mutex);
        }

        if (reaction_to_return != NULL) {
            // Got a reaction
            return reaction_to_return;
        } else {
            DEBUG_PRINT("Worker %d is out of ready reactions.", worker_number);
            // Ask the scheduler for more work or wait
            _lf_sched_wait_for_work(worker_number);
        }
    }

    // It's time for the worker thread to stop and exit.
    return NULL;
}

/**
 * @brief Inform the scheduler that worker thread 'worker_number' is done
 * executing the 'done_reaction'.
 * 
 * @param worker_number The worker number for the worker thread that has
 * finished executing 'done_reaction'.
 * @param done_reaction The reaction is that is done.
 */
void lf_sched_done_with_reaction(size_t worker_number, reaction_t* done_reaction) {
    if (!lf_bool_compare_and_swap(&done_reaction->status, running, inactive)) {
        error_print_and_exit("Unexpected reaction status: %d. Expected %d.", 
            done_reaction->status,
            running);
    }
    vector_push(&_lf_sched_threads_info[worker_number].done_reactions, (void*)done_reaction);
}

/**
 * @brief Inform the scheduler that worker thread 'worker_number' would like to
 * trigger 'reaction' at the current tag.
 * 
 * If a worker number is not available (e.g., this function is not called by a
 * worker thread), -1 should be passed as the 'worker_number'.
 * 
 * The scheduler will ensure that the same reaction is not triggered twice in
 * the same tag.
 * 
 * @param reaction The reaction to trigger at the current tag.
 * @param worker_number The ID of the worker that is making this call. 0 should be
 *  used if there is only one worker (e.g., when the program is using the
 *  unthreaded C runtime). -1 is used for an anonymous call in a context where a
 *  worker number does not make sense (e.g., the caller is not a worker thread).
 * 
 */
void lf_sched_trigger_reaction(reaction_t* reaction, int worker_number) {
    if (worker_number == -1) {
        // The scheduler should handle this immediately
        lf_mutex_lock(&mutex);
        // Do not enqueue this reaction twice.
        if (reaction != NULL && lf_bool_compare_and_swap(&reaction->status, inactive, queued)) {
            DEBUG_PRINT("Enqueing downstream reaction %s, which has level %lld.",
                        reaction->name, reaction->index & 0xffffLL);
            // Immediately put 'reaction' on the reaction queue.
            pqueue_insert(reaction_q, reaction);
        }
        lf_mutex_unlock(&mutex);
        return;
    }
    if (reaction != NULL && lf_bool_compare_and_swap(&reaction->status, inactive, queued)) {
        DEBUG_PRINT("Worker %d: Enqueuing downstream reaction %s, which has level %lld.",
        		worker_number, reaction->name, reaction->index & 0xffffLL);
        reaction->worker_affinity = worker_number;
        // Note: The scheduler will check that we don't enqueue this reaction
        // twice when it is actually pushing it to the global reaction queue.
        vector_push(&_lf_sched_threads_info[worker_number].output_reactions, (void*)reaction);
    }
}