// translate.cc 
//	Routines to translate virtual addresses to physical addresses.
//	Software sets up a table of legal translations.  We look up
//	in the table on every memory reference to find the true physical
//	memory location.
//
// Two types of translation are supported here.
//
//	Linear page table -- the virtual page # is used as an index
//	into the table, to find the physical page #.
//
//	Translation lookaside buffer -- associative lookup in the table
//	to find an entry with the same virtual page #.  If found,
//	this entry is used for the translation.
//	If not, it traps to software with an exception. 
//
//	In practice, the TLB is much smaller than the amount of physical
//	memory (16 entries is common on a machine that has 1000's of
//	pages).  Thus, there must also be a backup translation scheme
//	(such as page tables), but the hardware doesn't need to know
//	anything at all about that.
//
//	Note that the contents of the TLB are specific to an address space.
//	If the address space changes, so does the contents of the TLB!
//
// DO NOT CHANGE -- part of the machine emulation
//
// Copyright (c) 1992-1993 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation 
// of liability and disclaimer of warranty provisions.

#include "copyright.h"
#include "machine.h"
#include "addrspace.h"
#include "system.h"
// #include <stdlib.h>
#include "filesys.h"

// Routines for converting Words and Short Words to and from the
// simulated machine's format of little endian.  These end up
// being NOPs when the host machine is also little endian (DEC and Intel).

unsigned int
WordToHost(unsigned int word) {
#ifdef HOST_IS_BIG_ENDIAN
	 register unsigned long result;
	 result = (word >> 24) & 0x000000ff;
	 result |= (word >> 8) & 0x0000ff00;
	 result |= (word << 8) & 0x00ff0000;
	 result |= (word << 24) & 0xff000000;
	 return result;
#else 
	 return word;
#endif /* HOST_IS_BIG_ENDIAN */
}

unsigned short
ShortToHost(unsigned short shortword) {
#ifdef HOST_IS_BIG_ENDIAN
	 register unsigned short result;
	 result = (shortword << 8) & 0xff00;
	 result |= (shortword >> 8) & 0x00ff;
	 return result;
#else 
	 return shortword;
#endif /* HOST_IS_BIG_ENDIAN */
}

unsigned int
WordToMachine(unsigned int word) { return WordToHost(word); }

unsigned short
ShortToMachine(unsigned short shortword) { return ShortToHost(shortword); }

extern int* getLRUtimeFrame();   //to get the LRU clock frame
extern void printQueue();   //to print the queue


//----------------------------------------------------------------------
// Machine::ReadMem
//      Read "size" (1, 2, or 4) bytes of virtual memory at "addr" into 
//	the location pointed to by "value".
//
//   	Returns FALSE if the translation step from virtual to physical memory
//   	failed.
//
//	"addr" -- the virtual address to read from
//	"size" -- the number of bytes to read (1, 2, or 4)
//	"value" -- the place to write the result
//----------------------------------------------------------------------

bool
Machine::ReadMem(int addr, int size, int *value)
{
    int data;
    ExceptionType exception;
    int physicalAddress;
    
    DEBUG('a', "Reading VA 0x%x, size %d\n", addr, size);
    
    exception = Translate(addr, &physicalAddress, size, FALSE);
    if (exception != NoException) {
	machine->RaiseException(exception, addr);
	return FALSE;
    }
    switch (size) {
      case 1:
	data = machine->mainMemory[physicalAddress];
	*value = data;
	break;
	
      case 2:
	data = *(unsigned short *) &machine->mainMemory[physicalAddress];
	*value = ShortToHost(data);
	break;
	
      case 4:
	data = *(unsigned int *) &machine->mainMemory[physicalAddress];
	*value = WordToHost(data);
	break;

      default: ASSERT(FALSE);
    }
    
    DEBUG('a', "\tvalue read = %8.8x\n", *value);
    return (TRUE);
}

//----------------------------------------------------------------------
// Machine::WriteMem
//      Write "size" (1, 2, or 4) bytes of the contents of "value" into
//	virtual memory at location "addr".
//
//   	Returns FALSE if the translation step from virtual to physical memory
//   	failed.
//
//	"addr" -- the virtual address to write to
//	"size" -- the number of bytes to be written (1, 2, or 4)
//	"value" -- the data to be written
//----------------------------------------------------------------------

bool
Machine::WriteMem(int addr, int size, int value)
{
    ExceptionType exception;
    int physicalAddress;
     
    DEBUG('a', "Writing VA 0x%x, size %d, value 0x%x\n", addr, size, value);

    exception = Translate(addr, &physicalAddress, size, TRUE);
    if (exception != NoException) {
	machine->RaiseException(exception, addr);
	return FALSE;
    }
    switch (size) {
      case 1:
	machine->mainMemory[physicalAddress] = (unsigned char) (value & 0xff);
	break;

      case 2:
	*(unsigned short *) &machine->mainMemory[physicalAddress]
		= ShortToMachine((unsigned short) (value & 0xffff));
	break;
      
      case 4:
	*(unsigned int *) &machine->mainMemory[physicalAddress]
		= WordToMachine((unsigned int) value);
	break;
	
      default: ASSERT(FALSE);
    }
    
    return TRUE;
}

//----------------------------------------------------------------------
// Machine::Translate
// 	Translate a virtual address into a physical address, using 
//	either a page table or a TLB.  Check for alignment and all sorts 
//	of other errors, and if everything is ok, set the use/dirty bits in 
//	the translation table entry, and store the translated physical 
//	address in "physAddr".  If there was an error, returns the type
//	of the exception.
//
//	"virtAddr" -- the virtual address to translate
//	"physAddr" -- the place to store the physical address
//	"size" -- the amount of memory being read or written
// 	"writing" -- if TRUE, check the "read-only" bit in the TLB
//----------------------------------------------------------------------

ExceptionType
Machine::Translate(int virtAddr, int* physAddr, int size, bool writing)
{
    int i, j;
    unsigned int vpn, offset;
    TranslationEntry *entry;
    unsigned int page_frame;
 
    int flag = 0;
    unsigned int numPagesInVM = currentThread->space->GetNumPages();
    unsigned int start_machine, start_backup;
 
    DEBUG('a', "\tTranslate 0x%x, %s: \n\t", virtAddr, writing ? "write" : "read");
 
    // check for alignment errors
    if (((size == 4) && (virtAddr & 0x3)) || ((size == 2) && (virtAddr & 0x1))){
        DEBUG('a', "alignment problem at %d, size %d!\n", virtAddr, size);
        return AddressErrorException;
    }
 
    // we must have either a TLB or a page table, but not both!
    ASSERT(tlb == NULL || NachOSpageTable == NULL);   
    ASSERT(tlb != NULL || NachOSpageTable != NULL);   
 
    // calculate the virtual page number, and offset within the page,
    // from the virtual address
    vpn = (unsigned) virtAddr / PageSize;
    offset = (unsigned) virtAddr % PageSize;
 
    if (tlb == NULL) {      // => page table => vpn is index into table
        if (vpn >= NachOSpageTableSize) {
            DEBUG('a', "virtual page # %d too large for page table size %d!\n", 
                    virtAddr, NachOSpageTableSize);
            return AddressErrorException;
        } else if (!NachOSpageTable[vpn].valid) {
            if(currentThread->space->valid_pages < numPagesInVM) {
                // In this case we have to perform demand paging whose code is
                // given below
                flag = 1;
            } else {
                DEBUG('a', "virtual page # %d too large for page table size %d!\n", 
                        virtAddr, NachOSpageTableSize);
                return PageFaultException;
            }
        }
        entry = &NachOSpageTable[vpn];
 
        // Demand Paging
        if(flag) {
            // Open the file while is to be loaded into memory
            OpenFile *executable = fileSystem->Open(currentThread->space->filename);
            NoffHeader noffH = currentThread->space->noffH;
            unsigned int size = numPagesInVM * PageSize;
            unsigned int read_size = PageSize;
 
            if(numPagesAllocated == NumPhysPages) {
                // here we have to handle page replacement
                DEBUG('R', "\nInvoking page replacement algorithm: ");
 
                // The frame which will be replace
                int *frame_to_replace;
                TranslationEntry *frame_entry;
 
                // Select the frame according to the algorithm
                if(page_algo == FIFO) {
                    frame_to_replace = (int *)page_queue->Remove();
                    DEBUG('R', "\n\tFIFO selects frame %d", *frame_to_replace);
 
                } else if (page_algo == RANDOM) {
                    frame_to_replace = new int(1);
                    DEBUG('R', "\n\tRANDOM selects frame %d", *frame_to_replace);
 
                } else if (page_algo == LRU) {
                    frame_to_replace = (int *)page_queue->Remove();
                    DEBUG('R', "\n\tLRU selects frame %d", *frame_to_replace);
                } else if (page_algo == LRU_CLOCK) {
                    frame_to_replace = getLRUtimeFrame();
                    DEBUG('R', "\n\tLRU CLOCK selects frame %d", *frame_to_replace);
                }
 
                // Get the PTE of this frame
 
 
 
 
 
                frame_entry = page_entries[*frame_to_replace];
                NachOSThread *thread = threadArray[frame_entry->pid_thread];
 
                // We have to delete the frame_to_replace
                delete frame_to_replace;
 
                // DEBUG('R', "\n\tvirtual %d physical %d thread %d shared %d valid %d", 
                //         frame_entry->virtualPage, frame_entry->physicalPage, 
                //         frame_entry->threadPid, frame_entry->shared,
                //         frame_entry->valid);
 
                // Now this page should no longer be valid
                frame_entry->valid = FALSE;
 
                thread->space->valid_pages--;
 
                // Now we have to copy the values between the backup_memory and
                // machineMemory in case the page is dirty
                if(frame_entry->dirty) {
                    // DEBUG('R', "\n\tpage %d of thread %d is dirty"
                    //         , frame_entry->virtualPage, frame_entry->threadPid);
 
                    // Now this frame_entry is cached
                    frame_entry->cached = TRUE;
 
                    // Now we have to copy the mainMemory to backup_memory
                    start_machine = frame_entry->physicalPage * PageSize;
                    start_backup = frame_entry->virtualPage * PageSize;
                    for(j=0; j<PageSize; j++) {
                        thread->backup_memory[start_backup+j] = machine->mainMemory[start_machine+j];
                        // DEBUG('r', "B: %d\n", thread->backup_memory[start_backup+j]);
                        // DEBUG('r', "M: %d\n", machine->mainMemory[start_machine+j]);
                    }
 
 
                }
 
                // Now we have to change the page_frame of entry
                entry->physicalPage = frame_entry->physicalPage;
                DEBUG('R', "\n\n");
            } else {
                // Increment the numPagesAllocated
                numPagesAllocated++;
 
                // We either take the page from the pool of freed pages of we take a
                // page from the pool of unallocated pages
                int *physical_page_number = (int *)freed_pages->Remove();
                if(physical_page_number == NULL) {
                    entry->physicalPage = next_unallocated_page;
                    next_unallocated_page++;   // Update the number of pages allocated
                } else {
                    entry->physicalPage = *physical_page_number;
                    delete physical_page_number;
 
 
                }
 
 
 
 
 
 
 
 
 
            }
 
 
 
 
 
 
 
            // This stores a refernce to the pageTable entry
            page_entries[entry->physicalPage] = entry;
 
            // This is for ease of access
            page_frame = entry->physicalPage;
 
            // DEBUG('A', "Allocating physical page %d VPN %d virtualaddress %d\n", page_frame, vpn, virtAddr);
 
            // In case of FIFO or LRU_CLOCK we maintain a fifo queue of elements
            if(page_algo == FIFO) {
                int *temp = new int(page_frame);
                page_queue->Append((void *)temp);
                DEBUG('Q', "FIFO Update ");
                printQueue();
            }
 
            if(page_algo == LRU_CLOCK){
 
                int *temp = new int(page_frame);
                page_queue->Append((void *)temp);
 
                // If this is the first page fault then we set the LRU_CLOCK
                // hand
                if(stats->numPageFaults == 0) {
                    DEBUG('q', "Setting the LRU_CLOCK Hand to %d\n", *temp);
                    LRU_clock = new int(*temp);
                }
 
                DEBUG('Q', "LRU_CLOCK Update\t ");
                printQueue();
            }
 
            // zero out this particular page
            bzero(&machine->mainMemory[page_frame*PageSize], PageSize);
 
            // Now here are two cases, we may either have to use the
            // backup_memory of the thread or the mainMemory
 
            if(entry->cached) {
                DEBUG('R', "page %d of %d has been modified\n", 
                        entry->virtualPage, entry->pid_thread);
 
                start_machine = page_frame * PageSize;
                start_backup = entry->virtualPage * PageSize;
                for(j=0; j<PageSize; ++j) {
                    machine->mainMemory[start_machine+j] = currentThread->backup_memory[start_backup+j];
                }
 
            } else {
                // Now copy the corresponding area from memory
                if( vpn == (numPagesInVM - 1) ) {
                    read_size = size - vpn * PageSize;
                }
 
                executable->ReadAt(&(machine->mainMemory[page_frame * PageSize]),
                        read_size, noffH.code.inFileAddr + vpn*PageSize);
            }
 
 
            // The number of valid pages of this thread has increased
            currentThread->space->valid_pages++;
 
            // Mark this pagetable entry as valid
            entry->valid = TRUE;
 
            // delete the opened executable
            delete executable;
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
            return PageFaultException;
 
 
        }
    } else {
        for (entry = NULL, i = 0; i < TLBSize; i++)
            if (tlb[i].valid && (tlb[i].virtualPage == vpn)) {
                entry = &tlb[i];            // FOUND!
                break;
            }
        if (entry == NULL) {                // not found
            DEBUG('a', "*** no valid TLB entry found for this virtual page!\n");
            return PageFaultException;      // really, this is a TLB fault,
            // the page may be in memory,
            // but not in the TLB
        }
    }
 
    if (entry->readOnly && writing) {    // trying to write to a read-only page
        DEBUG('a', "%d mapped read-only at %d in TLB!\n", virtAddr, i);
        return ReadOnlyException;
    }
    page_frame = entry->physicalPage;
 
    // if the page_frame is too big, there is something really wrong! 
    // An invalid translation was loaded into the page table or TLB. 
    if (page_frame >= NumPhysPages) { 
        DEBUG('a', "*** frame %d > %d!\n", page_frame, NumPhysPages);
        return BusErrorException;
    }
    entry->use = TRUE;       // set the use, dirty bits
    if (writing)
        entry->dirty = TRUE;
    *physAddr = page_frame * PageSize + offset;
    ASSERT((*physAddr >= 0) && ((*physAddr + size) <= MemorySize));
    DEBUG('a', "phys addr = 0x%x\n", *physAddr);
 
    // A new frame is updated so ww change the LRU page_queue
    if(page_algo == LRU) {
        // First of all we delete the element if it's in the queue
        deleteFromPageQueue(page_frame);
 
        // We add this element to the bottom of the page_queue, every time we
        // pop, we pop from the head so this is the same as LRU
        int *temp = new int(page_frame);
        page_queue->Append((void *)temp);
        DEBUG('Q', "LRU Update %d\t", page_frame);
        printQueue();
 
 
    } else if (page_algo == LRU_CLOCK) {
        // We have to set the reference bit of the element if it's not set and
        // let it remain the same if it's set
        reference_bit[page_frame] = 1;
        DEBUG('Q', "LRU_C Update %d\t", page_frame);
        printQueue();
    }
 
    return NoException;
}

//----------------------------------------------------------------------
// Machine::GetPA
//      Returns the physical address corresponding to the passed virtual
//      address. Error conditions to check: virtual page number (vpn) is
//      bigger than the page table size and the page table entry is not valid.
//      In case of error, returns -1.
//----------------------------------------------------------------------

int
Machine::GetPA (unsigned vaddr)
{
   unsigned vpn = vaddr/PageSize;
   unsigned offset = vaddr % PageSize;
   TranslationEntry *entry;
   unsigned int page_frame;

   if ((vpn < NachOSpageTableSize) && NachOSpageTable[vpn].valid) {
      entry = &NachOSpageTable[vpn];
      page_frame = entry->physicalPage;
      if (page_frame >= NumPhysPages) return -1;
      return page_frame * PageSize + offset;
   }
   else return -1;
}

