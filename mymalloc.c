#include <windows.h>
#include <stdio.h>
#include <stdlib.h> // For size_t, memcpy

// Define custom block structure
typedef struct Block {
    size_t size;          // Size of the block, including header
    struct Block* next;   // Pointer to the next free block
    int free;             // 1 if free, 0 if allocated
    // char data[1]; // Flexible array member for user data
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

// Helper to ensure size is a multiple of 8 for alignment
static size_t align_size(size_t s) {
    return (s + 7) & ~7;
}

// Helper to add a block to the free list, maintaining sorted order and coalescing
static void add_block_to_free_list(Block* new_free_block) {
    if (new_free_block == NULL) return;

    new_free_block->free = 1;

    if (free_list_head == NULL || new_free_block < free_list_head) {
        new_free_block->next = free_list_head;
        free_list_head = new_free_block;
    } else {
        Block* current = free_list_head;
        while (current->next != NULL && current->next < new_free_block) {
            current = current->next;
        }
        new_free_block->next = current->next;
        current->next = new_free_block;
    }

    // Coalesce adjacent free blocks after adding
    Block* current = free_list_head;
    while (current != NULL && current->next != NULL) {
        if (current->free && current->next->free &&
            (char*)current + current->size == (char*)current->next) {
            current->size += current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}


// Initialize the memory pool
void my_malloc_init(size_t pool_size) {
    if (memory_pool_base != NULL) {
        fprintf(stderr, "Error: my_malloc_init called more than once.\n");
        return;
    }

    // Allocate a large chunk of memory from the OS
    // Ensure pool_size is at least MIN_BLOCK_SIZE
    if (pool_size < MIN_BLOCK_SIZE) {
        pool_size = MIN_BLOCK_SIZE;
    }
    pool_size = align_size(pool_size); // Ensure the total pool size is also aligned

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
    if (bytes == 0) return NULL;

    // Adjust requested bytes to include header and ensure minimum size/alignment
    size_t total_size = bytes + sizeof(Block);
    if (total_size < MIN_BLOCK_SIZE) {
        total_size = MIN_BLOCK_SIZE;
    }
    total_size = align_size(total_size);

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
                new_block->next = current->next; // Maintain free list chain
                new_block->free = 1;             // Mark new block as free

                current->size = total_size;      // Current block takes requested size
                current->next = new_block;       // Current block now points to the new free block
            }

            current->free = 0; // Mark current block as allocated

            // Remove the allocated block from the free list
            if (prev == NULL) { // 'current' was the head
                free_list_head = current->next;
            } else {
                prev->next = current->next;
            }
            // The free block (if split) or the next free block is now current->next.

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
        block->free == 1) { // Also check if it's within bounds and not already free
        fprintf(stderr, "Error: my_free received an invalid or already freed pointer 0x%p.\n", ptr);
        return;
    }

    // Add block back to the free list, handling coalescing internally
    add_block_to_free_list(block);
}

// Custom realloc implementation
void* my_realloc(void* ptr, size_t new_size) {
    if (ptr == NULL) {
        return my_malloc(new_size);
    }
    if (new_size == 0) {
        my_free(ptr);
        return NULL;
    }

    Block* old_block = GET_BLOCK_HEADER(ptr);

    // Basic validation for old_block
    if ((char*)old_block < (char*)memory_pool_base ||
        (char*)old_block >= (char*)memory_pool_base + memory_pool_size ||
        old_block->free == 1) {
        fprintf(stderr, "Error: my_realloc received an invalid or freed pointer 0x%p.\n", ptr);
        return NULL;
    }

    size_t old_data_size = old_block->size - sizeof(Block);
    size_t aligned_new_total_size = align_size(new_size + sizeof(Block));

    // Case 1: New size is smaller or equal, and we can shrink in-place.
    // If the difference is big enough for a new free block.
    if (old_block->size >= aligned_new_total_size) {
        if (old_block->size - aligned_new_total_size >= MIN_BLOCK_SIZE) {
            // Shrink the block and create a new free block from the remainder
            Block* remainder_block = (Block*)((char*)old_block + aligned_new_total_size);
            remainder_block->size = old_block->size - aligned_new_total_size;
            old_block->size = aligned_new_total_size;
            
            // Add the remainder to the free list, which will also coalesce
            add_block_to_free_list(remainder_block);
        }
        // If the difference is too small for a new free block, we just keep the current block size.
        // Data stays in place.
        return ptr;
    }

    // Case 2: New size is larger. Try to expand in-place if possible.
    // Check if the next block is free and contiguous, and large enough to merge.
    Block* next_block = (Block*)((char*)old_block + old_block->size);
    int can_expand_in_place = 0;
    if ((char*)next_block < (char*)memory_pool_base + memory_pool_size) { // Ensure next_block is within our pool
        // We need to check if 'next_block' is indeed a valid block and free,
        // which means it *must* be in the free list.
        // To avoid iterating the free list directly here, we assume if it's contiguous and free in memory,
        // it *should* be free. A more robust solution would be to make 'Block' a doubly linked list
        // or have a more sophisticated free list lookup. For now, we simplify:
        // We look for 'next_block' *in the free list*. If it's there and adjacent, we can merge.
        Block* current_free = free_list_head;
        Block* prev_free = NULL;
        while(current_free != NULL) {
            if (current_free == next_block) {
                // Found the adjacent block in the free list
                if (current_free->free && old_block->size + current_free->size >= aligned_new_total_size) {
                    can_expand_in_place = 1;
                    break;
                }
            }
            prev_free = current_free;
            current_free = current_free->next;
        }

        if (can_expand_in_place) {
            // We can merge with next_block
            size_t combined_size = old_block->size + next_block->size;

            if (combined_size - aligned_new_total_size >= MIN_BLOCK_SIZE) {
                // Merge, then split the combined block
                Block* remainder_block = (Block*)((char*)old_block + aligned_new_total_size);
                remainder_block->size = combined_size - aligned_new_total_size;
                remainder_block->free = 1; // Mark as free

                old_block->size = aligned_new_total_size; // Old block takes new size

                // Remove next_block from free list, and add remainder_block
                // This is tricky. Easiest is to remove next_block, then call add_block_to_free_list for remainder
                if (prev_free == NULL) { // next_block was free_list_head
                    free_list_head = current_free->next;
                } else {
                    prev_free->next = current_free->next;
                }
                add_block_to_free_list(remainder_block);
            } else {
                // Merge completely, no split.
                old_block->size = combined_size;

                // Remove next_block from free list
                if (prev_free == NULL) { // next_block was free_list_head
                    free_list_head = current_free->next;
                } else {
                    prev_free->next = current_free->next;
                }
            }
            // Data is still in place, possibly extended.
            return ptr;
        }
    }

    // Case 3: Cannot expand in-place. Allocate new block, copy data, free old block.
    void* new_ptr = my_malloc(new_size);
    if (new_ptr == NULL) {
        return NULL; // Failed to allocate new memory
    }

    // Copy original data to new location
    size_t bytes_to_copy = (old_data_size < new_size) ? old_data_size : new_size;
    memcpy(new_ptr, ptr, bytes_to_copy);

    // Free the old block
    my_free(ptr);

    return new_ptr;
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

    printf("\n--- Malloc/Free Tests ---\n");
    char* p1 = (char*)my_malloc(100);
    if (p1) {
        sprintf(p1, "Hello from p1!");
        printf("p1 allocated at 0x%p (data), value: \"%s\"\n", p1, p1);
    }
    print_free_list();

    char* p2 = (char*)my_malloc(200);
    if (p2) {
        sprintf(p2, "Hello from p2!");
        printf("p2 allocated at 0x%p (data), value: \"%s\"\n", p2, p2);
    }
    print_free_list();

    char* p3 = (char*)my_malloc(50);
    if (p3) {
        sprintf(p3, "Hello from p3!");
        printf("p3 allocated at 0x%p (data), value: \"%s\"\n", p3, p3);
    }
    print_free_list();

    printf("\nFreeing p2 (200 bytes)...\n");
    my_free(p2);
    printf("p2 freed.\n");
    print_free_list();

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
    print_free_list();

    printf("\nFreeing p3 (50 bytes)...\n");
    my_free(p3);
    printf("p3 freed.\n");
    print_free_list();

    printf("\nFreeing p4 (150 bytes)...\n");
    my_free(p4);
    printf("p4 freed.\n");
    print_free_list();

    printf("\nTesting allocation failure (large request)...\n");
    char* p_large = (char*)my_malloc(100 * 1024); // Request more than 64KB pool
    if (p_large == NULL) {
        printf("Correctly failed to allocate 100KB.\n");
    }

    printf("\n--- Realloc Tests ---\n");
    char* r1 = (char*)my_malloc(20);
    if (r1) {
        sprintf(r1, "Realloc test 1");
        printf("r1 allocated at 0x%p, value: \"%s\"\n", r1, r1);
    }
    print_free_list();

    // Realloc to smaller size (in-place shrink)
    printf("\nRealloc r1 to 10 bytes (shrink)...\n");
    char* r1_shrunk = (char*)my_realloc(r1, 10);
    if (r1_shrunk) {
        printf("r1 reallocated to 0x%p, new size 10, value: \"%s\"\n", r1_shrunk, r1_shrunk);
        if (r1_shrunk == r1) {
            printf("  Shrink happened in-place.\n");
        } else {
            printf("  Shrink caused re-allocation.\n"); // Should be in-place
        }
    }
    print_free_list();

    // Realloc to slightly larger size, likely in-place if next block is free or enough space
    printf("\nRealloc r1 to 40 bytes (expand, maybe in-place)...\n");
    char* r1_expanded = (char*)my_realloc(r1_shrunk, 40);
    if (r1_expanded) {
        printf("r1 reallocated to 0x%p, new size 40, value: \"%s\"\n", r1_expanded, r1_expanded);
        if (r1_expanded == r1_shrunk) {
            printf("  Expansion happened in-place.\n");
        } else {
            printf("  Expansion caused re-allocation.\n");
        }
    }
    print_free_list();
    
    // Allocate another block to test realloc requiring move
    char* r2 = (char*)my_malloc(100);
    if (r2) {
        sprintf(r2, "This is r2, blocking expansion of r1.");
        printf("r2 allocated at 0x%p, value: \"%s\"\n", r2, r2);
    }
    print_free_list();

    // Realloc r1 to a much larger size, forcing a move
    printf("\nRealloc r1 to 500 bytes (force move)...\n");
    char* r1_moved = (char*)my_realloc(r1_expanded, 500);
    if (r1_moved) {
        printf("r1 reallocated to 0x%p, new size 500, value: \"%s\"\n", r1_moved, r1_moved);
        if (r1_moved != r1_expanded) {
            printf("  Realloc caused a move to new memory.\n");
        } else {
            printf("  Realloc happened in-place (unexpected for this size).\n");
        }
    }
    print_free_list();

    printf("\nFreeing r1_moved and r2...\n");
    my_free(r1_moved);
    my_free(r2);
    print_free_list();

    // Test realloc(NULL, size) -> malloc
    printf("\nTesting realloc(NULL, 30)...\n");
    char* r_null = (char*)my_realloc(NULL, 30);
    if (r_null) {
        sprintf(r_null, "Realloc NULL test.");
        printf("realloc(NULL, 30) allocated at 0x%p, value: \"%s\"\n", r_null, r_null);
    }
    print_free_list();

    // Test realloc(ptr, 0) -> free
    printf("\nTesting realloc(r_null, 0)...\n");
    char* r_zero = (char*)my_realloc(r_null, 0);
    if (r_zero == NULL) {
        printf("realloc(ptr, 0) correctly returned NULL and freed block.\n");
    }
    print_free_list();

    printf("\n--- End of test ---\n");
    my_malloc_cleanup();

    return 0;
}