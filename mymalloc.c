#include <windows.h>
#include <stdio.h>
#include <stdlib.h> // For size_t

// Define custom block structure
typedef struct Block {
    size_t size;          // Size of the block, including header
    struct Block* next;   // Pointer to the next free block
    int free;             // 1 if free, 0 if allocated
} Block;

// Global head of the free list
static Block* free_list_head = NULL;

// Base address of the memory pool managed by our allocator
static LPVOID memory_pool_base = NULL;
static size_t memory_pool_size = 0;

// Minimum block size (to accommodate the Block header and alignment)
#define MIN_BLOCK_SIZE (sizeof(Block) + 8) // +8 for some minimal payload space and alignment

// Helper function to get block header from data pointer
#define GET_BLOCK_HEADER(ptr) ((Block*)((char*)ptr - sizeof(Block)))

// Initialize the memory pool
void my_malloc_init(size_t pool_size) {
    if (memory_pool_base != NULL) {
        fprintf(stderr, "Error: my_malloc_init called more than once.\n");
        return;
    }

    // Allocate a large chunk of memory from the OS
    memory_pool_base = VirtualAlloc(
        NULL,                 // Let the system determine the allocation address
        pool_size,            // Size of the region to allocate
        MEM_COMMIT | MEM_RESERVE, // Commit and reserve pages
        PAGE_READWRITE        // Read/write access
    );

    if (memory_pool_base == NULL) {
        fprintf(stderr, "Error: VirtualAlloc failed with error %lu\n", GetLastError());
        return;
    }

    memory_pool_size = pool_size;
    printf("Memory pool initialized: Base address 0x%p, size %zu bytes.\n", memory_pool_base, pool_size);

    // Initialize the entire pool as one large free block
    free_list_head = (Block*)memory_pool_base;
    free_list_head->size = memory_pool_size;
    free_list_head->next = NULL;
    free_list_head->free = 1;
}

// Custom malloc implementation
void* my_malloc(size_t bytes) {
    if (memory_pool_base == NULL) {
        fprintf(stderr, "Error: my_malloc_init not called. Initializing with default 1MB pool.\n");
        my_malloc_init(1024 * 1024); // Default 1MB pool
        if (memory_pool_base == NULL) {
            return NULL;
        }
    }

    // Adjust requested bytes to include header and ensure minimum size/alignment
    size_t total_size = bytes + sizeof(Block);
    if (total_size < MIN_BLOCK_SIZE) {
        total_size = MIN_BLOCK_SIZE;
    }

    // Ensure size is a multiple of 8 for alignment
    total_size = (total_size + 7) & ~7;

    Block* current = free_list_head;
    Block* prev = NULL;

    while (current != NULL) {
        if (current->free && current->size >= total_size) {
            // Found a suitable free block

            // Check if we can split the block
            if (current->size - total_size >= MIN_BLOCK_SIZE) {
                // Split the block
                Block* new_block = (Block*)((char*)current + total_size);
                new_block->size = current->size - total_size;
                new_block->next = current->next;
                new_block->free = 1;

                current->size = total_size;
                current->next = new_block; // Update current block's next pointer
            }

            current->free = 0; // Mark current block as allocated

            // If this block was the head of the free list, update the head
            if (current == free_list_head) {
                free_list_head = current->next; // Point to the next free block, which might be the split remainder
            } else {
                // Remove the allocated block from the free list (if it was fully consumed)
                // If it was split, the remainder is still in the list and its 'next' points to it.
                // If we completely consume current, we need to update prev->next to skip current.
                if (current->next != NULL && current->next->free) { // If it was split, the remainder is current->next
                    // The allocated block is 'current', and its 'next' points to the *newly split free block*.
                    // This means 'current' is still linking to a free block, but 'current' itself is not free.
                    // We need to re-link prev->next to point past 'current' to the actual next free block.
                    // This is where a double linked list or more careful free list management would simplify.
                    // For a simple first-fit free list, when we allocate 'current', we should remove it from the
                    // list and add the *remainder* (if split) to the free list.
                    // My current splitting logic keeps the remainder connected via current->next.
                    // Let's refine the free list management:
                    // If 'current' is fully consumed (no split), remove 'current' from the free list.
                    // If 'current' is split, the smaller 'current' becomes allocated, and the 'new_block' is the free remainder.
                    // In this case, 'new_block' should effectively replace 'current' in the free list chain.

                    if (prev != NULL) {
                        prev->next = current->next; // If current was split, prev now points to the new_block
                    } else { // 'current' was the head, and was split
                        free_list_head = current->next; // Head now points to the new_block
                    }
                } else { // Current was fully consumed, not split, or remainder is not free
                    if (prev != NULL) {
                        prev->next = current->next; // Remove current from the free list
                    } else { // Current was head and fully consumed
                        free_list_head = current->next; // Head points to whatever was after current
                    }
                }
            }

            // Return pointer to the user data section (after the Block header)
            return (void*)((char*)current + sizeof(Block));
        }
        prev = current;
        current = current->next;
    }

    fprintf(stderr, "Error: my_malloc failed to find a block of size %zu.\n", bytes);
    return NULL; // No suitable block found
}


// Custom free implementation
void my_free(void* ptr) {
    if (ptr == NULL) {
        return;
    }

    Block* block = GET_BLOCK_HEADER(ptr);

    // Basic validation: ensure the pointer is within our managed pool and is not already free
    if ((char*)block < (char*)memory_pool_base ||
        (char*)block >= (char*)memory_pool_base + memory_pool_size ||
        block->free == 1) {
        fprintf(stderr, "Error: my_free received an invalid or already freed pointer 0x%p.\n", ptr);
        return;
    }

    block->free = 1; // Mark block as free

    // Add block back to the free list, maintaining sorted order by address
    if (free_list_head == NULL || block < free_list_head) {
        block->next = free_list_head;
        free_list_head = block;
    } else {
        Block* current = free_list_head;
        while (current->next != NULL && current->next < block) {
            current = current->next;
        }
        block->next = current->next;
        current->next = block;
    }

    // Coalesce adjacent free blocks
    Block* current = free_list_head;
    while (current != NULL && current->next != NULL) {
        // If current block and the next block are adjacent in memory
        if ((char*)current + current->size == (char*)current->next && current->next->free) {
            current->size += current->next->size; // Merge them
            current->next = current->next->next;  // Skip the merged block
            // Do not advance current, check for further merging with the new next
        } else {
            current = current->next;
        }
    }
}

// Clean up the memory pool
void my_malloc_cleanup() {
    if (memory_pool_base != NULL) {
        if (!VirtualFree(memory_pool_base, 0, MEM_RELEASE)) {
            fprintf(stderr, "Error: VirtualFree failed with error %lu\n", GetLastError());
        } else {
            printf("Memory pool at 0x%p released.\n", memory_pool_base);
        }
        memory_pool_base = NULL;
        memory_pool_size = 0;
        free_list_head = NULL;
    }
}


// --- Test Code ---
void print_free_list() {
    printf("\n--- Free List Status ---\n");
    if (free_list_head == NULL) {
        printf("Free list is empty.\n");
        return;
    }
    Block* current = free_list_head;
    int i = 0;
    while (current != NULL) {
        printf("Block %d: Addr 0x%p, Size %zu, Free: %d, Next: 0x%p\n",
               i++, current, current->size, current->free, current->next);
        current = current->next;
    }
    printf("------------------------\n");
}

int main() {
    printf("Initializing my_malloc with 64KB pool...\n");
    my_malloc_init(64 * 1024); // 64 KB pool
    print_free_list();

    printf("\nAllocating 100 bytes (p1)...\n");
    char* p1 = (char*)my_malloc(100);
    if (p1) {
        sprintf(p1, "Hello from p1!");
        printf("p1 allocated at 0x%p (data), value: \"%s\"\n", p1, p1);
    }
    print_free_list();

    printf("\nAllocating 200 bytes (p2)...\n");
    char* p2 = (char*)my_malloc(200);
    if (p2) {
        sprintf(p2, "Hello from p2!");
        printf("p2 allocated at 0x%p (data), value: \"%s\"\n", p2, p2);
    }
    print_free_list();

    printf("\nAllocating 50 bytes (p3)...\n");
    char* p3 = (char*)my_malloc(50);
    if (p3) {
        sprintf(p3, "Hello from p3!");
        printf("p3 allocated at 0x%p (data), value: \"%s\"\n", p3, p3);
    }
    print_free_list();

    printf("\nFreeing p2 (200 bytes)...\n");
    my_free(p2);
    printf("p2 freed.\n");
    print_free_list(); // Should show p2's block merged if adjacent or added

    printf("\nAllocating 150 bytes (p4). This might reuse p2's space or split it.\n");
    char* p4 = (char*)my_malloc(150);
    if (p4) {
        sprintf(p4, "Hello from p4!");
        printf("p4 allocated at 0x%p (data), value: \"%s\"\n", p4, p4);
    }
    print_free_list();

    printf("\nFreeing p1 (100 bytes)...\n");
    my_free(p1);
    printf("p1 freed.\n");
    print_free_list(); // p1 should be added and potentially coalesced

    printf("\nFreeing p3 (50 bytes)...\n");
    my_free(p3);
    printf("p3 freed.\n");
    print_free_list(); // p3 should be added and potentially coalesced

    printf("\nFreeing p4 (150 bytes)...\n");
    my_free(p4);
    printf("p4 freed.\n");
    print_free_list(); // All blocks should be freed and coalesced into one big block

    printf("\nTesting allocation failure (large request)...\n");
    char* p_large = (char*)my_malloc(100 * 1024); // Request more than 64KB pool
    if (p_large == NULL) {
        printf("Correctly failed to allocate 100KB.\n");
    }

    printf("\n--- End of test ---\n");
    my_malloc_cleanup();

    return 0;
}