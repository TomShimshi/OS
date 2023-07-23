#include <iostream>
#include <atomic>
#include <algorithm>
#include "MapReduceFramework.h"
#include "Barrier.h"

#define SYSTEM_ERROR "system error: text\n"
#define CREATE_PTHREAD_ERROR "pthread_create failed"
#define MUTEX_DESTROY_ERROR "mutex_destroy failed"
#define LOCK_WAIT_MUTEX_ERROR "lock mutex for waitForJob failed"
#define UNLOCK_WAIT_MUTEX_ERROR "unlock mutex for waitForJob failed"
#define TOTAL_ELEMENTS_BITS 34
#define CURRENT_ELEMENTS_BITS 2
#define LOCK_ADD_OUTPUT_MUTEX_ERROR "lock mutex for adding to output vector failed"
#define UNLOCK_ADD_OUTPUT_MUTEX_ERROR "unlock mutex for adding to output vector failed"

struct ThreadContext {
    int tid;
    JobHandle job;
};

struct JobContext {
    JobContext(const MapReduceClient& client, const InputVec& inputVec, OutputVec& outputVec,
               int threadsCount):
            client(client), inputVec(inputVec), outputVec(outputVec), threadsCount(threadsCount),
            waitForJobMutex(PTHREAD_MUTEX_INITIALIZER), addToOutputMutex(PTHREAD_MUTEX_INITIALIZER),
            counterUpdateMutex(PTHREAD_MUTEX_INITIALIZER), barrier(new Barrier(threadsCount)),
            threads(new pthread_t[threadsCount]), contexts(new ThreadContext[threadsCount]){}

    const MapReduceClient& client;
    const InputVec& inputVec;
    OutputVec& outputVec;
    int threadsCount{};
    pthread_mutex_t waitForJobMutex{};
    pthread_mutex_t addToOutputMutex{};
    pthread_mutex_t counterUpdateMutex{};
    Barrier *barrier;

    // the first 2 are stage, next 31 are current stage progress
    // and the last 31 are current stage total
    std::atomic<uint64_t> atomicState{0};
    std::atomic<bool> waitForJobCalled{false};
    std::atomic<size_t> mapProgress{0};
    std::atomic<size_t> shuffleProgress{0};
    std::atomic<size_t> reduceProgress{0};
    std::atomic<size_t> totalMapped{0};
    std::atomic<size_t> totalReduced{0};
    pthread_t *threads;
    std::vector<IntermediateVec> intermediateVectorsVec;
    std::vector<IntermediateVec> shuffledVec;
    ThreadContext *contexts;

};

/**
 * Find the max key between several sorted vectors
 * @param vectorsVec vectors of IntermediateVec
 * @return the max K2
 */
K2 *findMaxKey (const std::vector<IntermediateVec >& vectorsVec) {
    K2 *maxKey = nullptr;
    for (const auto& vec : vectorsVec) {
        if (vec.empty()) {
            continue;
        }
        K2 *currKey = vec.back().first;
        if (!maxKey || *maxKey < *currKey) {
            maxKey = currKey;
        }
    }
    return maxKey;
}

/**
 * Safe exit with print
 * @param msg
 */
void exitWithError(const char *msg) {
    std::cout << SYSTEM_ERROR << msg << std::endl;
    exit(1);
}

/**
 * called by map function, add the mapped K2 V2 to our database
 * @param key key to add
 * @param value value to add
 * @param context the context of the vector
 */
void emit2 (K2* key, V2* value, void* context) {
    auto *threadContext = (ThreadContext *)context;
    auto *jobContext = (JobContext *)threadContext->job;
    jobContext->intermediateVectorsVec.at(threadContext->tid).emplace_back(key, value);
    jobContext->totalMapped++;
}

/**
 * called by reduce function, add the mapped K3 V3 to our database
 * @param key key to add
 * @param value value to add
 * @param context the context of the vector
 */
void emit3 (K3* key, V3* value, void* context) {
    auto *threadContext = (ThreadContext *)context;
    auto *jobContext = (JobContext *)threadContext->job;

    if (pthread_mutex_lock(&jobContext->addToOutputMutex)) {
        exitWithError(LOCK_ADD_OUTPUT_MUTEX_ERROR);
    }

    jobContext->outputVec.emplace_back(key, value);
    jobContext->atomicState += (1 << CURRENT_ELEMENTS_BITS);
    jobContext->totalReduced++;

    if (pthread_mutex_unlock(&jobContext->addToOutputMutex)) {
        exitWithError(UNLOCK_ADD_OUTPUT_MUTEX_ERROR);
    }
}

/**
 * Handle the map section for each thread
 * @param threadContext the thread context
 */
void threadMap(ThreadContext* threadContext) {
    auto *jobContext = (JobContext *)threadContext->job;
    size_t oldValue = jobContext->mapProgress++;
    if (oldValue < jobContext->inputVec.size()) {
        InputPair currPair = jobContext->inputVec.at(oldValue);
        jobContext->client.map(currPair.first, currPair.second, threadContext);
        jobContext->atomicState += (1 << CURRENT_ELEMENTS_BITS);
    }
}

/**
 * A function for std::sort, compare between 2 IntermediatePair elements
 * @param firstPair first element
 * @param secondPair second element
 * @return true if the first element is smaller than the second one, else otherwise
 */
bool sortHelper(const IntermediatePair firstPair, const IntermediatePair secondPair) {
    return *(firstPair.first) < *(secondPair.first);
}

/**
 * Handle the sort section for each thread
 * @param threadContext the thread context
 */
void threadSort(ThreadContext* threadContext) {
    auto *jobContext = (JobContext *)threadContext->job;
    IntermediateVec &currVec = jobContext->intermediateVectorsVec.at(threadContext->tid);
    std::sort(currVec.begin(), currVec.end(), sortHelper);
}

/**
 * Helper function for equality check between 2 elements of K2
 * @param first first element
 * @param second second element
 * @return true if both element are equal, else otherwise
 */
bool K2equality(K2 *first, K2 *second) {
    return !((*first < *second) || (*second < *first));
}

/**
 * Handle the shuffle section for the main thread
 * @param threadContext the thread context
 */
void threadShuffle(ThreadContext* threadContext) {
    auto *jobContext = (JobContext *)threadContext->job;
    size_t vectorsNum = jobContext->intermediateVectorsVec.size();

    while(jobContext->shuffleProgress.load() < jobContext->totalMapped.load()) {
        K2 *maxKey = findMaxKey(jobContext->intermediateVectorsVec);
        IntermediateVec maxKeyVec(0);
        for (size_t i = 0; i < vectorsNum; i++) {
            IntermediateVec* currVec = &(jobContext->intermediateVectorsVec.at(i));
            if (currVec->empty()) {
                continue;
            }
            K2 *currKey = currVec->back().first;
            while (!currVec->empty() && K2equality(currKey, maxKey)) {
                jobContext->shuffleProgress++;
                maxKeyVec.emplace_back(currVec->back());
                currVec->pop_back();
                jobContext->atomicState += (1 << CURRENT_ELEMENTS_BITS);
                if (!currVec->empty()) {
                    currKey = currVec->back().first;
                }
            }
        }
        jobContext->shuffledVec.emplace_back(maxKeyVec);
    }
}

/**
 * Handle the reduce section for each thread
 * @param threadContext the thread context
 */
void threadReduce(ThreadContext* threadContext) {
    auto *jobContext = (JobContext *)threadContext->job;
    size_t oldValue = jobContext->reduceProgress++;

    if (oldValue < jobContext->shuffledVec.size()) {
        IntermediateVec &intermediateVec = jobContext->shuffledVec.at(oldValue);
        jobContext->client.reduce(&intermediateVec, threadContext);
    }
}

/**
 * entry point for the threads
 * @param arg the thread context
 */
void *startThread(void *arg) {
    auto *threadContext = (ThreadContext *)arg;
    auto *jobContext = (JobContext *)threadContext->job;

    // Map
    if (pthread_mutex_lock(&jobContext->counterUpdateMutex)) {
        exitWithError(LOCK_ADD_OUTPUT_MUTEX_ERROR);
    }

    if ((jobContext->atomicState.load() & 1) != 1) {
        jobContext->atomicState = 1;
        jobContext->atomicState += (jobContext->inputVec.size() << TOTAL_ELEMENTS_BITS);
    }

    if (pthread_mutex_unlock(&jobContext->counterUpdateMutex)) {
        exitWithError(UNLOCK_ADD_OUTPUT_MUTEX_ERROR);
    }

    size_t totalElementsForMap = jobContext->inputVec.size();

    while (jobContext->mapProgress < totalElementsForMap) {
        threadMap(threadContext);
    }

    // Sort
    threadSort(threadContext);
    jobContext->barrier->barrier();

    // Shuffle
    if (threadContext->tid == 0) {
        jobContext->atomicState = 2;
        jobContext->atomicState += (jobContext->totalMapped.load() << TOTAL_ELEMENTS_BITS);

        threadShuffle(threadContext);
    }
    jobContext->barrier->barrier();


    // Reduce
    if (pthread_mutex_lock(&jobContext->counterUpdateMutex)) {
        exitWithError(LOCK_ADD_OUTPUT_MUTEX_ERROR);
    }

    if ((jobContext->atomicState.load() & 3) != 3) {
        jobContext->atomicState = 3;
        jobContext->atomicState += jobContext->shuffledVec.size() << TOTAL_ELEMENTS_BITS;
    }
    size_t totalElementsForReduce = jobContext->shuffledVec.size();
    if (pthread_mutex_unlock(&jobContext->counterUpdateMutex)) {
        exitWithError(UNLOCK_ADD_OUTPUT_MUTEX_ERROR);
    }
    while (jobContext->totalReduced.load() < totalElementsForReduce) {
        threadReduce(threadContext);
    }
    return nullptr;
}

/**
 * The main function of the file, handle the all operation
 * @param client object with map and reduce functions implemented, as well as all the elements
 * @param inputVec input vectors
 * @param outputVec output vectors to put the result into
 * @param multiThreadLevel number of thread
 * @return the handler of the service
 */
JobHandle startMapReduceJob(const MapReduceClient& client,
                            const InputVec& inputVec, OutputVec& outputVec,
                            int multiThreadLevel) {

    auto* jobContext = new JobContext{client, inputVec, outputVec, multiThreadLevel};

    for (int i = 0; i < multiThreadLevel; ++i) {
        jobContext->contexts[i] = {i, jobContext};
        jobContext->intermediateVectorsVec.push_back(IntermediateVec(0));
    }

    for (int i = 0; i < multiThreadLevel; i++) {
        if (pthread_create(jobContext->threads + i, nullptr, startThread,
                           jobContext->contexts + i)) {
            exitWithError(CREATE_PTHREAD_ERROR);
        }
    }

    return jobContext;
}

/**
 * Eliminate the threads
 * @param job the service handler
 */
void waitForJob(JobHandle job) {
    auto *jobContext = (JobContext *)job;

    if (pthread_mutex_lock(&jobContext->waitForJobMutex)) {
        exitWithError(LOCK_WAIT_MUTEX_ERROR);
    }

    if (!jobContext->waitForJobCalled) {
        jobContext->waitForJobCalled = true;
        for (int i = 0; i < jobContext->threadsCount; ++i) {
            pthread_join(jobContext->threads[i], nullptr);
        }
    }

    if (pthread_mutex_unlock(&jobContext->waitForJobMutex)) {
        exitWithError(UNLOCK_WAIT_MUTEX_ERROR);
    }
}

/**
 * Get the current status and progress of the service
 * @param job the job service
 * @param state and object to put the current state into
 */
void getJobState(JobHandle job, JobState *state) {
    auto *jobContext = (JobContext *)job;

    if (pthread_mutex_lock(&jobContext->counterUpdateMutex)) {
        exitWithError(LOCK_ADD_OUTPUT_MUTEX_ERROR);
    }

    uint64_t atomicValue = jobContext->atomicState.load();
    state->stage = (stage_t) (atomicValue & 3);
    state->percentage = 100 * (float) (atomicValue >> CURRENT_ELEMENTS_BITS & (0x7FFFFFFF)) /
                        (float) (atomicValue >> TOTAL_ELEMENTS_BITS);

    if (pthread_mutex_unlock(&jobContext->counterUpdateMutex)) {
        exitWithError(UNLOCK_ADD_OUTPUT_MUTEX_ERROR);
    }

}

/**
 * Eliminate the service
 * @param job the service handler
 */
void closeJobHandle(JobHandle job) {
    waitForJob(job);

    auto *jobContext = (JobContext *)job;

    if (pthread_mutex_destroy(&jobContext->waitForJobMutex)) {
        exitWithError(MUTEX_DESTROY_ERROR);
    }

    if (pthread_mutex_destroy(&jobContext->addToOutputMutex)) {
        exitWithError(MUTEX_DESTROY_ERROR);
    }

    if (pthread_mutex_destroy(&jobContext->counterUpdateMutex)) {
        exitWithError(MUTEX_DESTROY_ERROR);
    }

    delete jobContext->barrier;
    delete[] jobContext->threads;
    delete[] jobContext->contexts;
    delete jobContext;
}

