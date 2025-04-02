#include <assert.h>
#include <stdio.h>
#include <omp.h>
#include <stdlib.h>

#define NUM_PROCS 4
#define CACHE_SIZE 4
#define MEM_SIZE 16
#define MSG_BUFFER_SIZE 256
#define MAX_INSTR_NUM 32

typedef unsigned char byte;

typedef enum { MODIFIED, EXCLUSIVE, SHARED, INVALID } cacheLineState;

// In addition to cache line states, each directory entry also has a state
// EM ( exclusive or modified ) : memory block is in only one cache.
//                  When a memory block in a cache is written to, it would have
//                  resulted in cache hit and transition from EXCLUSIVE to MODIFIED.
//                  Main memory / directory has no way of knowing of this transition.
//                  Hence, this state only cares that the memory block is in a single
//                  cache, and not whether its in EXCLUSIVE or MODIFIED.
// S ( shared )     : memory block is in multiple caches
// U ( unowned )    : memory block is not in any cache
typedef enum { EM, S, U } directoryEntryState;

typedef enum { 
    READ_REQUEST,       // requesting node sends to home node on a read miss 
    WRITE_REQUEST,      // requesting node sends to home node on a write miss 
    REPLY_RD,           // home node replies with data to requestor for read request
    REPLY_WR,           // home node replies to requestor for write request
    REPLY_ID,           // home node replies with IDs of sharers to requestor
    INV,                // owner node asks sharers to invalidate
    UPGRADE,            // owner node asks home node to change state to EM
    WRITEBACK_INV,      // home node asks owner node to flush and change to INVALID
    WRITEBACK_INT,      // home node asks owner node to flush and change to SHARED
    FLUSH,              // owner flushes data to home + requestor
    FLUSH_INVACK,       // flush, piggybacking an InvAck message
    EVICT_SHARED,       // handle cache replacement of a shared cache line
    EVICT_MODIFIED      // handle cache replacement of a modified cache line
} transactionType;

// We will create our own address space which will be of size 1 byte
// LSB 4 bits indicate the location in memory
// MSB 4 bits indicate the processor it is present in.
// For example, 0x36 means the memory block at index 6 in the 3rd processor
typedef struct instruction {
    byte type;      // 'R' for read, 'W' for write
    byte address;
    byte value;     // used only for write operations
} instruction;

typedef struct cacheLine {
    byte address;           // this is the address in memory
    byte value;             // this is the value stored in cached memory
    cacheLineState state;   // state for you to implement MESI protocol
} cacheLine;

typedef struct directoryEntry {
    byte bitVector;         // each bit indicates whether that processor has this
                            // memory block in its cache
    directoryEntryState state;
} directoryEntry;

// Note that each message will contain values only in the fields which are relevant 
// to the transactionType
typedef struct message {
    transactionType type;
    int sender;          // thread id that sent the message
    byte address;        // memory block address
    byte value;          // value in memory / cache
    byte bitVector;      // ids of sharer nodes
    int secondReceiver;  // used when you need to send a message to 2 nodes, where
                         // 1 node id is in the sender field
    directoryEntryState dirState;   // directory entry state of the memory block
} message;

typedef struct messageBuffer {
    message queue[ MSG_BUFFER_SIZE ];
    // a circular queue message buffer
    int head;
    int tail;
    int count;          // store total number of messages processed by the node
} messageBuffer;

typedef struct processorNode {
    cacheLine cache[ CACHE_SIZE ];
    byte memory[ MEM_SIZE ];
    directoryEntry directory[ MEM_SIZE ];
    instruction instructions[ MAX_INSTR_NUM ];
    int instructionCount;
} processorNode;

void initializeProcessor( int threadId, processorNode *node, char *dirName );
void sendMessage( int receiver, message msg );  // IMPLEMENT
void handleCacheReplacement( int sender, cacheLine oldCacheLine );  // IMPLEMENT
void printProcessorState( int processorId, processorNode node );

messageBuffer messageBuffers[ NUM_PROCS ];
// IMPLEMENT
// Create locks to ensure thread-safe access to each processor's message buffer.
omp_lock_t msgBufferLocks[ NUM_PROCS ];

int main( int argc, char * argv[] ) {
    if (argc < 2) {
        fprintf( stderr, "Usage: %s <test_directory>\n", argv[0] );
        return EXIT_FAILURE;
    }
    char *dirName = argv[1];
    
    // IMPLEMENT
    // set number of threads to NUM_PROCS
    omp_set_num_threads(NUM_PROCS);

    for ( int i = 0; i < NUM_PROCS; i++ ) {
        messageBuffers[ i ].count = 0;
        messageBuffers[ i ].head = 0;
        messageBuffers[ i ].tail = 0;
        // IMPLEMENT
        // initialize the locks in msgBufferLocks
        omp_init_lock(&msgBufferLocks[i]);
    }

    // IMPLEMENT
    // Create the omp parallel region with an appropriate data environment
    #pragma omp parallel
    {
        processorNode node;
        int threadId = omp_get_thread_num();
        initializeProcessor( threadId, &node, dirName );
        // IMPLEMENT
        // wait for all processors to complete initialization before proceeding 
        #pragma omp barrier

        message msg;
        message msgReply;
        instruction instr;
        int instructionIdx = -1;
        int printProcState = 1;         // control how many times to dump processor
        byte waitingForReply = 0;       // if a processor is waiting for a reply
                                        // then dont proceed to next instruction
                                        // as current instruction has not finished
        while ( 1 ) {
            // Process all messages in message queue first
            while ( 
                messageBuffers[ threadId ].count > 0 &&
                messageBuffers[ threadId ].head != messageBuffers[ threadId ].tail
            ) {
                if ( printProcState == 0 ) {
                    printProcState++;
                }
                int head = messageBuffers[ threadId ].head;
                msg = messageBuffers[ threadId ].queue[ head ];
                messageBuffers[ threadId ].head = ( head + 1 ) % MSG_BUFFER_SIZE;

                #ifdef DEBUG_MSG
                printf( "Processor %d msg from: %d, type: %d, address: 0x%02X\n",
                        threadId, msg.sender, msg.type, msg.address );
                #endif /* ifdef DEBUG */

                // IMPLEMENT
                // extract procNodeAddr and memBlockAddr from message address
                byte procNodeAddr = msg.address >> 4;
                byte memBlockAddr = msg.address & 0x0F;
                byte cacheIndex = memBlockAddr % CACHE_SIZE;

                switch (msg.type) {
                    case READ_REQUEST: {
                        byte homeProc = msg.address >> 4;
                        byte memBlockAddr = msg.address & 0x0F;
                        
                        if (threadId == homeProc) {  // Only home node processes directory requests
                            if (node.directory[memBlockAddr].state == U) {
                                node.directory[memBlockAddr].bitVector = 1 << msg.sender;
                                node.directory[memBlockAddr].state = EM;
                                message reply = {REPLY_RD, threadId, msg.address, node.memory[memBlockAddr], 0, -1, EM};
                                sendMessage(msg.sender, reply);
                            } 
                            else if (node.directory[memBlockAddr].state == S) {
                                node.directory[memBlockAddr].bitVector |= 1 << msg.sender;
                                message reply = {REPLY_RD, threadId, msg.address, node.memory[memBlockAddr], 0, -1, S};
                                sendMessage(msg.sender, reply);
                            } 
                            else if (node.directory[memBlockAddr].state == EM) {
                                int owner = __builtin_ffs(node.directory[memBlockAddr].bitVector) - 1;
                                message wb_int = {WRITEBACK_INT, threadId, msg.address, 0, 0, msg.sender, EM};
                                sendMessage(owner, wb_int);
                            }
                        }
                        break;
                    }
                
                    case REPLY_RD: {
                        byte memBlockAddr = msg.address & 0x0F;
                        byte cacheIndex = memBlockAddr % CACHE_SIZE;
                        
                        if (node.cache[cacheIndex].address != 0xFF && node.cache[cacheIndex].state != INVALID) {
                            handleCacheReplacement(threadId, node.cache[cacheIndex]);
                        }
                        node.cache[cacheIndex].address = msg.address;
                        node.cache[cacheIndex].value = msg.value;
                        // If data came from memory, ensure it matches the home node's value
                        // if (msg.dirState == EM || msg.dirState == S) {
                        // node.cache[cacheIndex].value = node.memory[memBlockAddr];
                        // }
                        node.cache[cacheIndex].state = (msg.dirState == EM) ? EXCLUSIVE : SHARED;
                        waitingForReply = 0;
                        break;
                    }
                
                    case WRITEBACK_INT: {
                        byte memBlockAddr = msg.address & 0x0F;
                        byte cacheIndex = memBlockAddr % CACHE_SIZE;
                        
                        if (node.cache[cacheIndex].address == msg.address && 
                            (node.cache[cacheIndex].state == MODIFIED || node.cache[cacheIndex].state == EXCLUSIVE)) {
                            message flush = {FLUSH, threadId, msg.address, node.cache[cacheIndex].value, 0, msg.secondReceiver, EM};
                            sendMessage(msg.address >> 4, flush);  // Send to home node
                            if ((msg.address >> 4) != msg.secondReceiver) {
                                sendMessage(msg.secondReceiver, flush);  // Send to requestor
                            }
                            node.cache[cacheIndex].state = SHARED;
                        }
                        break;
                    }
                
                    case FLUSH: {
                        byte memBlockAddr = msg.address & 0x0F;
                        byte homeProc = msg.address >> 4;
                        
                        if (threadId == homeProc) {
                            node.memory[memBlockAddr] = msg.value;  // Update memory
                            // Reset bitVector and then set the bits for sender and receiver
                            node.directory[memBlockAddr].bitVector = (1 << msg.sender); // Change from |= to =
                            if (msg.secondReceiver != -1) {
                                node.directory[memBlockAddr].bitVector |= (1 << msg.secondReceiver);
                            }
                            // Always set to S state when shared between sender and receiver
                            node.directory[memBlockAddr].state = S;
                        }
                        if (threadId == msg.secondReceiver) {
                            byte cacheIndex = memBlockAddr % CACHE_SIZE;
                            if (node.cache[cacheIndex].address != 0xFF && node.cache[cacheIndex].state != INVALID) {
                                handleCacheReplacement(threadId, node.cache[cacheIndex]);
                            }
                            node.cache[cacheIndex].address = msg.address;
                            node.cache[cacheIndex].value = msg.value;
                            node.cache[cacheIndex].state = SHARED;
                        }
                        break;
                    }
                
                    case UPGRADE: {
                        byte memBlockAddr = msg.address & 0x0F;
                        byte homeProc = msg.address >> 4;
                        
                        if (threadId == homeProc && node.directory[memBlockAddr].state == S) {
                            message reply_id = {REPLY_ID, threadId, msg.address, 0, 
                                              node.directory[memBlockAddr].bitVector & ~(1 << msg.sender), 
                                              -1, EM};
                            sendMessage(msg.sender, reply_id);
                            node.directory[memBlockAddr].bitVector = 1 << msg.sender;
                            node.directory[memBlockAddr].state = EM;
                        }
                        break;
                    }
                
                    case REPLY_ID: {
                        byte memBlockAddr = msg.address & 0x0F;
                        byte cacheIndex = memBlockAddr % CACHE_SIZE;
                        
                        for (int i = 0; i < NUM_PROCS; i++) {
                            if (msg.bitVector & (1 << i)) {
                                message inv = {INV, threadId, msg.address, 0, 0, -1, U};
                                sendMessage(i, inv);
                            }
                        }
                        if (node.cache[cacheIndex].address != msg.address || node.cache[cacheIndex].state == INVALID) {
                            if (node.cache[cacheIndex].address != 0xFF && node.cache[cacheIndex].state != INVALID) {
                                handleCacheReplacement(threadId, node.cache[cacheIndex]);
                            }
                            node.cache[cacheIndex].address = msg.address;
                            node.cache[cacheIndex].value = msg.value; // Use the value from the message
                        }
                        node.cache[cacheIndex].state = MODIFIED;
                        // Find the correct instruction value
                        for (int i = 0; i <= instructionIdx; i++) {
                            if (node.instructions[i].type == 'W' && 
                                node.instructions[i].address == msg.address) {
                                node.cache[cacheIndex].value = node.instructions[i].value;
                                break;
                            }
                        }
                        waitingForReply = 0;
                        break;
                    }
                
                    case INV: {
                        byte memBlockAddr = msg.address & 0x0F;
                        byte cacheIndex = memBlockAddr % CACHE_SIZE;
                        
                        if (node.cache[cacheIndex].address == msg.address) {
                            node.cache[cacheIndex].state = INVALID;
                        }
                        break;
                    }
                
                    case WRITE_REQUEST: {
                        byte homeProc = msg.address >> 4;
                        byte memBlockAddr = msg.address & 0x0F;
                        
                        if (threadId == homeProc) {
                            if (node.directory[memBlockAddr].state == U) {
                                // Grant write permission and update memory
                                node.memory[memBlockAddr] = msg.value;  // Make sure this line is executed
                                message reply_wr = {REPLY_WR, threadId, msg.address, msg.value, 0, -1, EM};
                                sendMessage(msg.sender, reply_wr);
                                node.directory[memBlockAddr].bitVector = 1 << msg.sender;
                                node.directory[memBlockAddr].state = EM;
                            }
                            else if (node.directory[memBlockAddr].state == S) {
                                // Also update memory before changing ownership
                                node.memory[memBlockAddr] = msg.value;
                                message reply_id = {REPLY_ID, threadId, msg.address, 0, 
                                                   node.directory[memBlockAddr].bitVector, -1, EM};
                                sendMessage(msg.sender, reply_id);
                                node.directory[memBlockAddr].bitVector = 1 << msg.sender;
                                node.directory[memBlockAddr].state = EM;
                            } 
                            else if (node.directory[memBlockAddr].state == EM) {
                                int owner = __builtin_ffs(node.directory[memBlockAddr].bitVector) - 1;
                                message wb_inv = {WRITEBACK_INV, threadId, msg.address, 0, 0, msg.sender, EM};
                                sendMessage(owner, wb_inv);
                            }
                        }
                        break;
                    }
                
                    case REPLY_WR: {
                        byte memBlockAddr = msg.address & 0x0F;
                        byte cacheIndex = memBlockAddr % CACHE_SIZE;
                        
                        if (node.cache[cacheIndex].address != 0xFF && node.cache[cacheIndex].state != INVALID) {
                            handleCacheReplacement(threadId, node.cache[cacheIndex]);
                        }
                        node.cache[cacheIndex].address = msg.address;
                        node.cache[cacheIndex].value = msg.value;
                        printf("REPLY_WR received: address 0x%02X, value %d, in node: %d\n", 
                               msg.address, msg.value, threadId);  // Better debug message
                        node.cache[cacheIndex].state = MODIFIED;
                        waitingForReply = 0;
                        break;
                    }
                
                    case WRITEBACK_INV: {
                        byte memBlockAddr = msg.address & 0x0F;
                        byte cacheIndex = memBlockAddr % CACHE_SIZE;
                        
                        if (node.cache[cacheIndex].address == msg.address && node.cache[cacheIndex].state == MODIFIED) {
                            message flush_invack = {FLUSH_INVACK, threadId, msg.address, 
                                                  node.cache[cacheIndex].value, 0, 
                                                  msg.secondReceiver, EM};
                            sendMessage(msg.address >> 4, flush_invack);
                            if ((msg.address >> 4) != msg.secondReceiver) {
                                sendMessage(msg.secondReceiver, flush_invack);
                            }
                            node.cache[cacheIndex].state = INVALID;
                        }
                        break;
                    }
                
                    case FLUSH_INVACK: {
                        byte memBlockAddr = msg.address & 0x0F;
                        byte homeProc = msg.address >> 4;
                        
                        if (threadId == homeProc) {
                            node.memory[memBlockAddr] = msg.value;
                            node.directory[memBlockAddr].bitVector = 1 << msg.secondReceiver;
                            node.directory[memBlockAddr].state = EM;
                        }
                        if (threadId == msg.secondReceiver) {
                            byte cacheIndex = memBlockAddr % CACHE_SIZE;
                            if (node.cache[cacheIndex].address != 0xFF && node.cache[cacheIndex].state != INVALID) {
                                handleCacheReplacement(threadId, node.cache[cacheIndex]);
                            }
                            node.cache[cacheIndex].address = msg.address;
                            node.cache[cacheIndex].value = msg.value;
                            node.cache[cacheIndex].state = MODIFIED;
                            // We need to find the instruction that caused this write
                            for (int i = 0; i <= instructionIdx; i++) {
                                if (node.instructions[i].type == 'W' && 
                                    node.instructions[i].address == msg.address) {
                                    node.cache[cacheIndex].value = node.instructions[i].value;
                                    break;
                                }
                            }
                        }
                        break;
                    }
                
                    case EVICT_SHARED: {
                        byte memBlockAddr = msg.address & 0x0F;
                        
                        if (threadId == (msg.address >> 4)) {  // Only home node processes evictions
                            node.directory[memBlockAddr].bitVector &= ~(1 << msg.sender);
                            if (node.directory[memBlockAddr].bitVector == 0) {
                                node.directory[memBlockAddr].state = U;
                            } 
                            else if (__builtin_popcount(node.directory[memBlockAddr].bitVector) == 1) {
                                node.directory[memBlockAddr].state = EM;
                                int remaining = __builtin_ffs(node.directory[memBlockAddr].bitVector) - 1;
                                message evictMsg = {EVICT_SHARED, threadId, msg.address, 0, 0, -1, EM};
                                sendMessage(remaining, evictMsg);
                            } 
                            else {
                                node.directory[memBlockAddr].state = S;
                            }
                        }
                        break;
                    }
                
                    case EVICT_MODIFIED: {
                        byte memBlockAddr = msg.address & 0x0F;
                        
                        if (threadId == (msg.address >> 4)) {  // Only home node processes evictions
                            node.directory[memBlockAddr].bitVector &= ~(1 << msg.sender);
                            node.directory[memBlockAddr].state = U;
                            node.memory[memBlockAddr] = msg.value;
                        }
                        break;
                    }
                }
            }
            
            // Check if we are waiting for a reply message
            // if yes, then we have to complete the previous instruction before
            // moving on to the next
            if ( waitingForReply > 0 ) {
                continue;
            }

            // Process an instruction
            if ( instructionIdx < node.instructionCount - 1 ) {
                instructionIdx++;
            } else {
                if ( printProcState > 0 ) {
                    printProcessorState( threadId, node );
                    printProcState--;
                }
                // even though there are no more instructions, this processor might
                // still need to react to new transaction messages
                //
                // once all the processors are done printing and appear to have no
                // more network transactions, please terminate the program by sending
                // a SIGINT ( CTRL+C )
                continue;
            }
            instr = node.instructions[ instructionIdx ];

            #ifdef DEBUG_INSTR
            printf( "Processor %d: instr type=%c, address=0x%02X, value=%hhu\n",
                    threadId, instr.type, instr.address, instr.value );
            #endif

            // IMPLEMENT
            // Extract the home node's address and memory block index from
            // instruction address
            byte procNodeAddr = instr.address >> 4;           
            byte memBlockAddr = instr.address & 0x0F;
            byte cacheIndex = memBlockAddr % CACHE_SIZE;

            if (instr.type == 'R') {
                // Check if memory block is present in cache and valid
                if (node.cache[cacheIndex].address == instr.address && 
                    node.cache[cacheIndex].state != INVALID) {
                    // Cache hit - no action needed
                    waitingForReply = 0;
                } else {
                    // Read miss - send READ_REQUEST to home node
                    message read_req = {READ_REQUEST, threadId, instr.address, 0, 0, -1, U};
                    sendMessage(instr.address >> 4, read_req);
                    waitingForReply = 1;
                }
            } else { // 'W' - Write instruction
                if (node.cache[cacheIndex].address == instr.address && 
                    node.cache[cacheIndex].state != INVALID) {
                    // Write hit
                    if (node.cache[cacheIndex].state == MODIFIED || 
                        node.cache[cacheIndex].state == EXCLUSIVE) {
                        // Exclusive ownership - can write directly
                        node.cache[cacheIndex].value = instr.value;
                        node.cache[cacheIndex].state = MODIFIED;
                        waitingForReply = 0;
                    } else if (node.cache[cacheIndex].state == SHARED) {
                        // Shared state - need to invalidate other copies
                        message upgrade = {UPGRADE, threadId, instr.address, 0, 0, -1, S};
                        sendMessage(instr.address >> 4, upgrade);
                        waitingForReply = 1;
                    }
                } else {
                    // Write miss - send WRITE_REQUEST to home node
                    message write_req = {WRITE_REQUEST, threadId, instr.address, instr.value, 0, -1, U};
                    sendMessage(instr.address >> 4, write_req);
                    waitingForReply = 1;
                }
            }

        }
    }
}

void sendMessage( int receiver, message msg ) {
    // IMPLEMENT
    // Ensure thread safety while adding a message to the receiver's buffer
    // Manage buffer indices correctly to maintain a circular queue structure
    omp_set_lock(&msgBufferLocks[receiver]);
    int tail = messageBuffers[receiver].tail;
    messageBuffers[receiver].queue[tail] = msg;
    messageBuffers[receiver].tail = (tail + 1) % MSG_BUFFER_SIZE;
    messageBuffers[receiver].count++;
    omp_unset_lock(&msgBufferLocks[receiver]);
}

void handleCacheReplacement( int sender, cacheLine oldCacheLine ) {
    // IMPLEMENT
    // Notify the home node before a cacheline gets replaced
    // Extract the home node's address and memory block index from cacheline address
    byte procNodeAddr = oldCacheLine.address >> 4; 
    byte memBlockAddr = oldCacheLine.address & 0x0F;
    message msg;
    msg.sender = sender;
    msg.address = oldCacheLine.address;
    switch ( oldCacheLine.state ) {
        case EXCLUSIVE:
        case SHARED:
            // IMPLEMENT
            // If cache line was shared or exclusive, inform home node about the
            // eviction
            msg.type = EVICT_SHARED;
            sendMessage(procNodeAddr,msg);
            break;
        case MODIFIED:
            // IMPLEMENT
            // If cache line was modified, send updated value to home node 
            // so that memory can be updated before eviction
            msg.type = EVICT_MODIFIED;
            msg.value = oldCacheLine.value;
            sendMessage(procNodeAddr,msg);
            break;
        case INVALID:
            // No action required for INVALID state
            break;
    }
}

void initializeProcessor( int threadId, processorNode *node, char *dirName ) {
    // IMPORTANT: DO NOT MODIFY
    for ( int i = 0; i < MEM_SIZE; i++ ) {
        node->memory[ i ] = 20 * threadId + i;  // some initial value to mem block
        node->directory[ i ].bitVector = 0;     // no cache has this block at start
        node->directory[ i ].state = U;         // this block is in Unowned state
    }

    for ( int i = 0; i < CACHE_SIZE; i++ ) {
        node->cache[ i ].address = 0xFF;        // this address is invalid as we can
                                                // have a maximum of 8 nodes in the 
                                                // current implementation
        node->cache[ i ].value = 0;
        node->cache[ i ].state = INVALID;       // all cache lines are invalid
    }

    // read and parse instructions from core_<threadId>.txt
    char filename[ 128 ];
    snprintf(filename, sizeof(filename), "tests/%s/core_%d.txt", dirName, threadId);
    FILE *file = fopen( filename, "r" );
    if ( !file ) {
        fprintf( stderr, "Error: count not open file %s\n", filename );
        exit( EXIT_FAILURE );
    }

    char line[ 20 ];
    node->instructionCount = 0;
    while ( fgets( line, sizeof( line ), file ) &&
            node->instructionCount < MAX_INSTR_NUM ) {
        if ( line[ 0 ] == 'R' && line[ 1 ] == 'D' ) {
            sscanf( line, "RD %hhx",
                    &node->instructions[ node->instructionCount ].address );
            node->instructions[ node->instructionCount ].type = 'R';
            node->instructions[ node->instructionCount ].value = 0;
        } else if ( line[ 0 ] == 'W' && line[ 1 ] == 'R' ) {
            sscanf( line, "WR %hhx %hhu",
                    &node->instructions[ node->instructionCount ].address,
                    &node->instructions[ node->instructionCount ].value );
            node->instructions[ node->instructionCount ].type = 'W';
        }
        node->instructionCount++;
    }

    fclose( file );
    printf( "Processor %d initialized\n", threadId );
}

void printProcessorState(int processorId, processorNode node) {
    // IMPORTANT: DO NOT MODIFY
    static const char *cacheStateStr[] = { "MODIFIED", "EXCLUSIVE", "SHARED",
                                           "INVALID" };
    static const char *dirStateStr[] = { "EM", "S", "U" };

    char filename[32];
    snprintf(filename, sizeof(filename), "core_%d_output.txt", processorId);

    FILE *file = fopen(filename, "w");
    if (!file) {
        printf("Error: Could not open file %s\n", filename);
        return;
    }

    fprintf(file, "=======================================\n");
    fprintf(file, " Processor Node: %d\n", processorId);
    fprintf(file, "=======================================\n\n");

    // Print memory state
    fprintf(file, "-------- Memory State --------\n");
    fprintf(file, "| Index | Address |   Value  |\n");
    fprintf(file, "|----------------------------|\n");
    for (int i = 0; i < MEM_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %5d   |\n", i, (processorId << 4) + i,
                node.memory[i]);
    }
    fprintf(file, "------------------------------\n\n");

    // Print directory state
    fprintf(file, "------------ Directory State ---------------\n");
    fprintf(file, "| Index | Address | State |    BitVector   |\n");
    fprintf(file, "|------------------------------------------|\n");
    for (int i = 0; i < MEM_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %2s   |   0x%08x    |\n",
                i, (processorId << 4) + i, dirStateStr[node.directory[i].state],
                node.directory[i].bitVector);
    }
    fprintf(file, "--------------------------------------------\n\n");
    
    // Print cache state
    fprintf(file, "------------ Cache State ----------------\n");
    fprintf(file, "| Index | Address | Value |    State    |\n");
    fprintf(file, "|---------------------------------------|\n");
    for (int i = 0; i < CACHE_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %3d  |  %8s \t|\n",
               i, node.cache[i].address, node.cache[i].value,
               cacheStateStr[node.cache[i].state]);
    }
    fprintf(file, "----------------------------------------\n\n");

    fclose(file);
}