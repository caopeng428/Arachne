/* Copyright (c) 2015-2016 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "Arachne.h"
#include <stdio.h>
#include <string.h>
#include <thread>
#include "../PerfUtils/TimeTrace.h"
#include "../PerfUtils/Util.h"

namespace Arachne {

// Change 0 -> 1 in the following line to compile detailed time tracing in
// this file.
#define TIME_TRACE 0

using PerfUtils::Cycles;
using PerfUtils::TimeTrace;

/**
  * This variable prevents multiple initializations of the library, but does
  * not protect against the user calling Arachne functions without initializing
  * the library, which results in undefined behavior.
  */
bool initialized = false;

// Used to allow redirection of error messages generated by the library.
FILE* errorStream = stderr;

std::function<void()> initCore = nullptr;

// The following configuration options can be passed into init.

/**
  * The degree of parallelism between Arachne threads. If this is set higher
  * than the number of physical cores, the kernel will multiplex, which is
  * usually undesirable except when running unit tests on a single-core
  * system.
  */
volatile uint32_t numCores = 0;

/**
  * Since numCores is used during thread creation to select a core, it is not
  * safe to increment its value until state has been set up for a new core,
  * which happens asynchronously in a new kernel thread.
  *
  * However, there must be a way to prevent multiple threads from
  * simultaneously attempting to scale up the number of cores before numCores
  * is incremented. numCoresPrecursor serves this purpose, and represents the
  * future value of numCores;
  */
volatile uint32_t numCoresPrecursor;

/**
  * The largest number of cores that Arachne is permitted to utilize.
  * It is an invariant that maxNumCores >= numCores, but if the user explicitly
  * sets both then numCores will push maxNumCores up to satisfy the invariant.
  */
volatile uint32_t maxNumCores = 0;

/**
  * Protect state related to changes in the number of cores, and prevents
  * multiple threads from simultaneously attempting to change the number of
  * cores.
  */
SpinLock coreChangeMutex(false);

/**
  * Configurable maximum stack size for all threads.
  */
int stackSize = 1024 * 1024;

/**
  * Keep track of the kernel threads we are running so that we can join them on
  * destruction. Also, store a pointer to the original stacks to facilitate
  * switching back and joining.
  */
std::vector<std::thread> kernelThreads;
std::vector<void*> kernelThreadStacks;

/**
  * Alert the kernel threads that they should exit if there are no further
  * threads to run.
  */
volatile bool shutdown;

/**
  * The collection of possibly runnable contexts for each kernel thread.
  */
std::vector<ThreadContext**> allThreadContexts;

/**
  * This pointer allows fast access to the current kernel thread's
  * localThreadContexts without computing an offset from the global
  * allThreadContexts vector on each access.
  */
thread_local ThreadContext** localThreadContexts;

/**
  * Holds the identifier for the thread in which it is stored: allows each
  * kernel thread to identify itself. This should eventually become a coreId,
  * when we support multiple kernel threads per core to handle blocking system
  * calls.
  */
thread_local int kernelThreadId;

/**
  * This is the context that a given kernel thread is currently executing.
  */
thread_local ThreadContext *loadedContext;

/**
  * See documentation for MaskAndCount.
  */
std::vector<std::atomic<MaskAndCount> * > occupiedAndCount;
thread_local std::atomic<MaskAndCount> *localOccupiedAndCount;

/**
  * Setting a jth bit in the ith element of this vector indicates that the
  * priority of the thread living at index j on core i is temporarily raised.
  */
std::vector< std::atomic<uint64_t> *> publicPriorityMasks;

/**
  * This represents each core's local copy of the high-priority mask. Each call
  * to dispatch() will first examine this bitmask. It will clear the first set
  * bit and switch to that context. If there are no set bits, it will copy the
  * current value of publicPriorityMasks for the current core to here, and then
  * atomically clear those bits using an atomic OR.
  *
  * When ramping down cores, this value (if nonzero) should be cleared, since
  * all non-terminated threads on this core will be migrated away from this
  * thread.
  */
thread_local uint64_t privatePriorityMask;

/**
  * This variable holds the index into the current kernel thread's
  * localThreadContexts that it will check first the next time it looks for a
  * thread to run. It is used to implement round-robin scheduling of Arachne
  * threads.
  */
thread_local size_t nextCandidateIndex = 0;

/**
  * This quantity is used in the current heuristic for deciding that we need to
  * scale up the number of cores.
  * If we manage to find a runnable thread after less than this many iterations
  * through the loop, then we should attempt to increase the number of cores.
  */
uint64_t CORE_INCREASE_THRESHOLD = 3;
void incrementCoreCount();

/**
  * Allocate a block of memory aligned at the beginning of a cache line.
  *
  * \param size
  *     The amount of memory to allocate.
  */
void*
cacheAlignAlloc(size_t size) {
    void *temp;
    int result = posix_memalign(&temp, CACHE_LINE_SIZE, size);
    if (result != 0) {
        fprintf(errorStream, "posix_memalign returned %s", strerror(result));
        exit(1);
    }
    assert((reinterpret_cast<uint64_t>(temp) & (CACHE_LINE_SIZE - 1)) == 0);
    return temp;
}

/**
 * Main function for a kernel thread, which roughly corresponds to a core in the
 * current design of the system.
 * 
 * \param kId
 *     The kernel thread ID for the newly created kernel thread.
 */
void
threadMain(int kId) {
    PerfUtils::Util::pinAvailableCore();
    if (initCore) initCore();
    kernelThreadId = kId;
    localOccupiedAndCount = occupiedAndCount[kernelThreadId];
    localThreadContexts = allThreadContexts[kernelThreadId];

    loadedContext = localThreadContexts[0];

    // Transfers control to the Arachne dispatcher.
    // This context has been pre-initialized by init so it will "return"
    // to the schedulerMainLoop.
    // This call will return iff shutDown is called from the main thread.
    swapcontext(&loadedContext->sp, &kernelThreadStacks[kId]);
}

/**
  * Save the current register values onto one stack and load fresh register
  * values from another stack.
  * This method does not return to its caller immediately. It returns to the
  * caller when another thread on the same kernel thread invokes this method
  * with the current value of target as the saved parameter.
  *
  * \param saved
  *     Address of the stack location to load register values from.
  * \param target
  *     Address of the stack location to save register values to.
  */
void __attribute__((noinline))
swapcontext(void **saved, void **target) {
    // This code depends on knowledge of the compiler's calling convention: rdi
    // and rsi are the first two arguments.
    // Alternative approaches tend to run into conflicts with compiler register
    // use.

    // Save the registers that the compiler expects to persist across method
    // calls and store the stack pointer's location after saving these
    // registers.
    // NB: The space used by the pushed and
    // popped registers must equal the value of SpaceForSavedRegisters, which
    // should be updated atomically with this assembly.
    asm("pushq %r12\n\t"
        "pushq %r13\n\t"
        "pushq %r14\n\t"
        "pushq %r15\n\t"
        "pushq %rbx\n\t"
        "pushq %rbp\n\t"
        "movq %rsp, (%rsi)");

    // Load the stack pointer and restore the registers
    asm("movq (%rdi), %rsp\n\t"
        "popq %rbp\n\t"
        "popq %rbx\n\t"
        "popq %r15\n\t"
        "popq %r14\n\t"
        "popq %r13\n\t"
        "popq %r12");
}

/**
  * This is the top level method executed by each thread context. It is never
  * directly invoked. Instead, the thread's context is set up to "return" to
  * this method when we context switch to it the first time.
  */
void
schedulerMainLoop() {
    while (true) {
        // No thread to execute yet. This call will not return until we have
        // been assigned a new Arachne thread.
        dispatch();
        reinterpret_cast<ThreadInvocationEnabler*>(
                &loadedContext->threadInvocation)->runThread();
        // The thread has exited.
        // Cancel any wakeups the thread may have scheduled for itself before
        // exiting.
        loadedContext->wakeupTimeInCycles = UNOCCUPIED;

        // Bump the generation number for the next newborn thread.
        loadedContext->generation++;
        {
            // Handle joins
            std::lock_guard<SpinLock> joinGuard(loadedContext->joinLock);
            loadedContext->joinCV.notifyAll();
        }

        // The code below clears the occupied flag for the current
        // ThreadContext.
        //
        // While this logically comes before dispatch(), it is here to prevent
        // it from racing against thread creations that come before the start
        // of the outer loop, since the occupied flags for such creations would
        // get wiped out by this code.
        bool success;
        do {
            MaskAndCount slotMap = *localOccupiedAndCount;
            MaskAndCount oldSlotMap = slotMap;

            slotMap.numOccupied--;

            slotMap.occupied &=
                ~(1L << loadedContext->idInCore) & 0x00FFFFFFFFFFFFFF;
            success = localOccupiedAndCount->compare_exchange_strong(
                    oldSlotMap,
                    slotMap);
        } while (!success);

        // Newborn threads should not have elevated priority, even if the
        // predecessors had leftover priority
        privatePriorityMask &= ~(1L << (loadedContext->idInCore));
        *publicPriorityMasks[kernelThreadId] &= ~(1L << (loadedContext->idInCore));
    }
}

/**
  * This method is used as part of cooperative multithreading to give other
  * Arachne threads on the same core a chance to run.
  * It will return when all other threads have had a chance to run.
  */
void
yield() {
    if (!loadedContext) return;
    if (localOccupiedAndCount->load().numOccupied == 1) return;
    // This thread is still runnable since it is merely yielding.
    loadedContext->wakeupTimeInCycles = 0L;
    dispatch();
}

/**
  * Sleep for at least ns nanoseconds. The amount of additional delay may be
  * impacted by other threads' activities such as blocking and yielding.
  */
void
sleep(uint64_t ns) {
    loadedContext->wakeupTimeInCycles =
        Cycles::rdtsc() + Cycles::fromNanoseconds(ns);
    dispatch();
}

/**
  * Return a thread handle for the currently executing thread, identical to the
  * one returned by the createThread call that initially created this thread.
  *
  * When invoked from a non-Arachne thread, this function returns
  * Arachne::NullThread.
  */
ThreadId
getThreadId() {
    return loadedContext ? ThreadId(loadedContext, loadedContext->generation)
        : Arachne::NullThread;
}

/**
  * Deschedule the current thread until its wakeup time is reached (which may
  * have already happened) and find another thread to run. All direct and
  * indirect callers of this function must ensure that spurious wakeups are
  * safe.
  */
void
dispatch() {
    // Check the stack canary on the current context.
    if (*reinterpret_cast<uint64_t*>(loadedContext->stack) != StackCanary) {
        fprintf(errorStream, "Stack overflow detected on %p. Aborting...\n",
                loadedContext);
        fflush(errorStream);
        abort();
    }
    uint64_t currentCycles = Cycles::rdtsc();
    uint64_t mask = localOccupiedAndCount->load().occupied;

    // Check for high priority threads.
    if (!privatePriorityMask) {
        // Copy & paste from the public list.
        privatePriorityMask = *publicPriorityMasks[kernelThreadId];
        if (privatePriorityMask)
            *publicPriorityMasks[kernelThreadId] &= ~privatePriorityMask;
    }

    if (privatePriorityMask) {
        // This position is one-indexed with zero meaning that no bits were
        // set.
        int firstSetBit = ffsll(privatePriorityMask);
        if (firstSetBit) {
            firstSetBit--;
            privatePriorityMask &= ~(1L << (firstSetBit));

            ThreadContext* targetContext = localThreadContexts[firstSetBit];

            // Verify wakeup and occupied.
            if (targetContext->wakeupTimeInCycles == 0 &&
                    ((mask >> firstSetBit) & 1)) {
                if (targetContext == loadedContext) {
                    loadedContext->wakeupTimeInCycles = BLOCKED;
                    return;
                }
                void** saved = &loadedContext->sp;
                loadedContext = targetContext;
                swapcontext(&loadedContext->sp, saved);
                loadedContext->wakeupTimeInCycles = BLOCKED;
                return;
            }
        }
    }
    // Find a thread to switch to
    size_t currentIndex = nextCandidateIndex;
    mask >>= currentIndex;

    // Count the iterations it took us to find a runnable thread.
    // Heuristically, if this number is very small, then we may want to ramp up
    // the number of cores.
    uint64_t numIterations = 0;
    for (;;currentIndex++, mask >>= 1L, numIterations++) {
        if (mask == 0) {
            // We have reached the end of the threads, so we should go back to
            // the beginning.
            currentIndex = 0;
            mask = localOccupiedAndCount->load().occupied;
            currentCycles = Cycles::rdtsc();

            // Check for termination
            if (shutdown)
                swapcontext(
                        &kernelThreadStacks[kernelThreadId],
                        &loadedContext->sp);
        }
        // Optimize to eliminate unoccupied contexts
        if (!(mask & 1))
            continue;

        ThreadContext* currentContext = localThreadContexts[currentIndex];
        if (currentCycles >= currentContext->wakeupTimeInCycles) {
            if (numIterations < CORE_INCREASE_THRESHOLD &&
                    numCoresPrecursor < maxNumCores)
                incrementCoreCount();
            nextCandidateIndex = currentIndex + 1;
            if (nextCandidateIndex == maxThreadsPerCore) nextCandidateIndex = 0;

            if (currentContext == loadedContext) {
                loadedContext->wakeupTimeInCycles = BLOCKED;
                return;
            }
            void** saved = &loadedContext->sp;
            loadedContext = currentContext;
            swapcontext(&loadedContext->sp, saved);
            // After the old context is swapped out above, this line executes
            // in the new context.
            loadedContext->wakeupTimeInCycles = BLOCKED;
            return;
        }
    }
}

/**
  * Make the thread referred to by ThreadId runnable.
  * If one thread exits and another is created between the check and the setting
  * of the wakeup flag, this signal will result in a spurious wake-up.
  * If this method is invoked on a currently running thread, it will have the
  * effect of causing the thread to immediately unblock the next time it blocks.
  *
  * \param id
  *     The id of the thread to signal.
  */
void
signal(ThreadId id) {
    uint64_t oldWakeupTime = id.context->wakeupTimeInCycles;
    if (oldWakeupTime != UNOCCUPIED) {
        // We do the CAS in assembly because we do not want to pay for the
        // extra memory fences for ordinary stores that std::atomic adds.
        uint64_t newValue = 0L;
        __asm__ __volatile__("lock; cmpxchgq %0,%1" : "=r" (newValue), 
                "=m" (id.context->wakeupTimeInCycles),
                "=a" (oldWakeupTime) : "0" (newValue), "2" (oldWakeupTime));

        // Raise the priority of the newly awakened thread.
        if (id.context->coreId != static_cast<uint8_t>(~0))
            *publicPriorityMasks[id.context->coreId] |=
                (1L << id.context->idInCore);
    }
}

/**
  * Block the current thread until the thread identified by id finishes its
  * execution.
  *
  * \param id
  *     The id of the thread to join.
  */
void
join(ThreadId id) {
    std::lock_guard<SpinLock> joinGuard(id.context->joinLock);
    // Thread has already exited.
    if (id.generation != id.context->generation) return;
    id.context->joinCV.wait(id.context->joinLock);
}

/**
  * This function must be called by the main application thread and will block
  * until Arachne is terminated via a call to shutDown().
  *
  * Upon termination, this function tears down all state created by init,
  * and restores the state of the system to the time before init is
  * called.
  */
void waitForTermination() {
    for (size_t i = 0; i < kernelThreads.size(); i++) {
        kernelThreads[i].join();
    }
    kernelThreads.clear();
    kernelThreadStacks.clear();

    // We now assume that all threads are done executing.
    PerfUtils::Util::serialize();

    for (size_t i = 0; i < numCores; i++) {
        for (int k = 0; k < maxThreadsPerCore; k++) {
            free(allThreadContexts[i][k]->stack);
            allThreadContexts[i][k]->joinLock.~SpinLock();
            allThreadContexts[i][k]->joinCV.~ConditionVariable();
            free(allThreadContexts[i][k]);
        }
        delete[] allThreadContexts[i];
        free(occupiedAndCount[i]);
    }
    allThreadContexts.clear();
    occupiedAndCount.clear();
    PerfUtils::Util::serialize();
    initialized = false;
}

/**
  * This function parses out the arguments intended for the thread library from
  * a command line, and adjusts the values of argc and argv to eliminate the
  * arguments that the thread library consumed.
  */
void
parseOptions(int* argcp, const char** argv) {
    if (argcp == NULL) return;

    int argc = *argcp;

    struct OptionSpecifier {
        // The string that the user uses after `--`.
        const char* optionName;
        // The id for the option that is returned when it is recognized.
        int id;
        // Does the option take an argument?
        bool takesArgument;
    } optionSpecifiers[] = {
        {"numCores", 'c', true},
        {"maxNumCores", 'm', true},
        {"stackSize", 's', true}
    };
    const int UNRECOGNIZED = ~0;

    int i = 1;
    while (i < argc) {
        if (argv[i][0] != '-' || argv[i][1] != '-') {
            i++;
            continue;
        }
        const char* optionName = argv[i] + 2;
        int optionId = UNRECOGNIZED;
        const char* optionArgument = NULL;

        for (size_t k = 0;
                k < sizeof(optionSpecifiers) / sizeof(OptionSpecifier); k++) {
            const char* candidateName = optionSpecifiers[k].optionName;
            bool needsArg = optionSpecifiers[k].takesArgument;
            if (strncmp(candidateName,
                        optionName, strlen(candidateName)) == 0) {
                if (needsArg) {
                    if (i + 1 >= argc) {
                        fprintf(errorStream,
                                "Missing argument to option %s!\n",
                                candidateName);
                        break;
                    }
                    optionArgument = argv[i+1];
                    optionId = optionSpecifiers[k].id;
                    argc -= 2;
                    memmove(argv + i, argv + i + 2, (argc - i) * sizeof(char*));
                } else {
                    optionId = optionSpecifiers[k].id;
                    argc -= 1;
                    memmove(argv + i, argv + i + 1, (argc - i) * sizeof(char*));
                }
                break;
            }
        }
        switch (optionId) {
            case 'c':
                numCores = atoi(optionArgument);
                break;
            case 'm':
                maxNumCores = atoi(optionArgument);
                break;
            case 's':
                stackSize = atoi(optionArgument);
                break;
            case UNRECOGNIZED:
                i++;
        }
    }
    *argcp = argc;
}

ThreadContext::ThreadContext(uint8_t coreId, uint8_t idInCore)
    : stack(malloc(stackSize))
    , sp(reinterpret_cast<char*>(stack) +
            stackSize - 2*sizeof(void*))
    , wakeupTimeInCycles(UNOCCUPIED)
    , generation(1)
    , joinLock()
    , joinCV()
    , coreId(coreId)
    , idInCore(idInCore)
{
    // Immediately before schedulerMainLoop gains control, we want the
    // stack to look like this, so that the swapcontext call will
    // transfer control to schedulerMainLoop.
    //           +-----------------------+
    //           |                       |
    //           +-----------------------+
    //           |     Return Address    |
    //           +-----------------------+
    //     sp->  |       Registers       |
    //           +-----------------------+
    //           |                       |
    //           |                       |
    //
    // Set up the stack so that the first time we switch context to
    // this thread, we enter schedulerMainLoop.
    *reinterpret_cast<void**>(sp) = reinterpret_cast<void*>(schedulerMainLoop);

    /**
     * Decrement the stack pointer by the amount of space needed to
     * store the registers in swapcontext.
     */
    sp = reinterpret_cast<char*>(sp) - SpaceForSavedRegisters;

    /**
     * Set the stack canary value to detect stack overflows.
     */
    *reinterpret_cast<uint64_t*>(stack) = StackCanary;
}

/**
 * This function sets up state needed by the thread library, and must be
 * invoked before any other function in the thread library is invoked. It is
 * undefined behavior to invoke other Arachne functions before this one.
 *
 * Arachne will take configuration options from the command line specified by
 * argc and argv, and then update the values of argv and argc to reflect the
 * remaining arguments.
 *
 * Here are the current available options.
 *
 *     --numCores
 *        The starting number of cores the application should use.
 *     --maxNumCores
 *        The largest number of core the appliation may use
 *     --stackSize
 *        The size of each user stack.
 *
 * \param argcp
 *    The pointer to argc, the number of arguments passed to the application.
 *    This pointer will be used to update argc after Arachne has consumed its
 *    arguments.
 * \param argv
 *    The pointer to the command line argument array, which will be modiifed to
 *    remove the options that Arachne recognizes.
 */
void
init(int* argcp, const char** argv) {
    if (initialized)
        return;
    initialized = true;
    parseOptions(argcp, argv);

    if (numCores == 0)
        numCores = std::thread::hardware_concurrency();
    numCoresPrecursor = numCores;
    maxNumCores = std::max(numCores, maxNumCores);

    for (unsigned int i = 0; i < numCores; i++) {
        occupiedAndCount.push_back(
                reinterpret_cast<std::atomic<Arachne::MaskAndCount>* >(
                    cacheAlignAlloc(sizeof(MaskAndCount))));
        memset(occupiedAndCount.back(), 0, sizeof(std::atomic<MaskAndCount>));
        publicPriorityMasks.push_back(
                reinterpret_cast< std::atomic<uint64_t>* >(
                    cacheAlignAlloc(sizeof(std::atomic<uint64_t>))));
        memset(publicPriorityMasks.back(), 0, sizeof(std::atomic<uint64_t>));
        // Here we will allocate all the thread contexts and stacks
        ThreadContext **contexts = new ThreadContext*[maxThreadsPerCore];
        for (uint8_t k = 0; k < maxThreadsPerCore; k++) {
            contexts[k] = reinterpret_cast<ThreadContext*>(
                    cacheAlignAlloc(sizeof(ThreadContext)));
            new (contexts[k]) ThreadContext(static_cast<uint8_t>(i), k);
        }
        allThreadContexts.push_back(contexts);
    }

    // Allocate space to store all the original kernel pointers
    kernelThreadStacks.resize(numCores);
    shutdown = false;

    // Ensure that data structure and stack allocation completes before we
    // begin to use it in a new thread.
    PerfUtils::Util::serialize();

    // Note that the main thread is not part of the thread pool.
    for (unsigned int i = 0; i < numCores; i++) {
        // These threads are started with threadMain instead of
        // schedulerMainLoop because we want schedulerMainLoop to run on a user
        // stack rather than a kernel-provided stack
        kernelThreads.emplace_back(threadMain, i);
    }
}

/**
  * This function should be invoked in unit test setup to make Arachne
  * functions callable from the unit test, which is not an Arachne thread.
  *
  * This function sets up just enough state to allow the current thread to
  * execute unit tests which call Arachne functions.
  * We assume that the unit tests are run from the main kernel thread which
  * will never swap out when running the dispatch() loop.
  */
void
testInit() {
    kernelThreadId = numCores;
    localOccupiedAndCount =
        reinterpret_cast<std::atomic<Arachne::MaskAndCount>* >(
            cacheAlignAlloc(sizeof(MaskAndCount)));
    memset(localOccupiedAndCount, 0, sizeof(MaskAndCount));

    localThreadContexts = new ThreadContext*[maxThreadsPerCore];
    for (uint8_t k = 0; k < maxThreadsPerCore; k++) {
        // Technically, this allocates a bunch of user stacks which will never
        // be used, and it can be optimized out if it turns out to be too
        // expensive.
        localThreadContexts[k] = reinterpret_cast<ThreadContext*>(
                cacheAlignAlloc(sizeof(ThreadContext)));
        new (localThreadContexts[k]) ThreadContext(~0, k);
        localThreadContexts[k]->wakeupTimeInCycles = BLOCKED;
    }
    loadedContext = *localThreadContexts;
    *localOccupiedAndCount = {1, 1};
}

/**
  * This function should be invoked in unit test teardown to clean up the state
  * that makes Arachne functions callable from the unit test.
  */
void testDestroy() {
    free(localOccupiedAndCount);
    for (int k = 0; k < maxThreadsPerCore; k++) {
        free(localThreadContexts[k]->stack);
        localThreadContexts[k]->joinLock.~SpinLock();
        localThreadContexts[k]->joinCV.~ConditionVariable();

        free(localThreadContexts[k]);
    }
    delete[] localThreadContexts;
    loadedContext = NULL;
    *localOccupiedAndCount = {0, 0};
}

/**
  * This call will cause all Arachne threads to terminate, and cause
  * waitForTermination() to return.
  *
  * It is typically used only for an application's unit tests, where the global
  * teardown function in the unit test would call Arachne::shutDown() followed
  * immediately by Arachne::waitForTermination().
  *
  * This function can be called from any Arachne or non-Arachne thread.
  */
void
shutDown() {
    // Tell all the kernel threads to terminate at the first opportunity.
    shutdown = true;
}


/** Attempt to acquire this resource and block if it is not available. */
void
SleepLock::lock() {
    std::unique_lock<SpinLock> guard(blockedThreadsLock);
    if (owner == NULL) {
        owner = loadedContext;
        return;
    }
    blockedThreads.push_back(getThreadId());
    guard.unlock();
    do {
        // Spurious wake-ups can happen due to signalers of past inhabitants of
        // this loadedContext.
        dispatch();
    } while(owner != loadedContext);
}

/** 
 * Attempt to acquire this resource once.
 * \return
 *    Whether or not the acquisition succeeded.
 */
bool
SleepLock::try_lock() {
    std::lock_guard<SpinLock> guard(blockedThreadsLock);
    if (owner == NULL) {
        owner = loadedContext;
        return true;
    }
    return false;
}

/** Release resource. */
void
SleepLock::unlock() {
    std::lock_guard<SpinLock> guard(blockedThreadsLock);
    if (blockedThreads.empty()) {
        owner = NULL;
        return;
    }
    owner = blockedThreads.front().context;
    signal(blockedThreads.front());
    blockedThreads.pop_front();
}

ConditionVariable::ConditionVariable()
    : blockedThreads() {}

ConditionVariable::~ConditionVariable() { }

/**
  * Awaken one of the threads waiting on this condition variable.
  * The caller must hold the mutex that waiting threads held when they called
  * wait().
  */
void
ConditionVariable::notifyOne() {
    if (blockedThreads.empty()) return;
    ThreadId awakenedThread = blockedThreads.front();
    blockedThreads.pop_front();
    signal(awakenedThread);
}

/**
  * Awaken all of the threads waiting on this condition variable.
  * The caller must hold the mutex that waiting threads held when they called
  * wait().
  */
void
ConditionVariable::notifyAll() {
    while (!blockedThreads.empty())
        notifyOne();
}

/**
  * Change the target of the error stream, allowing redirection to an
  * application's log.
  */
void setErrorStream(FILE* stream) {
    errorStream = stream;
}

/**
  * When Arachne needs to scale up its number of cores, this function should be
  * invoked from the new kernel thread.
  */
void joinKernelThreadPool() {
    PerfUtils::Util::pinAvailableCore();
    if (initCore) initCore();
    // Allocate data structures, assign them to thread-local variable, and then
    localOccupiedAndCount =
        reinterpret_cast<std::atomic<Arachne::MaskAndCount>* >(
            cacheAlignAlloc(sizeof(MaskAndCount)));

    memset(localOccupiedAndCount, 0, sizeof(MaskAndCount));

    localThreadContexts = new ThreadContext*[maxThreadsPerCore];
    for (uint8_t k = 0; k < maxThreadsPerCore; k++) {
        // Technically, this allocates a bunch of user stacks which will never
        // be used, and it can be optimized out if it turns out to be too
        // expensive.
        localThreadContexts[k] = reinterpret_cast<ThreadContext*>(
                cacheAlignAlloc(sizeof(ThreadContext)));
        new (localThreadContexts[k]) ThreadContext(~0, k);
    }

    // Ensure that the memory above is properly allocated; prevent pipelining.
    PerfUtils::Util::serialize();

    // Take a mutex to exclude other threads from simultaneously trying to
    // change the number of cores.
    coreChangeMutex.lock();
    occupiedAndCount.push_back(localOccupiedAndCount);
    allThreadContexts.push_back(localThreadContexts);
    publicPriorityMasks.push_back(
                reinterpret_cast< std::atomic<uint64_t>* >(
                    cacheAlignAlloc(sizeof(std::atomic<uint64_t>))));
    kernelThreadStacks.push_back(NULL);
    kernelThreadId = numCores++;
    coreChangeMutex.unlock();

    // Since we know the kernelThreadId now, we can now correct the
    // ThreadContext.coreId() here. Eventually, this correction will take place
    // every time a core is returned to the application.
    for (uint8_t k = 0; k < maxThreadsPerCore; k++) {
        localThreadContexts[k]->coreId = static_cast<uint8_t>(kernelThreadId);
    }

    // See documentation in threadMain
    loadedContext = localThreadContexts[0];
    swapcontext(&loadedContext->sp, &kernelThreadStacks[kernelThreadId]);
}

/**
  * This function can be called from any thread to increase the number of cores
  * used by Arachne.
  */
void incrementCoreCount() {
    std::lock_guard<SpinLock> _(coreChangeMutex);
    if (numCoresPrecursor < maxNumCores) {
        fprintf(errorStream, "Number of cores increasing from %u to %u\n",
                numCoresPrecursor, numCoresPrecursor + 1);
        fflush(errorStream);
        kernelThreads.emplace_back(joinKernelThreadPool);
        numCoresPrecursor++;
    }
}
} // namespace Arachne
