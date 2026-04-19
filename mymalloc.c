#include <windows.h>
#include <stdio.h>
#include <stdlib.h> // For size_t, memcpy, NULL

// Define custom block structure
// We'll put a magic number at the start and end of the block for integrity checks
#define BLOCK_MAGIC_START 0xDEADBEEF
#define BLOCK_MAGIC_END   0xBEEFDEAD

typedef struct Block {
    size_t size;          // Total size of the block, including Block header, magic_end and user data
    struct Block* next;   // Pointer to the next free block in the free list
    int free;             // 1 if free, 0 if allocated
    unsigned int magic_start; // Magic number to detect header corruption
    // User data follows immediately after this struct
} Block;

// Global head of the free list
static Block* free_list_head = NULL;

// Base address of the memory pool managed by our allocator
static LPVOID memory_pool_base = NULL;
static size_t memory_pool_size = 0;

// Minimum block size (to accommodate the Block header, magic_end, and some minimal payload space/alignment)
#define MIN_USER_DATA_SIZE 8 // Minimum bytes for user data
#define BLOCK_HEADER_OVERHEAD (sizeof(Block) + sizeof(unsigned int))
#define MIN_BLOCK_SIZE (((BLOCK_HEADER_OVERHEAD + MIN_USER_DATA_SIZE) + 7) & ~7)

// Helper function to get block header from data pointer
#define GET_BLOCK_HEADER(ptr) ((Block*)((char*)ptr - sizeof(Block)))

// Helper to get the address of the magic_end for a given block
#define GET_BLOCK_MAGIC_END_PTR(block_ptr) ((unsigned int*)((char*)block_ptr + block_ptr->size - sizeof(unsigned int)))

// Helper to ensure size is a multiple of 8 for alignment
static size_t align_size(size_t s) {
    return (s + 7) & ~7;
}

// Helper to check block integrity (magic numbers)
static int check_block_integrity(Block* block, const char* caller_func) {
    if (block->magic_start != BLOCK_MAGIC_START) {
        fprintf(stderr, "Heap corruption detected (HEADER MAGIC) in %s! Block 0x%p, expected 0x%X, got 0x%X.\n",
                caller_func, block, BLOCK_MAGIC_START, block->magic_start);
        return 0;
    }
    unsigned int* magic_end_ptr = GET_BLOCK_MAGIC_END_PTR(block);
    if (*magic_end_ptr != BLOCK_MAGIC_END) {
        fprintf(stderr, "Heap corruption detected (FOOTER MAGIC) in %s! Block 0x%p, expected 0x%X, got 0x%X.\n",
                caller_func, block, BLOCK_MAGIC_END, *magic_end_ptr);
        return 0;
    }
    return 1;
}

// Helper to add a block to the free list, maintaining sorted order and coalescing
static void add_block_to_free_list(Block* new_free_block) {
    if (new_free_block == NULL) return;

    // Mark as free and set magic numbers (in case it was just freed)
    new_free_block->free = 1;
    new_free_block->magic_start = BLOCK_MAGIC_START;
    *GET_BLOCK_MAGIC_END_PTR(new_free_block) = BLOCK_MAGIC_END;

    Block* current = free_list_head;
    Block* prev = NULL;

    // Find insertion point to keep the free list sorted by address
    while (current != NULL && current < new_free_block) {
        prev = current;
        current = current->next;
    }

    // Insert the new_free_block
    if (prev == NULL) { // Insert at head
        new_free_block->next = free_list_head;
        free_list_head = new_free_block;
    } else {
        new_free_block->next = prev->next;
        prev->next = new_free_block;
    }

    // Coalesce with previous block if possible
    if (prev != NULL && prev->free && (char*)prev + prev->size == (char*)new_free_block) {
        if (!check_block_integrity(prev, "add_block_to_free_list (coalesce_prev)")) return;
        prev->size += new_free_block->size;
        prev->next = new_free_block->next; // Skip new_free_block
        new_free_block = prev; // Continue coalescing with the new merged block
    }

    // Coalesce with next block if possible
    if (new_free_block->next != NULL && new_free_block->next->free &&
        (char*)new_free_block + new_free_block->size == (char*)new_free_block->next) {
        if (!check_block_integrity(new_free_block, "add_block_to_free_list (coalesce_next)")) return;
        if (!check_block_integrity(new_free_block->next, "add_block_to_free_list (coalesce_next_target)")) return;

        new_free_block->size += new_free_block->next->size;
        new_free_block->next = new_free_block->next->next;
    }
}


// Initialize the memory pool
void my_malloc_init(size_t pool_size) {
    if (memory_pool_base != NULL) {
        fprintf(stderr, "Error: my_malloc_init called more than once.\n");
        return;
    }

    // Ensure pool_size is at least MIN_BLOCK_SIZE and aligned
    if (pool_size < MIN_BLOCK_SIZE) {
        pool_size = MIN_BLOCK_SIZE;
    }
    pool_size = align_size(pool_size);

    memory_pool_base = VirtualAlloc(
        NULL,
        pool_size,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    );

    if (memory_pool_base == NULL) {
        fprintf(stderr, "Error: VirtualAlloc failed with error %lu\n", GetLastError());
        return;
    }

    memory_pool_size = pool_size;
    printf("Memory pool initialized: Base address 0x%p, size %zu bytes.\n", memory_pool_base, pool_size);

    // Initialize the entire pool as one large free block
    Block* initial_block = (Block*)memory_pool_base;
    initial_block->size = memory_pool_size;
    initial_block->next = NULL;
    // Call add_block_to_free_list to properly initialize state and magic numbers
    add_block_to_free_list(initial_block);
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

    // Adjust requested bytes to include header, magic_end, and ensure minimum size/alignment
    size_t total_block_size = bytes + BLOCK_HEADER_OVERHEAD;
    if (total_block_size < MIN_BLOCK_SIZE) {
        total_block_size = MIN_BLOCK_SIZE;
    }
    total_block_size = align_size(total_block_size);

    Block* current = free_list_head;
    Block* prev = NULL;

    while (current != NULL) {
        if (!check_block_integrity(current, "my_malloc (free list traversal)")) {
            // Corruption detected, abort or try to skip
            fprintf(stderr, "Skipping corrupted free block 0x%p in my_malloc.\n", current);
            // This is a difficult situation. For now, we'll just skip and hope for the best.
            // A real allocator might panic or attempt recovery.
            prev = current;
            current = current->next;
            continue;
        }

        if (current->free && current->size >= total_block_size) {
            // Found a suitable free block

            // Check if we can split the block
            if (current->size - total_block_size >= MIN_BLOCK_SIZE) {
                // Split the block
                Block* new_free_block = (Block*)((char*)current + total_block_size);
                new_free_block->size = current->size - total_block_size;
                new_free_block->free = 1;
                // Add new_free_block to the free list immediately to ensure it's properly handled
                // We're essentially moving the remainder to its correct place in the sorted free list.
                // First, remove 'current' (potentially resized) from the free list.
                if (prev == NULL) { // 'current' was the head
                    free_list_head = current->next;
                } else {
                    prev->next = current->next;
                }
                // Now current is no longer in the free list chain.
                // Insert new_free_block (the remainder) back into the free list.
                // This will re-sort and coalesce automatically.
                add_block_to_free_list(new_free_block);

                current->size = total_block_size;      // Current block takes requested size
                current->next = NULL;                  // Allocated blocks don't participate in free list chain directly
            } else {
                // No split, consume the entire block. Remove it from the free list.
                if (prev == NULL) { // 'current' was the head
                    free_list_head = current->next;
                } else {
                    prev->next = current->next;
                }
                current->next = NULL; // Allocated blocks don't have 'next' in free list context
            }

            current->free = 0; // Mark current block as allocated
            current->magic_start = BLOCK_MAGIC_START;
            *GET_BLOCK_MAGIC_END_PTR(current) = BLOCK_MAGIC_END;

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
        (char*)block >= (char*)memory_pool_base + memory_pool_size) {
        fprintf(stderr, "Error: my_free received a pointer 0x%p outside managed pool.\n", ptr);
        return;
    }
    if (block->free == 1) { // Check if it's already free
        fprintf(stderr, "Error: my_free received an already freed pointer 0x%p.\n", ptr);
        return;
    }

    // Check for heap corruption before freeing
    if (!check_block_integrity(block, "my_free")) {
        return; // Abort free operation if corruption is detected
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
        (char*)old_block >= (char*)memory_pool_base + memory_pool_size) {
        fprintf(stderr, "Error: my_realloc received a pointer 0x%p outside managed pool.\n", ptr);
        return NULL;
    }
    if (old_block->free == 1) {
        fprintf(stderr, "Error: my_realloc received an already freed pointer 0x%p.\n", ptr);
        return NULL;
    }

    // Check for heap corruption before reallocating
    if (!check_block_integrity(old_block, "my_realloc")) {
        return NULL; // Abort realloc operation if corruption is detected
    }

    size_t old_user_data_size = old_block->size - BLOCK_HEADER_OVERHEAD;
    size_t aligned_new_total_block_size = align_size(new_size + BLOCK_HEADER_OVERHEAD);

    // Case 1: New size is smaller or equal, and we can shrink in-place.
    if (old_block->size >= aligned_new_total_block_size) {
        if (old_block->size - aligned_new_total_block_size >= MIN_BLOCK_SIZE) {
            // Shrink the block and create a new free block from the remainder
            Block* remainder_block = (Block*)((char*)old_block + aligned_new_total_block_size);
            remainder_block->size = old_block->size - aligned_new_total_block_size;
            old_block->size = aligned_new_total_block_size;
            
            // Re-set magic numbers for the (shrunk) allocated block
            old_block->magic_start = BLOCK_MAGIC_START;
            *GET_BLOCK_MAGIC_END_PTR(old_block) = BLOCK_MAGIC_END;

            // Add the remainder to the free list, which will also coalesce
            add_block_to_free_list(remainder_block);
        }
        // If the difference is too small for a new free block, we just keep the current block size.
        // Data stays in place. Magic numbers already correct.
        return ptr;
    }

    // Case 2: New size is larger. Try to expand in-place if possible.
    Block* next_block_candidate = (Block*)((char*)old_block + old_block->size);

    // Check if next_block_candidate is within the pool bounds and is free
    if ((char*)next_block_candidate >= (char*)memory_pool_base + memory_pool_size ||
        !next_block_candidate->free) {
        goto allocate_new_block_and_copy; // Not free or out of bounds, can't expand in-place
    }

    // Found a free block immediately after the current one.
    // Check its integrity before proceeding.
    if (!check_block_integrity(next_block_candidate, "my_realloc (next_block_candidate)")) {
        goto allocate_new_block_and_copy; // Next block is corrupted, can't use it
    }

    // Calculate combined size
    size_t combined_size = old_block->size + next_block_candidate->size;

    if (combined_size >= aligned_new_total_block_size) {
        // We can expand in-place by merging with next_block_candidate.

        // Remove next_block_candidate from the free list
        Block* current_free_ptr = free_list_head;
        Block* prev_free_ptr = NULL;
        while(current_free_ptr != NULL && current_free_ptr != next_block_candidate) {
            prev_free_ptr = current_free_ptr;
            current_free_ptr = current_free_ptr->next;
        }

        if (current_free_ptr == next_block_candidate) { // Found and it's in the free list
            if (prev_free_ptr == NULL) { // next_block_candidate was free_list_head
                free_list_head = current_free_ptr->next;
            } else {
                prev_free_ptr->next = current_free_ptr->next;
            }
        } else {
            // This should ideally not happen if next_block_candidate->free was true,
            // but means it's not properly linked in free_list_head.
            // For robustness, proceed to allocate new.
            goto allocate_new_block_and_copy;
        }

        if (combined_size - aligned_new_total_block_size >= MIN_BLOCK_SIZE) {
            // Merge and then split the combined block
            Block* remainder_block = (Block*)((char*)old_block + aligned_new_total_block_size);
            remainder_block->size = combined_size - aligned_new_total_block_size;
            
            old_block->size = aligned_new_total_block_size; // Old block takes new size

            // Add the remainder block back to the free list
            add_block_to_free_list(remainder_block);
        } else {
            // Merge completely, no split.
            old_block->size = combined_size;
        }
        
        // Re-set magic numbers for the (expanded) allocated block
        old_block->magic_start = BLOCK_MAGIC_START;
        *GET_BLOCK_MAGIC_END_PTR(old_block) = BLOCK_MAGIC_END;

        return ptr; // Data is still in place, possibly extended.
    }

allocate_new_block_and_copy:;   // ← semicolon turns it into an empty statement
    void* new_ptr = my_malloc(new_size);
    if (new_ptr == NULL) {
        return NULL; // Failed to allocate new memory
    }

    // Copy original data to new location
    size_t bytes_to_copy = (old_user_data_size < new_size) ? old_user_data_size : new_size;
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

    printf("\nTesting allocation failure (large request)....\n");
    char* p_large = (char*)my_malloc(100 * 1024); // Request more than 64KB pool
    if (p_large == NULL) {
        printf("Correctly failed to allocate 100KB.\n");
    }
    print_free_list();

    printf("\n--- Realloc Tests ---\n");
    char* r1 = (char*)my_malloc(50);
    if (r1) {
        sprintf(r1, "Realloc test 1 long data string!");
        printf("r1 allocated at 0x%p (data), value: \"%s\"\n", r1, r1);
    }
    print_free_list();

    // Realloc to smaller size (in-place shrink)
    printf("\nRealloc r1 to 10 bytes (shrink)...\n");
    char* r1_shrunk = (char*)my_realloc(r1, 10);
    if (r1_shrunk) {
        printf("r1 reallocated to 0x%p (data), new size 10, value: \"%.10s\"\n", r1_shrunk, r1_shrunk);
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
        printf("r1 reallocated to 0x%p (data), new size 40, value: \"%s\"\n", r1_expanded, r1_expanded);
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
        printf("r2 allocated at 0x%p (data), value: \"%s\"\n", r2, r2);
    }
    print_free_list();

    // Realloc r1 to a much larger size, forcing a move
    printf("\nRealloc r1 to 500 bytes (force move)...\n");
    char* r1_moved = (char*)my_realloc(r1_expanded, 500);
    if (r1_moved) {
        printf("r1 reallocated to 0x%p (data), new size 500, value: \"%s\"\n", r1_moved, r1_moved);
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
        printf("realloc(NULL, 30) allocated at 0x%p (data), value: \"%s\"\n", r_null, r_null);
    }
    print_free_list();

    // Test realloc(ptr, 0) -> free
    printf("\nTesting realloc(r_null, 0)...\n");
    char* r_zero = (char*)my_realloc(r_null, 0);
    if (r_zero == NULL) {
        printf("realloc(ptr, 0) correctly returned NULL and freed block.\n");
    }
    print_free_list();

    printf("\n--- Corruption Detection Tests (expecting errors) ---\n");
    char* corrupt_ptr = (char*)my_malloc(20);
    if (corrupt_ptr) {
        sprintf(corrupt_ptr, "Original content");
        printf("Allocated block for corruption test at 0x%p (data), value: \"%s\"\n", corrupt_ptr, corrupt_ptr);

        // Corrupt the header magic
        printf("Attempting to corrupt HEADER MAGIC...\n");
        Block* corrupt_block_hdr = GET_BLOCK_HEADER(corrupt_ptr);
        corrupt_block_hdr->magic_start = 0xBAD;
        my_free(corrupt_ptr); // Should detect header corruption

        // Reallocate and corrupt footer magic
        corrupt_ptr = (char*)my_malloc(20);
        if (corrupt_ptr) {
            sprintf(corrupt_ptr, "Another content");
            printf("Allocated another block for corruption test at 0x%p (data), value: \"%s\"\n", corrupt_ptr, corrupt_ptr);
            printf("Attempting to corrupt FOOTER MAGIC (overrun)...\n");
            unsigned int* magic_end_ptr = GET_BLOCK_MAGIC_END_PTR(GET_BLOCK_HEADER(corrupt_ptr));
            *magic_end_ptr = 0xBAD; // Corrupt the footer
            my_free(corrupt_ptr); // Should detect footer corruption
        }
    }
    print_free_list();


    printf("\n--- End of test ---\n");
    my_malloc_cleanup();

    return 0;
}
