#include <setjmp.h>
#include <stdio.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <iostream>
#include <map>
#include <list>
#include <vector>
#include <bits/stdc++.h>
#include "uthreads.h"

#define FAILURE -1
#define SUCCESS 0
#define ERROR_EXIT 1
#define MAIN_THREAD_ID 0
#define LONGJMP_VAL 1

#define THREAD_LIB_ERR "thread library error: "
#define SYS_ERR "system error: "
#define QUANTUM_USECS_NON_POSITIVE "quantom_usecs has to be non positive integer"
#define NO_SUCH_THREAD "thread doesn't exist"
#define OVER_MAX_THREADS "you reached your threads count limit"
#define BLOCK_MAIN_THREAD "main thread can't be blocked"
#define SLEEP_MAIN_THREAD "main thread can't be fall asleep"
#define INVALID_SLEEP_QUANTUM "quantum sleep number has to be positive integer"
#define SETITIMER_ERR "error in setitimer"
#define SIGACTION_ERR "error in sigaction"
#define BLOCK_SIGNAL_ERR "error in block signal"
#define UNBLOCK_SIGNAL_ERR "error in unblock signal"

#define QUANTUM_TO_SEC(usec) (usec / 1000000)
#define QUANTUM_TO_USEC(usec) (usec % 1000000)

// BLACK BOX
#ifdef __x86_64__
/* code for 64 bit Intel arch */

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
                 "rol    $0x11,%0\n"
            : "=g" (ret)
            : "0" (addr));
    return ret;
}

#else
/* code for 32 bit Intel arch */

typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5


/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%gs:0x18,%0\n"
                 "rol    $0x9,%0\n"
    : "=g" (ret)
    : "0" (addr));
    return ret;
}

#endif
// BLACK BOX

/**
 * Enum for the possible states of a thread
 */
enum states {
    READY, RUNNING, BLOCKED, TO_BE_DELETED
};

/**
 * Enum for the possible flags when we have an action to perform on the running thread
 */
enum scheduler_flags {
    TIME, BLOCK, TERMINATE, SLEEP
};

class Thread {
public:
    states state;
    int quantum;
    bool sleeping;
    Thread(): state(READY), quantum(0), sleeping(false) {}
};

typedef void (*thread_entry_point)();
sigjmp_buf env[MAX_THREAD_NUM];
std::map<int, Thread *> threads_map;
std::queue<int> ready_threads;
std::priority_queue <int, std::vector<int>, std::greater<int> > available_ids;
std::map<int, std::vector<int>> sleeping_map;
char stacks[MAX_THREAD_NUM][STACK_SIZE];
int curr_thread_id = -1;
int total_quantums = 0;
struct itimerval timer;
sigset_t blocked_signals_set;

/**
 * Delete all the threads from the program resources
 */
void delete_all_threads() {
    for (std::pair<int, Thread *> pair : threads_map) {
        delete pair.second;
    }
}

/**
 * A function that prints error message from a system call to stderr
 * @param msg the message to print
 */
void system_error(const char *msg = nullptr) {
    if (msg) {
        std::cerr << SYS_ERR << msg << "\n";
    }
    delete_all_threads();
    exit(ERROR_EXIT);
}

/**
 * Blocks signals from interrupt
 */
void block_signals() {
    if (sigprocmask (SIG_BLOCK, &blocked_signals_set, NULL) < 0) {
        system_error(BLOCK_SIGNAL_ERR);
    }
}

/**
 * Release the block of the signals
 */
void unblock_signals() {
    if (sigprocmask (SIG_UNBLOCK, &blocked_signals_set, NULL) < 0) {
        system_error(UNBLOCK_SIGNAL_ERR);
    }
}

/**
 * A function that prints error message by the thread library to stderr
 * @param msg the message to print
 * @return -1 (FAILURE)
 */
int thread_lib_error(const std::string &msg) {
    std::cerr << THREAD_LIB_ERR << msg << "\n";
    return FAILURE;
}

/**
 * Set the thread to ready, and adds it to the priority queue of ready threads
 * @param tid the thread's id
 */
void set_thread_as_ready(int tid) {
    threads_map[tid]->state = READY;
    ready_threads.push(tid);
}

/**
 * Set the current thread (by his id) to running status
 */
void set_curr_thread_as_running() {
    threads_map[curr_thread_id]->state = RUNNING;
}

/**
 * Finds the next ready thread that need to run
 * @return the thread's id
 */
int get_next_available_id() {
    int ret_id = available_ids.top();
    available_ids.pop();
    return ret_id;
}

/**
 * Set the global variable curr_thread_id to the next ready thread that is going to run
 */
void set_next_ready_thread_id() {
    if (ready_threads.empty()) {
        curr_thread_id = MAIN_THREAD_ID;
        return;
    }
    curr_thread_id = ready_threads.front();
    ready_threads.pop();
}

/**
 * Remove a thread from the priority queue of ready-threads
 * @param tid the thread's id
 */
void remove_from_ready_threads(int tid) {
    std::queue<int> temp;
    while (!ready_threads.empty()) {
        int curr_id = ready_threads.front();
        if (tid != curr_id) {
            temp.push(curr_id);
        }
        ready_threads.pop();
    }
    ready_threads = temp;
}

/**
 * Deleted a single thread from the memory of the program
 * @param tid the thread's id
 */
void delete_thread(int tid) {
    Thread *curr_thread = threads_map[tid];
    if (curr_thread->state == READY) {
        remove_from_ready_threads(tid);
    } else if (curr_thread->sleeping) {
        for (const auto& sleeping_pair : sleeping_map) {
            std::vector<int> v = sleeping_pair.second;
            remove(v.begin(), v.end(), tid);
        }
    }
    available_ids.push(tid);
    threads_map.erase(tid);
    delete curr_thread;
}

/**
 * Find the next ready thread that should run (by the priority queue), and run it
 */
void run_next_ready_thread() {
    set_next_ready_thread_id();
    set_curr_thread_as_running();
}

/**
 * Awake all the sleeping threads that should wake up in the current quantum (total quantum)
 */
void wake_up_sleepers() {
    for (int sleeper_id : sleeping_map[total_quantums]) {
        Thread *curr_thread = threads_map[sleeper_id];
        if (curr_thread->state == READY) {
            ready_threads.push(sleeper_id);
        }
        curr_thread->sleeping = false;
    }
}

/**
 * Resets the timer
 */
void reset_timer() {
    if (setitimer(ITIMER_VIRTUAL, &timer, NULL) < 0)
    {
        system_error(SETITIMER_ERR);
    }
}

/**
 * Update the total quantum counter and the current-running thread quantum
 */
void update_quantum() {
    total_quantums++;
    threads_map[curr_thread_id]->quantum++;
}

/**
 * The function handle a change with the running thread
 * @param flag
 */
void scheduler(scheduler_flags flag) {
    if (threads_map.count(curr_thread_id) <= 0) {
        thread_lib_error(NO_SUCH_THREAD);
        unblock_signals();
        return;
    }
    Thread *curr_thread = threads_map[curr_thread_id];
    switch (flag) {
        case TIME: {
            set_thread_as_ready(curr_thread_id);
            break;
        } case TERMINATE: {
            threads_map[curr_thread_id]->state = TO_BE_DELETED;
            break;
        } case SLEEP: {
            curr_thread->sleeping = true;
            curr_thread->state = READY;
            break;
        } case BLOCK: {
            curr_thread->state = BLOCKED;
            break;
        }
    }

    if (flag != TERMINATE) {
        int ret_val = sigsetjmp(env[curr_thread_id], 1);
        if (ret_val == LONGJMP_VAL) {
            unblock_signals();
            return;
        }
    }

    wake_up_sleepers();
    run_next_ready_thread();
    update_quantum();
    reset_timer();
    siglongjmp(env[curr_thread_id], LONGJMP_VAL);
}

/**
 * The function called when each quantum pass
 * @param sig signal id, not being used
 */
void timer_handler(int sig) {
    block_signals();
    scheduler(TIME);
    wake_up_sleepers();
    unblock_signals();
};

/**
 * Initialize the queue of the available ids
 */
void initiate_ids() {
    for (int i = 1; i < MAX_THREAD_NUM; ++i) {
        available_ids.push(i);
    }
}

/**
 * Initialize the timer
 * @param quantum_usecs the quantum to usecs arg
 */
void initiate_timer(int quantum_usecs) {
    struct sigaction sa;

    sa.sa_handler = &timer_handler;
    if (sigaction(SIGVTALRM, &sa, NULL) < 0)
    {
        system_error(SIGACTION_ERR);
    }

    timer.it_value.tv_sec = QUANTUM_TO_SEC(quantum_usecs);
    timer.it_value.tv_usec = QUANTUM_TO_USEC(quantum_usecs);
    timer.it_interval.tv_sec = QUANTUM_TO_SEC(quantum_usecs);
    timer.it_interval.tv_usec = QUANTUM_TO_USEC(quantum_usecs);

    reset_timer();
}

/**
 * Initiate any other thread that isn't the main thread
 * @param tid thread's id
 * @param stack pointer to the thread's stack
 * @param entry_point the function which the thread should start running from
 */
void initiate_thread(int tid, char* stack, thread_entry_point entry_point) {
    address_t sp = (address_t) stack + STACK_SIZE - sizeof(address_t);
    address_t pc = (address_t) entry_point;
    sigsetjmp(env[tid], 1);
    (env[tid]->__jmpbuf)[JB_SP] = translate_address(sp);
    (env[tid]->__jmpbuf)[JB_PC] = translate_address(pc);
    sigemptyset(&env[tid]->__saved_mask);
}

/**
 * Initialize the main thread- env, status and quantum
 */
void initiate_main_thread() {
    Thread *main_thread = new Thread();
    int ret_val = sigsetjmp(env[MAIN_THREAD_ID], 1);
    if (ret_val == LONGJMP_VAL){
        return;
    }
    main_thread->state = RUNNING;
    main_thread->quantum++;
    threads_map[MAIN_THREAD_ID] = main_thread;
    curr_thread_id = MAIN_THREAD_ID;
    sigemptyset(&env[MAIN_THREAD_ID]->__saved_mask);
};

/**
 * Blocks SIGVTALRM signal
 */
void initiate_block_signal_set() {
    if (sigemptyset(&blocked_signals_set) < 0) {
        system_error(BLOCK_SIGNAL_ERR);
    }
    if (sigaddset(&blocked_signals_set, SIGVTALRM) < 0) {
        system_error(BLOCK_SIGNAL_ERR);
    }
}

/**
 * @brief initializes the thread library.
 *
 * Once this function returns, the main thread (tid == 0) will be set as RUNNING. There is no need to
 * provide an entry_point or to create a stack for the main thread - it will be using the "regular" stack and PC.
 * You may assume that this function is called before any other thread library function, and that it is called
 * exactly once.
 * The input to the function is the length of a quantum in micro-seconds.
 * It is an error to call this function with non-positive quantum_usecs.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_init(int quantum_usecs) {
    if (quantum_usecs <= 0) {
        std::cerr << THREAD_LIB_ERR << QUANTUM_USECS_NON_POSITIVE << "\n";
        return FAILURE;
    }
    initiate_block_signal_set();
    block_signals();
    initiate_main_thread();
    initiate_ids();
    initiate_timer(quantum_usecs);
    total_quantums = 1;
    unblock_signals();
    return SUCCESS;
}

/**
 * @brief Creates a new thread, whose entry point is the function entry_point with the signature
 * void entry_point(void).
 *
 * The thread is added to the end of the READY threads list.
 * The uthread_spawn function should fail if it would cause the number of concurrent threads to exceed the
 * limit (MAX_THREAD_NUM).
 * Each thread should be allocated with a stack of size STACK_SIZE bytes.
 * It is an error to call this function with a null entry_point.
 *
 * @return On success, return the ID of the created thread. On failure, return -1.
*/
int uthread_spawn(thread_entry_point entry_point) {
    block_signals();
    if (!entry_point || available_ids.empty()) {
        unblock_signals();
        return thread_lib_error(OVER_MAX_THREADS);
    }
    int thread_id = get_next_available_id();
    ready_threads.push(thread_id);
    auto *thread = new Thread();
    threads_map[thread_id] = thread;
    initiate_thread(thread_id, stacks[thread_id], entry_point);
    unblock_signals();
    return thread_id;
}

/**
 * @brief Terminates the thread with ID tid and deletes it from all relevant control structures.
 *
 * All the resources allocated by the library for this thread should be released. If no thread with ID tid exists it
 * is considered an error. Terminating the main thread (tid == 0) will result in the termination of the entire
 * process using exit(0) (after releasing the assigned library memory).
 *
 * @return The function returns 0 if the thread was successfully terminated and -1 otherwise. If a thread terminates
 * itself or the main thread is terminated, the function does not return.
*/
int uthread_terminate(int tid) {
    block_signals();
    if (threads_map.count(tid) <= 0) {
        unblock_signals();
        return thread_lib_error(NO_SUCH_THREAD);
    }
    if (tid == MAIN_THREAD_ID) {
        delete_all_threads();
        exit(SUCCESS);
    } else if (tid == curr_thread_id) {
        scheduler(TERMINATE);
    }
    delete_thread(tid);
    unblock_signals();
    return SUCCESS;
}

/**
 * @brief Blocks the thread with ID tid. The thread may be resumed later using uthread_resume.
 *
 * If no thread with ID tid exists it is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision should be made. Blocking a thread in
 * BLOCKED state has no effect and is not considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_block(int tid) {
    block_signals();
    if (threads_map.count(tid) <= 0) {
        unblock_signals();
        return thread_lib_error(NO_SUCH_THREAD);
    }
    Thread *curr_thread = threads_map[tid];
    if (tid == MAIN_THREAD_ID) {
        unblock_signals();
        return thread_lib_error(BLOCK_MAIN_THREAD);
    } else if (tid == curr_thread_id) {
        scheduler(BLOCK);
    } else {
        if (curr_thread->state == READY && !curr_thread->sleeping) {
            remove_from_ready_threads(tid);
        }
        curr_thread->state = BLOCKED;
    }
    unblock_signals();
    return SUCCESS;
}

/**
 * @brief Resumes a blocked thread with ID tid and moves it to the READY state.
 *
 * Resuming a thread in a RUNNING or READY state has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_resume(int tid) {
    block_signals();
    if (threads_map.count(tid) <= 0) {
        unblock_signals();
        return thread_lib_error(NO_SUCH_THREAD);
    }
    Thread *curr_thread = threads_map[tid];
    if (curr_thread->state == BLOCKED) {
        set_thread_as_ready(tid);
    }
    unblock_signals();
    return SUCCESS;
}

/**
 * @brief Blocks the RUNNING thread for num_quantums quantums.
 *
 * Immediately after the RUNNING thread transitions to the BLOCKED state a scheduling decision should be made.
 * After the sleeping time is over, the thread should go back to the end of the READY queue.
 * If the thread which was just RUNNING should also be added to the READY queue, or if multiple threads wake up
 * at the same time, the order in which they're added to the end of the READY queue doesn't matter.
 * The number of quantums refers to the number of times a new quantum starts, regardless of the reason. Specifically,
 * the quantum of the thread which has made the call to uthread_sleep isn’t counted.
 * It is considered an error if the main thread (tid == 0) calls this function.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_sleep(int num_quantums) {
    block_signals();
    if (num_quantums <= 0) {
        unblock_signals();
        return thread_lib_error(INVALID_SLEEP_QUANTUM);
    } else if (curr_thread_id == MAIN_THREAD_ID) {
        unblock_signals();
        return thread_lib_error(SLEEP_MAIN_THREAD);
    }
    sleeping_map[total_quantums + num_quantums].push_back(curr_thread_id);
    scheduler(SLEEP);
    unblock_signals();
    return SUCCESS;
}

/**
 * @brief Returns the thread ID of the calling thread.
 *
 * @return The ID of the calling thread.
*/
int uthread_get_tid() {
    return curr_thread_id;
}

/**
 * @brief Returns the total number of quantums since the library was initialized,
 * including the current quantum.
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantum starts, regardless of the reason, this number should be increased by 1.
 *
 * @return The total number of quantums.
*/
int uthread_get_total_quantums() {
    return total_quantums;
}

/**
 * @brief Returns the number of quantums the thread with ID tid was in RUNNING state.
 *
 * On the first time a thread runs, the function should return 1. Every additional quantum that the thread starts should
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state when this function is called, include
 * also the current quantum). If no thread with ID tid exists it is considered an error.
 *
 * @return On success, return the number of quantums of the thread with ID tid. On failure, return -1.
*/
int uthread_get_quantums(int tid) {
    block_signals();
    if (threads_map.count(tid) <= 0) {
        unblock_signals();
        return thread_lib_error(NO_SUCH_THREAD);
    }
    Thread *curr_thread = threads_map[tid];
    unblock_signals();
    return curr_thread->quantum;
}
