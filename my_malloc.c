#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h> 
#include <pthread.h>


//allocate blocks of size { 2^(MIN_ORDER), 2^(MIN_ORDER + 1) ... 2^(MAX_ORDER) }  
#define MAX_ORDER 10
#define MIN_ORDER 5 // ( 2^MIN_ORDER ) >=  ( 2 * sizeof(void*)) ) 
#define POWER_2 20
#define TOTAL_SIZE (1 << POWER_2) // total bytes for user: (1 << POWER_2)
#define ALIGN 64 //  max( cache line size,  2^MIN_ORDER)




typedef struct {
    unsigned int value : 4; // 4 bits :  0 <= value <= 15
} BitField;                 // max(value) >= (MAX_ORDER - MIN_ORDER + 1)



typedef struct Block{
    Block* next;
    Block* prev;
}Block;



// order_blocks[x] = order :  if (1 <= order <= 6) , there is a free block of size 2^(order + 4) that starts with the addres_blocks of ((char*)x + init_addres_blocks)

// order_blocks[x] = order :  if (7 <= order <= 12) , A block that starts with the addres_blocks of ((char*)x + init_addres_blocks), with size of 2^(order - 2)  is not free 

// static BitField order_blocks[TOTAL_SIZE] = {0};    
static BitField* order_bitMap;   





// every list here contains free block that ready for allocation. 
// each block here contains information that helps to manage the memory 
// we use the memory that is in the free blocks to manage the memory
static Block* free_blocks_arrayOf_lists[MAX_ORDER + 1];

char* init_addres_blocks;
static char* original_malloc_adress;
static bool is_init = false;

pthread_mutex_t memory_mutex = PTHREAD_MUTEX_INITIALIZER; // PTHREAD_MUTEX_INITIALIZER : it is properly initialized before it is used


void init_malloc_frag(){
    
    for(int i = 0; i < (MAX_ORDER + 1); i++){
        free_blocks_arrayOf_lists[i] = NULL;
    }
      
    size_t extra = (ALIGN - 1);
    original_malloc_adress = (char*)malloc( (sizeof(char) * TOTAL_SIZE) + (sizeof(BitField) * TOTAL_SIZE) + extra); //ad
    init_addres_blocks = (char*)(((uintptr_t)original_malloc_adress + extra) & (~(ALIGN - 1)));
    if(!init_addres_blocks){
        pthread_mutex_lock(&memory_mutex);
        fprintf(stderr, "Failed to allocate memory pool\n");
        exit(1); 
    }
    
    order_bitMap = (BitField*)(init_addres_blocks + (TOTAL_SIZE * sizeof(char)));

    

    int num_blocks_order_10 = TOTAL_SIZE >> MAX_ORDER;
    char* start_blocks_addres_blocks = init_addres_blocks;
    for(int i = 0; i < (TOTAL_SIZE >> MAX_ORDER); i++){  // ( TOTAL_SIZE >> MAX_ORDER ) = num of blocks with order of MAX_ORDER that covvered TOTAL_SIZE of memory
        
        //add a new free block with order of MAX_ORDER which begin at the adres_blocks : i*(1 << MAX_ORDER)
        BitField order_block;
        order_block.value = MAX_ORDER - 4;
        order_bitMap[ i*(1 << MAX_ORDER ) ] = order_block;
        
        //create this block and add it to the list of free blocks with order of MAX_ORDER
        Block* block = (init_addres_blocks + i*(1 << MAX_ORDER ));        
        block->next = free_blocks_arrayOf_lists[MAX_ORDER];
        block->prev = NULL;
        
        if(free_blocks_arrayOf_lists[MAX_ORDER] != NULL){
            free_blocks_arrayOf_lists[MAX_ORDER]->prev = block;
        }                 
        free_blocks_arrayOf_lists[MAX_ORDER] = block;
       
  
    }
    

}








void* malloc_frag(size_t size){
    
    pthread_mutex_lock(&memory_mutex);
    if(!is_init){
        init_malloc_frag();
        is_init = true;
    }
        
    //searching for the minimal order that bigger than 'size' 
    int order = MIN_ORDER;
    while((1 << order) < size){
        order++;
    }

    Block* res_block;
    bool mem_found = false;
    int order_of_block_found = -1; 

    // searching for a free block with the minimal order that is needed or bigger
    for(int i = order; i < (MAX_ORDER + 1); i++){
        if(free_blocks_arrayOf_lists[i] != NULL){
            
            BitField order_block;
            order_block.value = order + 2; // block became used/not-free so its order is (7 <= order <= 12)
            order_bitMap[free_blocks_arrayOf_lists[i] - init_addres_blocks] = order_block; // (free_blocks_arrayOf_lists[i] - init_addres_blocks) == the start byte of this block if we count from 0
            res_block = free_blocks_arrayOf_lists[i];
            free_blocks_arrayOf_lists[i] = res_block->next;
            free_blocks_arrayOf_lists[i]->prev = NULL;
            mem_found = true;
            order_of_block_found = i;  
            break;
            }
        }

    if(!mem_found){
         pthread_mutex_unlock(&memory_mutex);
        return NULL; //non block has been found
    }
        



    //split the block if its more memory than the user asked for
    int x = 1;
    while((order_of_block_found - x) >= order){ 
       //create new block with order of (order_of_block_found - x)
       Block* new_block =  (Block*)( res_block + (1 << (order_of_block_found - x)) );

       //update the bit-map that there is a new free block with order of (order_of_block_found - x)
       BitField order_block;
       order_block.value = (order_of_block_found - x) - 4;  //new block ready for using so its order is (1 <= order <= 6)
       order_bitMap[( res_block + (1 << order_of_block_found - x) ) - init_addres_blocks] = order_block;

       //add this new block to the free list blocks of order (order_of_block_found - x)
       new_block->prev = NULL;
       new_block->next = free_blocks_arrayOf_lists[order_of_block_found - x];
       free_blocks_arrayOf_lists[order_of_block_found - x]->prev = new_block;
       free_blocks_arrayOf_lists[order_of_block_found - x] = new_block;
       x++;        
    }
    pthread_mutex_unlock(&memory_mutex);
    return res_block;
}






void* free_malloc_frag(void* ptr){
    pthread_mutex_lock(&memory_mutex);
    size_t returned_adress_indxOf_bitMap = (char*)ptr - init_addres_blocks;
    BitField order_block = order_bitMap[returned_adress_indxOf_bitMap];
    int returned_block_order = order_block.value - 2;

    
    //we go (1 << returned_block_order) bytes right/left to returned block and check its order by the bit-map that tells us:
    //A. if there is a block that start with this adress 
    //B. if yes, what its order
    //C. if yes, does it a free or used block
    
    size_t left_adress_indxOf_bitMap = ((char*)ptr - (1 << returned_block_order)) - (char*)init_addres_blocks;
    size_t right_adress_indxOf_bitMap = ((char*)ptr + (1 << returned_block_order)) - (char*)init_addres_blocks;
    
    //checking if there is a free block with the same order left to the returned block - left block and returned block merged/into to the left block
    if( (order_bitMap[ left_adress_indxOf_bitMap].value >= 1) && (order_bitMap[ left_adress_indxOf_bitMap].value <= 6) ){
        
        int left_block_order = (order_bitMap[ left_adress_indxOf_bitMap].value) + 4;
        if(left_block_order == returned_block_order){
            //update the adress of the left-block at the bit-map to represent free block with order of (returned_block_order + 1 )
            BitField order_block;
            order_block.value = (returned_block_order + 1) - 4; 
            order_bitMap[left_adress_indxOf_bitMap] = order_block;
            
            Block* left_block = (Block*)((char*)ptr - (1 << returned_block_order));
            if(left_block->prev != NULL && left_block->next != NULL){
                (left_block->prev)->next = left_block->next;
                (left_block->next)->prev = left_block->prev;

            }
            //left block is the end block of the list
            else if(left_block->prev != NULL && left_block->next == NULL){
                 (left_block->prev)->next = NULL;
            }
            //left block is the head of the list
            else{
                (left_block->next)->prev = NULL;
                free_blocks_arrayOf_lists[left_block_order] = left_block->next;

            }
            Block* new_block = left_block;
            free_blocks_arrayOf_lists[returned_block_order + 1]->prev = new_block;
            new_block->prev = NULL;
            new_block->next = free_blocks_arrayOf_lists[returned_block_order + 1];
            free_blocks_arrayOf_lists[returned_block_order + 1] = new_block;

            //update the bit-map : non block starts at this adress 
            order_bitMap[returned_adress_indxOf_bitMap].value = 0;

        }

    }


    //checking if there is a free block with the same order right to the returned block - right block and returned block merged/into to the returned block
    else if( (order_bitMap[ right_adress_indxOf_bitMap].value >= 1) && (order_bitMap[ right_adress_indxOf_bitMap].value <= 6) ){
        
        int right_block_order = (order_bitMap[ right_adress_indxOf_bitMap].value) + 4;
        if(right_block_order == returned_block_order){
            //update the adress of the returned-block at the bit-map to represent free block with order of (returned_block_order + 1 )
            BitField order_block;
            order_block.value = (returned_block_order + 1) - 4; 
            order_bitMap[returned_adress_indxOf_bitMap] = order_block;

            Block* right_block = (Block*)((char*)ptr - (1 << returned_block_order));
            if(right_block->prev != NULL && right_block->next != NULL){
                (right_block->prev)->next = right_block->next;
                (right_block->next)->prev = right_block->prev;

            }
            //left block is the end block of the list
            else if(right_block->prev != NULL && right_block->next == NULL){
                 (right_block->prev)->next = NULL;
            }
            //left block is the head of the list
            else{
                (right_block->next)->prev = NULL;
                free_blocks_arrayOf_lists[right_block_order] = right_block->next;

            }

            Block* new_block = (Block*)ptr;
            free_blocks_arrayOf_lists[returned_block_order + 1]->prev = new_block;
            new_block->prev = NULL;
            new_block->next = free_blocks_arrayOf_lists[returned_block_order + 1];
            free_blocks_arrayOf_lists[returned_block_order + 1] = new_block;

            //update the bit-map : non block starts at this adress
            order_bitMap[right_adress_indxOf_bitMap].value = 0;


        }

    }

    //there are no free blocks  with the same order at the left and the right of the returned block
    else{
        
        //update the adress of the returned-block at the bit-map to represent free block with the same order 
        BitField order_block;
        order_block.value = returned_block_order - 4; 
        order_bitMap[returned_adress_indxOf_bitMap] = order_block;

        Block* new_block = (Block*)ptr;
        free_blocks_arrayOf_lists[returned_block_order - 4]->prev = new_block;
        new_block->prev = NULL;
        new_block->next = free_blocks_arrayOf_lists[returned_block_order - 4];
        free_blocks_arrayOf_lists[returned_block_order - 4] = new_block;

    }
    pthread_mutex_unlock(&memory_mutex);

}

