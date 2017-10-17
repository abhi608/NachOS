// addrspace.h 
//	Data structures to keep track of executing user programs 
//	(address spaces).
//
//	For now, we don't keep any information about address spaces.
//	The user level CPU state is saved and restored in the thread
//	executing the user program (see thread.h).
//
// Copyright (c) 1992-1993 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation 
// of liability and disclaimer of warranty provisions.

#ifndef ADDRSPACE_H
#define ADDRSPACE_H
#include "copyright.h"
#include "filesys.h"
#include "noff.h"

#define UserStackSize		1024 	// increase this as necessary!

extern void deleteFromPageQueue(int data);

class ProcessAddrSpace {
  public:
    ProcessAddrSpace(OpenFile *executable);	// Create an address space,
					// initializing it with the program
					// stored in the file "executable"

    ProcessAddrSpace (ProcessAddrSpace *parentSpace, int pid_thread);	// Used by fork

    ~ProcessAddrSpace();			// De-allocate an address space

    void InitUserCPURegisters();		// Initialize user-level CPU registers,
					// before jumping to user code

    void SaveStateOnSwitch();			// Save/restore address space-specific
    void RestoreStateOnSwitch();		// info on a context switch
    void setPagesFree(bool delete_page_table);  //in order to free the pages and delete the page table

    unsigned GetNumPages();

    TranslationEntry* GetPageTable();

    unsigned makeSharedPageTable(int size_allocate, int *pages_created);  //makes a new page table with shared pages_created
    int shared_pages_count;  //to keep count of number of shared pages
    int valid_pages;      //keeps a count of valid pages of address space
    NoffHeader noffH;
    // noffHeader noff;   //stores information, see more in bin/noff.h
    char filename[300];

  private:
    TranslationEntry *NachOSpageTable;	// Assume linear page table translation
					// for now!
    unsigned int numPagesInVM;		// Number of pages in the virtual 
					// address space
};

#endif // ADDRSPACE_H

