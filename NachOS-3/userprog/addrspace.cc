// addrspace.cc 
//	Routines to manage address spaces (executing user programs).
//
//	In order to run a user program, you must:
//
//	1. link with the -N -T 0 option 
//	2. run coff2noff to convert the object file to Nachos format
//		(Nachos object code format is essentially just a simpler
//		version of the UNIX executable object code format)
//	3. load the NOFF file into the Nachos file system
//		(if you haven't implemented the file system yet, you
//		don't need to do this last step)
//
// Copyright (c) 1992-1993 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation 
// of liability and disclaimer of warranty provisions.

#include "copyright.h"
#include "system.h"
#include "addrspace.h"

//----------------------------------------------------------------------
// SwapHeader
// 	Do little endian to big endian conversion on the bytes in the 
//	object file header, in case the file was generated on a little
//	endian machine, and we're now running on a big endian machine.
//----------------------------------------------------------------------

static void 
SwapHeader (NoffHeader *noffH)
{
	noffH->noffMagic = WordToHost(noffH->noffMagic);
	noffH->code.size = WordToHost(noffH->code.size);
	noffH->code.virtualAddr = WordToHost(noffH->code.virtualAddr);
	noffH->code.inFileAddr = WordToHost(noffH->code.inFileAddr);
	noffH->initData.size = WordToHost(noffH->initData.size);
	noffH->initData.virtualAddr = WordToHost(noffH->initData.virtualAddr);
	noffH->initData.inFileAddr = WordToHost(noffH->initData.inFileAddr);
	noffH->uninitData.size = WordToHost(noffH->uninitData.size);
	noffH->uninitData.virtualAddr = WordToHost(noffH->uninitData.virtualAddr);
	noffH->uninitData.inFileAddr = WordToHost(noffH->uninitData.inFileAddr);
}

//----------------------------------------------------------------------
// ProcessAddrSpace::ProcessAddrSpace
// 	Create an address space to run a user program.
//	Load the program from a file "executable", and set everything
//	up so that we can start executing user instructions.
//
//	Assumes that the object code file is in NOFF format.
//
//	First, set up the translation from program memory to physical 
//	memory.  For now, this is really simple (1:1), since we are
//	only uniprogramming, and we have a single unsegmented page table
//
//	"executable" is the file containing the object code to load into memory
//----------------------------------------------------------------------

ProcessAddrSpace::ProcessAddrSpace(OpenFile *executable)
{
    
        unsigned int i, size;
        int pid_thread = currentThread->GetPID();

        executable->ReadAt((char *)&noffH, sizeof(noffH), 0);
        if ((noffH.noffMagic != NOFFMAGIC) && 
            (WordToHost(noffH.noffMagic) == NOFFMAGIC))
            SwapHeader(&noffH);
        ASSERT(noffH.noffMagic == NOFFMAGIC);

        // how big is address space?
        size = noffH.code.size + noffH.initData.size + noffH.uninitData.size 
                + UserStackSize;    // we need to increase the size
                            // to leave room for the stack
        numPagesInVM = divRoundUp(size, PageSize);

        DEBUG('a', "Initializing address space, num pages %d, size %d\n", 
                        numPagesInVM, size);

        // first, set up the translation 
        NachOSpageTable = new TranslationEntry[numPagesInVM];
        for (i = 0; i < numPagesInVM; i++) {
            NachOSpageTable[i].virtualPage = i;
            NachOSpageTable[i].physicalPage = -1;
            NachOSpageTable[i].valid = FALSE;
            NachOSpageTable[i].use = FALSE;
            NachOSpageTable[i].dirty = FALSE;
            NachOSpageTable[i].readOnly = FALSE;  // if the code segment was entirely on 
                            // a separate page, we could set its 
                            // pages to be read-only
            NachOSpageTable[i].shared = FALSE;
            NachOSpageTable[i].cached = FALSE;
            NachOSpageTable[i].pid_thread = pid_thread;
        }

        valid_pages = 0;   //initialization of number of valid pages to 0
        shared_pages_count = 0;   //initialization of shared pages count to 0

        // Now, we need to initialize the backupMemory 
        currentThread->initializeBackupMemory(numPagesInVM*PageSize);
    }
    

//----------------------------------------------------------------------
// ProcessAddrSpace::ProcessAddrSpace (ProcessAddrSpace*) is called by a forked thread.
//      We need to duplicate the address space of the parent.
//----------------------------------------------------------------------

ProcessAddrSpace::ProcessAddrSpace(ProcessAddrSpace *parentSpace, int pid_thread)
{
    
        numPagesInVM = parentSpace->GetNumPages();
        shared_pages_count = parentSpace->shared_pages_count;
        valid_pages = parentSpace->valid_pages;
        unsigned i, j, k;
        noffH = parentSpace->noffH;

        //time to copy the executable name of parentSpace to childSpace
        strcpy(filename, parentSpace->filename);

        numPagesAllocated += valid_pages - shared_pages_count;

        DEBUG('a', "Initializing address space, num pages %d, shared %d, valid %d\n",
                                            numPagesInVM, shared_pages_count, valid_pages);

        ASSERT(numPagesAllocated <= NumPhysPages);                // check we're not trying
                                                                                    // to run anything too big --
                                                                                    // at least until we have
                                                                                    // virtual memory


        // first, set up the translation
        TranslationEntry* parentPageTable = parentSpace->GetPageTable();
        NachOSpageTable = new TranslationEntry[numPagesInVM];
        for(i=0; i<numPagesInVM; i++){
            //shared pages need to point to the correct location
            if(parentPageTable[i].shared == TRUE){
                DEBUG('A', "Linking to shared page %d\n taking place", parentPageTable[i].physicalPage);
                NachOSpageTable[i].physicalPage = parentPageTable[i].physicalPage;
            }
            else{
                //allocate pages to those pages which are valid nad not to invalid pages
                if(parentPageTable[i].valid == TRUE){
                    //if any unallocated page is present in freed pool, then use them
                    //otherwise use the next unallocated page
                    int *physical_page_number = (int *)freed_pages->Remove();
                    if(physical_page_number == NULL){
                        NachOSpageTable[i].physicalPage = next_unallocated_page;
                        next_unallocated_page++;   //updating the number of allocated pages
                    }
                    else{
                        NachOSpageTable[i].physicalPage = *physical_page_number;
                        delete physical_page_number;
                    }

                    //store the reference to page table entry
                    page_entries[NachOSpageTable[i].physicalPage] = &NachOSpageTable[i];

                    printf("Creating a new page %d for %d copying %d\n", NachOSpageTable[i].physicalPage, currentThread->GetPID(),
                                                        parentPageTable[i].physicalPage);
                }

                }

            NachOSpageTable[i].virtualPage = i;
            // NachOSpageTable[i].physicalPage = i+numPagesAllocated;
            NachOSpageTable[i].valid = parentPageTable[i].valid;
            NachOSpageTable[i].use = parentPageTable[i].use;
            NachOSpageTable[i].dirty = parentPageTable[i].dirty;
            NachOSpageTable[i].readOnly = parentPageTable[i].readOnly;      // if the code segment was entirely on
                                                        // a separate page, we could set its
                                                        // pages to be read-only
            NachOSpageTable[i].shared = parentPageTable[i].shared;
            NachOSpageTable[i].cached = FALSE;
            NachOSpageTable[i].pid_thread = pid_thread; 
        }

        // Copy the contents
        unsigned startAddrParent;
        unsigned startAddrChild;
        for(i=0; i<numPagesInVM; i++){
            //If current page is not shared and the page is valid, then copy it
            if(!NachOSpageTable[i].shared && NachOSpageTable[i].valid){
                startAddrParent = parentPageTable[i].physicalPage * PageSize;
                startAddrChild = NachOSpageTable[i].physicalPage * PageSize;
                for(j=0; j<PageSize; ++j){
                    machine->mainMemory[startAddrChild+j] = machine->mainMemory[startAddrParent+j];
                }
            }
        }

        //Initialize the backup memory for child
        threadArray[pid_thread]->initializeBackupMemory(numPagesInVM * PageSize);
    }
    
unsigned ProcessAddrSpace::makeSharedPageTable(int size_allocate, int *pages_created)
{
    int pid_thread = currentThread->GetPID();
    unsigned number_original_pages = GetNumPages();   //stores the number of pages in original address space
    unsigned number_shared_pages = size_allocate / PageSize;  //compute the number of shared pages
    if(size_allocate % PageSize){    //round up the number of shared pages if needed
        number_shared_pages++;
    }
    shared_pages_count += number_shared_pages;
    *pages_created = number_shared_pages;  //to return the number of shared pages made
    numPagesInVM = number_original_pages + number_shared_pages;  //to update number of pages in VM
    unsigned i;

    numPagesAllocated += number_shared_pages;
    ASSERT(numPagesAllocated <= NumPhysPages);        // check we're not trying
                                                                                // to run anything too big --
                                                                                // at least until we have
                                                                                // virtual memory

    printf("Extending address space, shared pages %d\n", number_shared_pages);

    //first, set up the translation
    TranslationEntry* NachOSoriginalPageTable = GetPageTable();
    NachOSpageTable = new TranslationEntry[numPagesInVM];
    for(i=0; i<number_original_pages; i++){
        NachOSpageTable[i].virtualPage = i;
        NachOSpageTable[i].physicalPage = NachOSoriginalPageTable[i].physicalPage;
        NachOSpageTable[i].valid = NachOSoriginalPageTable[i].valid;
        NachOSpageTable[i].use = NachOSoriginalPageTable[i].use;
        NachOSpageTable[i].dirty = NachOSoriginalPageTable[i].dirty;
        NachOSpageTable[i].readOnly = NachOSoriginalPageTable[i].readOnly;      // if the code segment was entirely on
                                                    // a separate page, we could set its
                                                    // pages to be read-only
        NachOSpageTable[i].shared = NachOSoriginalPageTable[i].shared;
        NachOSpageTable[i].cached = NachOSoriginalPageTable[i].cached;
        NachOSpageTable[i].pid_thread =NachOSoriginalPageTable[i].pid_thread;  

        //store a reference to page table entry
        page_entries[NachOSpageTable[i].physicalPage] = &NachOSpageTable[i];
    }
    
    //Now, set up the translation entry for shared memory region
    for(i=number_original_pages; i<numPagesInVM; ++i){
        NachOSpageTable[i].virtualPage = i;

        //if any unallocated page is present in freed pool, then use them
        //otherwise use the next unallocated page
        int *physical_page_number = (int *)freed_pages->Remove();
        if(physical_page_number == NULL){
            NachOSpageTable[i].physicalPage = next_unallocated_page;
            bzero(&machine->mainMemory[next_unallocated_page*PageSize], PageSize);
            next_unallocated_page++;      //update number of unallocated pages
        }
        else{
            NachOSpageTable[i].physicalPage = *physical_page_number;
            delete physical_page_number;
        }

        printf("Going to create a shared page %d for thread %d\n", NachOSpageTable[i].physicalPage, currentThread->GetPID());

        //time to store this page table entry into the hash-map of page_entries
        printf("Going to add page entry for %d\n", NachOSpageTable[i].physicalPage);
        page_entries[NachOSpageTable[i].physicalPage] = &NachOSpageTable[i];

        NachOSpageTable[i].valid = TRUE;
        NachOSpageTable[i].use = FALSE;
        NachOSpageTable[i].dirty = FALSE;
        NachOSpageTable[i].readOnly = FALSE;      // if the code segment was entirely on
                                                    // a separate page, we could set its
                                                    // pages to be read-only
        NachOSpageTable[i].shared = TRUE;
        NachOSpageTable[i].cached = FALSE;
        NachOSpageTable[i].pid_thread =pid_thread;
    }

    //Increase the number of pages allocated by number of shared pages allocated right now
    valid_pages += number_shared_pages;


    //setting up machine parameters
    machine->NachOSpageTable = NachOSpageTable;
    machine->NachOSpageTableSize = numPagesInVM;

    //free NachOSoriginalPageTable
    delete NachOSoriginalPageTable;

    //return the starting address of shared page
    return number_original_pages * PageSize;

}

//----------------------------------------------------------------------
// ProcessAddrSpace::~ProcessAddrSpace
// 	Dealloate an address space.  Nothing for now!
//----------------------------------------------------------------------

ProcessAddrSpace::~ProcessAddrSpace()
{
    delete NachOSpageTable;
}

//----------------------------------------------------------------------
// ProcessAddrSpace::InitUserCPURegisters
// 	Set the initial values for the user-level register set.
//
// 	We write these directly into the "machine" registers, so
//	that we can immediately jump to user code.  Note that these
//	will be saved/restored into the currentThread->userRegisters
//	when this thread is context switched out.
//----------------------------------------------------------------------

void
ProcessAddrSpace::InitUserCPURegisters()
{
    int i;

    for (i = 0; i < NumTotalRegs; i++)
	machine->WriteRegister(i, 0);

    // Initial program counter -- must be location of "Start"
    machine->WriteRegister(PCReg, 0);	

    // Need to also tell MIPS where next instruction is, because
    // of branch delay possibility
    machine->WriteRegister(NextPCReg, 4);

   // Set the stack register to the end of the address space, where we
   // allocated the stack; but subtract off a bit, to make sure we don't
   // accidentally reference off the end!
    machine->WriteRegister(StackReg, numPagesInVM * PageSize - 16);
    DEBUG('a', "Initializing stack register to %d\n", numPagesInVM * PageSize - 16);
}

//----------------------------------------------------------------------
// ProcessAddrSpace::SaveStateOnSwitch
// 	On a context switch, save any machine state, specific
//	to this address space, that needs saving.
//
//	For now, nothing!
//----------------------------------------------------------------------

void ProcessAddrSpace::SaveStateOnSwitch() 
{}

//----------------------------------------------------------------------
// ProcessAddrSpace::RestoreStateOnSwitch
// 	On a context switch, restore the machine state so that
//	this address space can run.
//
//      For now, tell the machine where to find the page table.
//----------------------------------------------------------------------

void ProcessAddrSpace::RestoreStateOnSwitch() 
{
    machine->NachOSpageTable = NachOSpageTable;
    machine->NachOSpageTableSize = numPagesInVM;
}

unsigned
ProcessAddrSpace::GetNumPages()
{
   return numPagesInVM;
}

TranslationEntry*
ProcessAddrSpace::GetPageTable()
{
   return NachOSpageTable;
}

//setPagesFree frees the pages of given address space 
//and adds then to freed_pages list

void ProcessAddrSpace::setPagesFree(bool delete_page_table)
{
    //iterate through the list of pages on address space and add
    //all pages into the free pages list
    int i;
    int count, *tmp;
    for(i=0, count=0; i<numPagesInVM; i++){
        if(NachOSpageTable[i].valid && !NachOSpageTable[i].shared){  //if not shared and valid
            count++;
            tmp = new int(NachOSpageTable[i].physicalPage);
            freed_pages->Append((void *)tmp);
            printf("Going to free the page %d\n", NachOSpageTable[i].physicalPage);

            //remove this entry from hash-map
            page_entries[NachOSpageTable[i].physicalPage] = NULL;

            //delete this element from page_queue
            deleteFromPageQueue(NachOSpageTable[i].physicalPage);
        }
    }

    //if delete_page_table is true, then delete the page table
    if(delete_page_table){
        delete NachOSpageTable;
    }

    numPagesAllocated -= count;   //reduce the number of allocated pages by

}

