#include <stdio.h>
#include <thread>
#include "string.h"
#include "Arachne.h"
#include "Cycles.h"
#include "TimeTrace.h"

namespace Arachne {

using PerfUtils::Cycles;
using PerfUtils::TimeTrace;

enum InitializationState {
    NOT_INITIALIZED,
    INITIALIZING,
    INITIALIZED
};

void schedulerMainLoop();


InitializationState initializationState = NOT_INITIALIZED;
unsigned numCores = 1;

/**
 * Work to do on each thread. 
 * TODO(hq6): If vector is too slow, we may switch to a linked list.  */
std::vector<std::vector<UserContext*> > possiblyRunnableThreads;


/**
 * Threads that are sleeping and waiting.
 */
static std::vector<std::deque<UserContext*> > sleepQueues;

thread_local int kernelThreadId;
std::vector<std::deque<void*> > stackPool;

/**
  * This is the context that a given core is currently executing in.
 */
thread_local UserContext *running;

/**
  * This structure holds the flags, functions, and arguments for the main functions of user threads passed across cores.
  * TODO: In a NUMA world, it may make more sense if this is TaskBox** to allow
  * cores on different NUMA nodes to allocate from the memory that is closer to
  * them.
  */
TaskBox* taskBoxes;



/**
 * This function will allocate stacks and create kernel threads pinned to particular cores.
 *
 * Note that the setting of initializationState should probably use memory
 * barriers, but we are currently assuming that the application will only call
 * it from a single thread of execution.
 *
 * Calling the function twice is a no-op.
 */
void threadInit() {
    if (initializationState != NOT_INITIALIZED) {
        return;
    }
    initializationState = INITIALIZING;

    // Allocate stacks. Note that number of cores is actually number of
    // hyperthreaded cores, rather than necessarily real CPU cores.
    numCores = std::thread::hardware_concurrency(); 
//    printf("numCores = %u\n", numCores);

    // Special magic to ensure aligned allocation of taskBoxes
    int result = posix_memalign(reinterpret_cast<void**>(&taskBoxes),
            CACHE_LINE_SIZE, sizeof(TaskBox) * numCores);
    if (result != 0) {
        fprintf(stderr, "posix_memalign returned %s", strerror(result));
        exit(1);
    }
    assert((reinterpret_cast<uint64_t>(taskBoxes) & 0x3f) == 0);

    for (unsigned int i = 0; i < numCores; i++) {
        possiblyRunnableThreads.push_back(std::vector<UserContext*>());
        sleepQueues.push_back(std::deque<UserContext*>());

        // Initialize stack pool for each kernel thread
        stackPool.push_back(std::deque<void*>());
        for (int j = 0; j < stackPoolSize; j++)
            stackPool[i].push_back(malloc(stackSize));

        // Leave one thread for the main thread
        if (i != numCores - 1) {
            std::thread(threadMainFunction, i).detach();
        }

    }

    // Set the kernelThreadId for the main thread
    kernelThreadId = numCores - 1;

    initializationState = INITIALIZED;
}

/**
 * Main function for a kernel-level thread participating in the thread pool. 
 */
void threadMainFunction(int id) {
    // Switch to a user stack for symmetry, so that we can deallocate it ourselves later.
    // If we are the last user thread on this core, we will never return since we will simply poll for work.
    // If we are not the last, we will context switch out of this function and have our
    // stack deallocated by the thread we swap to (perhaps in the unblock
    // function), the next time Arachne gets control.
    // The original thread given by the kernel is simply discarded in the
    // current implementation.
    kernelThreadId = id;

    // This temporary context is used for bootstrapping the swap to a user stack.
    UserContext tempContext;
    running = &tempContext;

    createNewRunnableThread();
}

/**
 * When there is a need for a new runnable thread, either because there are
 * none, or because there are more tasks than current threads, this function is
 * invoked to allocate a new stack and switch to it on the current core.
 */
void createNewRunnableThread() {
    // First see if there is already an empty context among the current set of contexts.
    void** saved = &running->sp;
    auto& maybeRunnable = possiblyRunnableThreads[kernelThreadId];
    for (size_t i = 0; i < maybeRunnable.size(); i++)
        if (!maybeRunnable[i]->occupied) {
            running = maybeRunnable[i];
            running->wakeup = false;
            swapcontext(&running->sp, saved);
            return;
        }

    // Create a new context if there is not one available
    auto stack = stackPool[kernelThreadId].front();
    stackPool[kernelThreadId].pop_front();

    running = new UserContext;
    maybeRunnable.push_back(running);

    running->stack = stack;
    running->occupied = false;
    running->wakeup = false;


    // Set up the stack to return to the main thread function.
    running->sp = (char*) running->stack + stackSize - 64; 
    *(void**) running->sp = (void*) schedulerMainLoop;
    savecontext(&running->sp);

    // Switch to the new thread and start polling for work there.
    swapcontext(&running->sp, saved);
}

/**
 * Load a new context without saving anything.
 */
void  __attribute__ ((noinline))  setcontext(void **saved) {

    // Load the stack pointer and restore the registers
    asm("movq (%rdi), %rsp");

    asm("popq %rbp\n\t"
        "popq %rbx\n\t"
        "popq %r15\n\t"
        "popq %r14\n\t"
        "popq %r13\n\t"
        "popq %r12");
}

/**
 * Save a context of the currently executing process.
 */
void  __attribute__ ((noinline))  savecontext(void **target) {
    // Load the new stack pointer, push the registers, and then restore the old stack pointer.

    asm("movq %rsp, %r11\n\t"
        "movq (%rdi), %rsp\n\t"
        "pushq %r12\n\t"
        "pushq %r13\n\t"
        "pushq %r14\n\t"
        "pushq %r15\n\t"
        "pushq %rbx\n\t"
        "pushq %rbp\n\t"
        "movq  %rsp, (%rdi)\n\t"
        "movq %r11, %rsp"
        );
}

/**
 * Save one set of registers and load another set.
 * %rdi, %rsi are the two addresses of where stack pointers are stored.
 *
 * Load from saved and store into target.
 */
void  __attribute__ ((noinline))  swapcontext(void **saved, void **target) {

    // Save the registers and store the stack pointer
    asm("pushq %r12\n\t"
        "pushq %r13\n\t"
        "pushq %r14\n\t"
        "pushq %r15\n\t"
        "pushq %rbx\n\t"
        "pushq %rbp\n\t"
        "movq  %rsp, (%rsi)");

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
  * This function runs the scheduler in the context of the most recently run
  * thread, unless there is already a new thread creation request on this core.
  */
void schedulerMainLoop() {
    // At most one user thread on each core should be going through this loop
    // at any given time.  Most threads should be inside runThread, and only
    // re-enter the thread library by making an API call into Arachne.
    while (true) {
        // Poll for work on my taskBox and take it off first so that we avoid
        // blocking other create requests onto our core.
        if (taskBoxes[kernelThreadId].data.loadState.load() == FILLED) {
            // Take on a new task directly if this is an empty context.
            if (!running->occupied) {
                // Copy the task onto the local stack
                auto task = taskBoxes[kernelThreadId].getTask();

                auto expectedTaskState = FILLED; // Because of compare_exchange_strong requires a reference
                taskBoxes[kernelThreadId].data.loadState.compare_exchange_strong(expectedTaskState, EMPTY);
                running->occupied = true;
                reinterpret_cast<TaskBase*>(&task)->runThread();
                running->occupied = false;
            } else { // Create a new context since this is a blocked context.
                createNewRunnableThread();
            }
        }
        
        checkSleepQueue();
        {
            // If we see no other runnable user threads, then we are the last
            // runnable thread and should continue running.
            auto& maybeRunnable = possiblyRunnableThreads[kernelThreadId];
            for (size_t i = 0; i < maybeRunnable.size(); i++) {
                if (maybeRunnable[i]->wakeup) {
                    TimeTrace::record("Detected new runnable thread in scheduler main loop");
                    // If the blocked context is our own, we can simply return after clearing our own wake-up flag.
                    if (maybeRunnable[i] == running) {
                        TimeTrace::record("About to set wakeup flag and return immediately");
                        running->wakeup = false;
                        return;
                    }
                    // It is assumed that an empty context will not have the
                    // wakeup flag set, so this must be a valid thread to
                    // switch to.
                    void** saved = &running->sp;
                    running = maybeRunnable[i];
                    TimeTrace::record("Assigned running.");

                    swapcontext(&running->sp, saved);
                }
            }
        }
    }
}

/**
 * Restore control back to the thread library.
 * Assume we are the running process in the current kernel thread if we are calling
 * yield.
 *
 * TODO: Start from a different point in the list every time to avoid two
 * runnable threads from starving all later ones on the list.
 */
void yield() {

    // Poll for incoming task.
    auto& maybeRunnable = possiblyRunnableThreads[kernelThreadId];
    if (taskBoxes[kernelThreadId].data.loadState.load() == FILLED) {
        maybeRunnable.push_back(running);
        createNewRunnableThread();
    }
    checkSleepQueue();

    for (size_t i = 0; i < maybeRunnable.size(); i++) {
        if (maybeRunnable[i]->wakeup && maybeRunnable[i] != running) {
            void** saved = &running->sp;

            // This thread is still runnable since it is merely yielding.
            running->wakeup = true; 
            running = maybeRunnable[i];
            running->wakeup = false;
            swapcontext(&running->sp, saved);
            return;
        }
    }
}
void checkSleepQueue() {
    uint64_t currentCycles = Cycles::rdtsc();
    auto& sleepQueue = sleepQueues[kernelThreadId];

    // Assume sorted and move it off the list
    while (sleepQueue.size() > 0 && sleepQueue[0]->wakeUpTimeInCycles < currentCycles) {
        // Move onto the ready queue
        sleepQueue[0]->wakeup = true;
        sleepQueue.pop_front();  
    }
}

// Sleep for at least the argument number of ns.
// We keep in core-resident to avoid cross-core cache coherency concerns.
// It must be the case that this function returns after at least ns have
// passed.
void sleep(uint64_t ns) {
    running->wakeUpTimeInCycles = Cycles::rdtsc() + Cycles::fromNanoseconds(ns);

    auto& sleepQueue = sleepQueues[kernelThreadId];
    if (sleepQueue.size() == 0) {
        sleepQueue.push_back(running);
    }
    else {
        auto it = sleepQueue.begin();
        for (; it != sleepQueue.end() ; it++)
            if ((*it)->wakeUpTimeInCycles > running->wakeUpTimeInCycles) {
                sleepQueue.insert(it, running);
                break;
            }
        // Insert now
        if (it == sleepQueue.end()) {
            sleepQueue.push_back(running);
        }
    }

    // Poll for incoming task.
    if (taskBoxes[kernelThreadId].data.loadState.load() == FILLED) {
        createNewRunnableThread();
    }
        
    // TODO: Decide if this is necessary here.
    checkSleepQueue();

    auto& maybeRunnable = possiblyRunnableThreads[kernelThreadId];
    for (size_t i = 0; i < maybeRunnable.size(); i++) {
        // There are other runnable threads, so we simply switch to the first one.
        if (maybeRunnable[i]->wakeup) {
            void** saved = &running->sp;
            running = maybeRunnable[i];
            maybeRunnable[i]->wakeup = false;
            swapcontext(&running->sp, saved);
            return;
        }
    }

    // Run the main scheduler loop in the context of this thread, since this is
    // the last thread that was runnable.
    schedulerMainLoop();
}

/**
  * Returns a thread handle to the application, which can be passed into the signal function.
  */
ThreadId getThreadId() {
    return running;
}

/**
  * Deschedule the current thread until the application signals using the a
  * ThreadId.
  */
void block() {
    auto& maybeRunnable = possiblyRunnableThreads[kernelThreadId];

    // Poll for incoming task.
    if (taskBoxes[kernelThreadId].data.loadState.load() == FILLED) {
        maybeRunnable.push_back(running);
        createNewRunnableThread();
        return;
    }

    // Find a thread to switch to
    for (size_t i = 0; i < maybeRunnable.size(); i++) {
        if (maybeRunnable[i]->wakeup) {
            void** saved = &running->sp;
            running = maybeRunnable[i];
            running->wakeup = false;
            swapcontext(&running->sp, saved);
            return;
        }
    }

    // Run the main scheduler loop in the context of this thread, since this is
    // the last thread that was runnable.
    schedulerMainLoop();
}

/* Make the thread referred to by ThreadId runnable once again. */
void signal(ThreadId id) {
    id->wakeup = true;
}


/**
 * This is a special function to allow the main thread to join the thread pool
 * after seeding initial tasks for itself and possibly other threads.
 *
 * The other way of implementing this is to merge this function with the
 * threadInit, and ask the user to provide an initial task to run in the main
 * thread, which will presumably spawn other tasks.
 */
void mainThreadJoinPool() {
    threadMainFunction(numCores - 1);
}

}
