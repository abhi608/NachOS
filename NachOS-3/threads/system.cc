// system.cc 
//	Nachos initialization and cleanup routines.
//
// Copyright (c) 1992-1993 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation 
// of liability and disclaimer of warranty provisions.

#include "copyright.h"
#include "system.h"

// This defines *all* of the global data structures used by Nachos.
// These are all initialized and de-allocated by this file.

NachOSThread *currentThread;			// the thread we are running now
NachOSThread *threadToBeDestroyed;  		// the thread that just finished
NachOSscheduler *scheduler;			// the ready list
Interrupt *interrupt;			// interrupt status
Statistics *stats;			// performance metrics
Timer *timer;				// the hardware timer device,
					// for invoking context switches



List *freed_pages;           //list of pages freed by syscall exec
// TranslationEntry *page_entries[NumPhysPages];   //hash-map of page entries

unsigned numPagesAllocated;              // number of physical frames allocated
unsigned next_unallocated_page;

NachOSThread *threadArray[MAX_THREAD_COUNT];  // Array of thread pointers
unsigned thread_index;			// Index into this array (also used to assign unique pid)
bool initializedConsoleSemaphores;
bool exitThreadArray[MAX_THREAD_COUNT];  //Marks exited threads

TimeSortedWaitQueue *sleepQueueHead;	// Needed to implement system_call_Sleep

int schedulingAlgo;			// Scheduling algorithm to simulate
char **batchProcesses;			// Names of batch processes
int *priority;				// Process priority
int page_algo;              //Page replacement algo number

TranslationEntry *page_entries[NumPhysPages];  //list of page entries
List *page_queue;
int *LRU_clock;       //to be used for LRU
int reference_bit[NumPhysPages];    // array of reference bits of the page frames

int cpu_burst_start_time;        // Records the start of current CPU burst
int completionTimeArray[MAX_THREAD_COUNT];        // Records the completion time of all simulated threads
bool excludeMainThread;		// Used by completion time statistics calculation

#ifdef FILESYS_NEEDED
FileSystem  *fileSystem;
#endif

#ifdef FILESYS
SynchDisk   *synchDisk;
#endif

#ifdef USER_PROGRAM	// requires either FILESYS or FILESYS_STUB
Machine *machine;	// user program memory and registers
#endif

#ifdef NETWORK
PostOffice *postOffice;
#endif


// External definition, to allow us to take a pointer to this function
extern void Cleanup();


//----------------------------------------------------------------------
// TimerInterruptHandler
// 	Interrupt handler for the timer device.  The timer device is
//	set up to interrupt the CPU periodically (once every TimerTicks).
//	This routine is called each time there is a timer interrupt,
//	with interrupts disabled.
//
//	Note that instead of calling YieldCPU() directly (which would
//	suspend the interrupt handler, not the interrupted thread
//	which is what we wanted to context switch), we set a flag
//	so that once the interrupt handler is done, it will appear as 
//	if the interrupted thread called YieldCPU at the point it is 
//	was interrupted.
//
//	"dummy" is because every interrupt handler takes one argument,
//		whether it needs it or not.
//----------------------------------------------------------------------
static void
TimerInterruptHandler(int dummy)
{
    TimeSortedWaitQueue *ptr;
    if (interrupt->getStatus() != IdleMode) {
        // Check the head of the sleep queue
        while ((sleepQueueHead != NULL) && (sleepQueueHead->GetWhen() <= (unsigned)stats->totalTicks)) {
           sleepQueueHead->GetThread()->Schedule();
           ptr = sleepQueueHead;
           sleepQueueHead = sleepQueueHead->GetNext();
           delete ptr;
        }
        //printf("[%d] Timer interrupt.\n", stats->totalTicks);
        if ((schedulingAlgo == ROUND_ROBIN) || (schedulingAlgo == UNIX_SCHED)) {
           if ((stats->totalTicks - cpu_burst_start_time) >= SCHED_QUANTUM) {
              ASSERT(cpu_burst_start_time == currentThread->GetCPUBurstStartTime());
	      interrupt->YieldOnReturn();
           }
        }
    }
}


//function to print the queue
void printQueue()
{
    List *tmp_list = new List();
    int *tmp;

    printf("\nQueue: \t");
    tmp = (int *)page_queue->Remove();
    while(tmp != NULL){
        tmp_list->Append((void *)tmp);
        tmp = (int *)page_queue->Remove();

    }

    //the page_queue is now modified
    delete page_queue;
    page_queue = tmp_list;

}


//implementation of LRU_CLOCK

int *getLRUtimeFrame()
{
    List *original_list = new List();
    List *time_list = new List();

    //WE NEED TO HAVE TWO COPIES OF LIST
    //1. for traversal
    //2. maintain the fifo order

    int *tmp1;
    int *tmp = (int *)page_queue->Remove();
    while(tmp != NULL){
        tmp1 = new int(*tmp);
        original_list->Append((void *)tmp);
        time_list->Append((void *)tmp1);
        tmp = (int *)page_queue->Remove();
    }

    //time to delete the page_queue and connect it to
    //original_list
    delete page_queue;
    page_queue = original_list;

    //time to re order the clock so that clocktime is the head
    tmp = (int *)time_list->Remove();
    while(*tmp != *LRU_clock){
        time_list->Append((void *)tmp);
        tmp = (int *)time_list->Remove();
    }

    //now, we will run the LRU clock algorithm to check
    //if there exists an element with reference bit unset
    int *replacement_frame = NULL;    //This is the frame to be replaced
    while(TRUE){
        if(reference_bit[*tmp] == 0){
            replacement_frame = new int(*tmp);
            tmp = (int *)time_list->Remove();
            *LRU_clock = *tmp;
            break;
        }
        else{
            //reset the reference bit
            reference_bit[*tmp] = 0;
        }

        //Add this element back to time_list
        time_list->Append((void *)tmp);

        //delete tmp
        tmp = (int *)time_list->Remove();
    }

    //now, we need to delete the clocklist
    tmp = (int *)time_list->Remove();
    while(tmp != NULL){
        delete tmp;
        tmp = (int *)time_list->Remove();
    }
    delete time_list;

    //return the frame to be replaced
    return replacement_frame;

}


//This function is meant to delete an element from fifo queue
void deleteFromPageQueue(int data){
    List *tmp_list = new List();
    int *tmp;

    tmp = (int *)page_queue->Remove();
    while(tmp != NULL){
        if(*tmp == data){
            printf("\nGoing to delete the frame %d from page_queue\n", *tmp);
            delete tmp;
        }
        else{
            tmp_list->Append((void *)tmp);
        }
        tmp = (int *)page_queue->Remove();
    }

    //the current page_queue is has been modified
    delete page_queue;
    page_queue = tmp_list;

    printf("Going to delete queue %d", data);
    printQueue();
}

//----------------------------------------------------------------------
// Initialize
// 	Initialize Nachos global data structures.  Interpret command
//	line arguments in order to determine flags for the initialization.  
// 
//	"argc" is the number of command line arguments (including the name
//		of the command) -- ex: "nachos -d +" -> argc = 3 
//	"argv" is an array of strings, one for each command line argument
//		ex: "nachos -d +" -> argv = {"nachos", "-d", "+"}
//----------------------------------------------------------------------
void
Initialize(int argc, char **argv)
{
    int argCount, i;
    char* debugArgs = "";
    bool randomYield = FALSE;

    initializedConsoleSemaphores = false;
    numPagesAllocated = 0;
    next_unallocated_page = 0;
    freed_pages = new List();   //this list will have number of freed pages

    schedulingAlgo = NON_PREEMPTIVE_BASE;	// Default
    page_algo = NORMAL;
    page_queue = new List();

    for(i=0; i<NumPhysPages; ++i){
        page_entries[i] = NULL;
        reference_bit[i] = 0;
    }

    batchProcesses = new char*[MAX_BATCH_SIZE];
    ASSERT(batchProcesses != NULL);
    for (i=0; i<MAX_BATCH_SIZE; i++) {
       batchProcesses[i] = new char[256];
       ASSERT(batchProcesses[i] != NULL);
    }

    priority = new int[MAX_BATCH_SIZE];
    ASSERT(priority != NULL);
    
    excludeMainThread = FALSE;

    for (i=0; i<MAX_THREAD_COUNT; i++) { threadArray[i] = NULL; exitThreadArray[i] = false; completionTimeArray[i] = -1; }
    thread_index = 0;

    sleepQueueHead = NULL;

#ifdef USER_PROGRAM
    bool debugUserProg = FALSE;	// single step user program
#endif
#ifdef FILESYS_NEEDED
    bool format = FALSE;	// format disk
#endif
#ifdef NETWORK
    double rely = 1;		// network reliability
    int netname = 0;		// UNIX socket name
#endif
    
    for (argc--, argv++; argc > 0; argc -= argCount, argv += argCount) {
	argCount = 1;
	if (!strcmp(*argv, "-d")) {
	    if (argc == 1)
		debugArgs = "+";	// turn on all debug flags
	    else {
	    	debugArgs = *(argv + 1);
	    	argCount = 2;
	    }
	} else if (!strcmp(*argv, "-rs")) {
	    ASSERT(argc > 1);
	    RandomInit(atoi(*(argv + 1)));	// initialize pseudo-random
						// number generator
	    randomYield = TRUE;
	    argCount = 2;
	}
#ifdef USER_PROGRAM
	if (!strcmp(*argv, "-s"))
	    debugUserProg = TRUE;
#endif
#ifdef FILESYS_NEEDED
	if (!strcmp(*argv, "-f"))
	    format = TRUE;
#endif
#ifdef NETWORK
	if (!strcmp(*argv, "-l")) {
	    ASSERT(argc > 1);
	    rely = atof(*(argv + 1));
	    argCount = 2;
	} else if (!strcmp(*argv, "-m")) {
	    ASSERT(argc > 1);
	    netname = atoi(*(argv + 1));
	    argCount = 2;
	}
#endif
    }

    DebugInit(debugArgs);			// initialize DEBUG messages
    stats = new Statistics();			// collect statistics
    interrupt = new Interrupt;			// start up interrupt handling
    scheduler = new NachOSscheduler();		// initialize the ready queue
    //if (randomYield)				// start the timer (if needed)
       timer = new Timer(TimerInterruptHandler, 0, randomYield);

    threadToBeDestroyed = NULL;

    // We didn't explicitly allocate the current thread we are running in.
    // But if it ever tries to give up the CPU, we better have a NachOSThread
    // object to save its state.
    currentThread = NULL;
    currentThread = new NachOSThread("main", MIN_NICE_PRIORITY);		
    currentThread->setStatus(RUNNING);
    stats->start_time = stats->totalTicks;
    cpu_burst_start_time = stats->totalTicks;

    interrupt->Enable();
    CallOnUserAbort(Cleanup);			// if user hits ctl-C
    
#ifdef USER_PROGRAM
    machine = new Machine(debugUserProg);	// this must come first
#endif

#ifdef FILESYS
    synchDisk = new SynchDisk("DISK");
#endif

#ifdef FILESYS_NEEDED
    fileSystem = new FileSystem(format);
#endif

#ifdef NETWORK
    postOffice = new PostOffice(netname, rely, 10);
#endif
}

//----------------------------------------------------------------------
// Cleanup
// 	Nachos is halting.  De-allocate global data structures.
//----------------------------------------------------------------------
void
Cleanup()
{
    printf("\nCleaning up...\n");
#ifdef NETWORK
    delete postOffice;
#endif
    
#ifdef USER_PROGRAM
    delete machine;
#endif

#ifdef FILESYS_NEEDED
    delete fileSystem;
#endif

#ifdef FILESYS
    delete synchDisk;
#endif
    
    delete timer;
    delete scheduler;
    delete interrupt;
    
    Exit(0);
}


