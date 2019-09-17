#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <ucontext.h>
#include <vector>
#include <map>
#include <iostream>


uint32_t N_GENERATIONS = 100;
uint32_t N_MUTATIONS = 5;
uint32_t N_ALLOWED_GENES = 1;
uint32_t timeout = 2;

struct Instruction {
    std::vector <uint8_t> instr;
    uint8_t size:4; // Maximum size of x86 instructions are 15bytes, 15 = 1111 = 4 bits 
};
struct MetadataJump{
    uint32_t src_line;
    uint32_t dest_line;
    int32_t rel_value;
};
struct Chromossome{
    std::vector<Instruction> chromossome;
    std::vector<MetadataJump> metadata;  
};

std::map <uint8_t, uint8_t> instruction_sizes_map = {
    {0x55, 1},  // push reg
    {0xC3, 1},  // ret
    {0x5D, 1},  // pop reg
    {0x48, 3},  // mov reg ; div reg; mul reg
    {0x49, 3},  // inc regx; cmp regx, reg     
    {0x4D, 3},  // xor regx, regx
    {0x66, 4},  // movi 
    {0xE9, 5},  // jmp im32
    {0x0F, 6},   // jae im32
    {0x50, 1},
    {0x51, 1},
    {0x52, 1},
    {0x53, 1},
    {0x54, 1},
    {0x56, 1},
    {0x57, 1},
    {0x58, 1},
    {0x59, 1},
    {0x5A, 1},
    {0x5B, 1},
    {0x5C, 1},
    {0x5D, 1},
    {0x5E, 1},
    {0x5F, 1},
    {0x41, 2}
};


//Variables used in the logic of the threads used
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_t threadRunner;
pthread_t threadWatcher;
uint8_t isThreadRunnerAlive;

typedef struct {
    uint8_t *ptr;
    uint32_t n;
} thread_arg_t, *ptr_thread_arg_t;

//
void printInstructionVector(const std::vector<Instruction> &vec);
void addSourceCodeToVector(uint8_t* sourcecode, std::vector<Instruction> &to_vector, uint32_t size);
void addSourceCodeToArray(uint8_t* sourcecode, FILE *file);
void printInstructionVector(const std::vector<Instruction> &vec);
void remapJumpLocations(uint32_t newline, uint8_t nbytes, std::vector<Instruction> &vec, std::vector<MetadataJump> &jumps_metadata);
void copyVectorToArray(uint8_t *code2memory, std::vector<Instruction> &chromossome);
void executeInMemory(std::vector<Instruction> &chromossome);

void mutate(Chromossome &current);
void selectRandomGene(Instruction &aux, uint32_t random_line);

void *pthreadWaitOrKill(void* args);
void *pthreadExecuteInMemory(void* _args);
void *pthreadWaitWithTimeoutAndCond(void* args);

static void sigaction_sigfpe(int signal, siginfo_t *si, void *arg);
void setSignalHanlder(int32_t signo);

uint8_t getSizeOfInstruction(uint8_t opcode);
uint32_t getChromossomeSize(Chromossome &chromossome);
uint32_t mapJumpLocationsAux(const std::vector<Instruction> vec, uint32_t line, int32_t value);
inline uint32_t generateRandomNumber(uint32_t min, uint32_t max);
//

// Gets the size of a instruction. This is hardcoded and must be changed to work with other inputs
uint8_t getSizeOfInstruction(uint8_t opcode){
    return instruction_sizes_map[opcode];
}

// Adds the x86 hex code to a Vector of Instruction
void addSourceCodeToVector(uint8_t* sourcecode, std::vector<Instruction> &to_vector, uint32_t size){

    uint32_t i = 0, j = 0;
    while ( i < size){
        j = getSizeOfInstruction(sourcecode[i]);
        Instruction aux;

        aux.size = j;
        for(uint32_t k = 0; k < j; k++){
            aux.instr.push_back(sourcecode[i + k]);
        }

        printf("sourcecode %.2X ---> i %d\n", sourcecode[i], i);
        to_vector.push_back(aux);

        i += j;
    }
}

// Prints in groups of 2 bytes all the instructions contained in &vec
void printInstructionVector(const std::vector<Instruction> &vec){

    for(std::size_t i = 0; i < vec.size() ; ++i){
        // printf("%d: ", i);
        for (uint32_t j = 0; j < vec[i].instr.size(); j++){
            printf("%.2X ", vec[i].instr[j]);
        }
        printf("\n");
    } 
}

// Just an auxiliary function to be used in mapJumpLocations()
uint32_t mapJumpLocationsAux(const std::vector<Instruction> vec, uint32_t line, int32_t value){
    uint32_t sum = 0, currentline = line; 
    for(;;){
        if(sum > abs(value)){
            return currentline - 1;
        }
        else if(sum == abs(value)){
            return currentline + 1;

        }else{

            if(value > 0){ // verify whether the relative jump is forwards or backwards

                currentline++;
                sum += vec[currentline].size;
            }else{

                sum += vec[currentline].size;
                currentline--;
            }
        }
    }
}

// Executed only once. Maps the location of all jump instructions and their relative destination into a struct of type MetadataJump
void mapJumpLocations(const std::vector<Instruction> &vec, std::vector<MetadataJump> &jumps_metadata){

    for (uint32_t line = 0; line < vec.size(); line++ ){

        std::vector<uint8_t> instr = vec[line].instr; 
        uint8_t opcode = instr[0];
        uint32_t size = instr.size();
        int32_t value = 0x0;
        MetadataJump meta_aux;


        if (opcode == 0xE9){ // jmp
            for(uint32_t idx = 1; idx < size; idx++){
                value = value | ( instr[idx] << ((idx-1)*8));
            }
            meta_aux.src_line = line;
            meta_aux.dest_line = mapJumpLocationsAux(vec, line, value);
            meta_aux.rel_value = value;
            jumps_metadata.push_back(meta_aux);


        }else if (opcode == 0x0F) { // jae
            for(uint32_t idx = 2; idx < size; idx++){
                value = value | ( instr[idx] << ((idx-2)*8));
            }
            meta_aux.src_line = line;
            meta_aux.dest_line = mapJumpLocationsAux(vec, line, value);
            meta_aux.rel_value = value;
            jumps_metadata.push_back(meta_aux);
        }
    }
}

// Recalculates the location of all jump instructions and their relative destination when a new instruction, in the position of newline, is added 
void remapJumpLocations(uint32_t newline, uint8_t nbytes, std::vector<Instruction> &vec, std::vector<MetadataJump> &jumps_metadata){

    for(uint32_t i = 0; i < jumps_metadata.size(); i++){

        uint32_t src_line = jumps_metadata[i].src_line;
        uint32_t dest_line = jumps_metadata[i].dest_line;
        int32_t rel_value = jumps_metadata[i].rel_value;

        if (rel_value > 0){ // dest_line > src_line (a jump forwards)
            if(newline > src_line && newline <= dest_line){ // we need to alter dest_line and value

                //FIXME: search for correct information about instruction size (it is now hardcoded)
                uint8_t instr_size = instruction_sizes_map[0x0f];

                rel_value += nbytes;

                for(uint8_t idx = 0; idx < 4; idx++){
                    vec[src_line].instr[instr_size - idx - 1] = (uint8_t)((0xFF000000 >> 8*idx) & rel_value)>>(8*(3-idx));
                }

                jumps_metadata[i].dest_line++;
                jumps_metadata[i].rel_value = rel_value;

            }else if(newline <= src_line){ // if the instruction is before src_line than shift both source and destination by 1line
                jumps_metadata[i].src_line++;
                jumps_metadata[i].dest_line++;

            }
        }else{  // this means that src_line > dest_line (a jump backwards)
            if(newline <= src_line && newline > dest_line){

                //FIXME: search for correct information about instruction size (it is now hardcoded)
                uint8_t instr_size = instruction_sizes_map[0xe9];

                rel_value -= nbytes;

                for(uint8_t idx = 0; idx < 4; idx++){
                    vec[src_line].instr[instr_size - idx - 1] = ((0xFF000000 >> 8*idx) & rel_value)>>(8*(3-idx));
                }

                jumps_metadata[i].src_line++;
                jumps_metadata[i].rel_value = rel_value;

            }else if(newline <= dest_line){ // if the instruction is before dest_line than shift both source and destination by 1line
                jumps_metadata[i].src_line++;
                jumps_metadata[i].dest_line++;
            }
        }
    }
}

//Returns a random number between min and max ie. [min, max] 
inline uint32_t generateRandomNumber(uint32_t min, uint32_t max){ 
    // return rand() % (max)  + min;
    return (rand() % (max + 1 - min)) + min;
}

// Copies a vector of type Instruction into an array of type uint8_t
void copyVectorToArray(uint8_t *code2memory, std::vector<Instruction> &chromossome){

    uint32_t idx = 0; 
    for ( auto &elem : chromossome ) {

        uint8_t* pos = elem.instr.data();
        for(uint32_t i = 0; i < elem.instr.size(); i++){
            code2memory[idx++] = *pos++;
        }
    }
}

//Executes in memory a vector containing bytes correspondent to x86 instructions    
void executeInMemory(std::vector<Instruction> &chromossome){

    uint32_t chrom_size = 0;
    for(uint32_t i = 0; i < chromossome.size(); i++){
        chrom_size += chromossome[i].size;
    }

    uint8_t *code2memory = (uint8_t*) malloc(sizeof(uint8_t)*chrom_size); 

    copyVectorToArray(code2memory, chromossome);    

    uint32_t length = sysconf ( _SC_PAGE_SIZE ) ;
    void * memory = mmap (NULL , length , PROT_NONE , MAP_PRIVATE | MAP_ANONYMOUS , -1 , 0);
    mprotect ( memory , length , PROT_WRITE );
    memcpy ( memory , ( void *) ( code2memory ) , sizeof(uint8_t)*chrom_size );
    mprotect ( memory , length , PROT_EXEC );

    const uint64_t (* jit ) ( const uint64_t , const uint64_t , const uint64_t ) = 
        ( const uint64_t (*) ( const uint64_t , const uint64_t , const uint64_t ) ) ( memory ) ;

    printf ( "2^12 mod 10 = %lu \n" , (* jit ) (2 , 12 ,10 ) ) ;
    
    munmap ( memory , length ) ;
}

// Sets the thread cancel mode to assynchronous and Executes in memory a vector passed by reference containing bytes correspondent to x86 instructions  
void* pthreadExecuteInMemory(void* _args){

    isThreadRunnerAlive = 1;
    
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    setSignalHanlder(SIGFPE);
    usleep(100);

    ptr_thread_arg_t ptr_args = (ptr_thread_arg_t) _args;

    uint32_t length = sysconf ( _SC_PAGE_SIZE ) ;
    void * memory = mmap (NULL , length , PROT_NONE , MAP_PRIVATE | MAP_ANONYMOUS , -1 , 0);
    mprotect ( memory , length , PROT_WRITE );
    memcpy ( memory , ( void *) ( ptr_args->ptr ) , sizeof(uint8_t)*ptr_args->n );
    mprotect ( memory , length , PROT_EXEC );


    const uint64_t (* jit ) ( const uint32_t , const uint32_t , const uint64_t ) = 
        ( const uint64_t (*) ( const uint32_t , const uint32_t , const uint64_t ) ) ( memory ) ;

    uint64_t retval = 0;
    retval = (*jit)(2, 12, 10);

    munmap ( memory , length ) ;
    // pthread_cond_signal(&cond);
    isThreadRunnerAlive = 0;
    pthread_exit ( (void *) retval );
}

// Kills a thread if a given time has elapsed (global timeout variable)
void *pthreadWaitOrKill(void* args){

    // clock_t start, end;
    // double cpu_time_used;
    volatile time_t begin = NULL, end = NULL;
    
    begin = time(NULL);
    
    //     start = clock();
    
    while (true){
        // pthread_mutex_trylock(&mutex);
        // end = clock();
        usleep(100);
        end = time(NULL);

        // cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;
        if(isThreadRunnerAlive == 0 ){
            break;
        }else if(end - begin >= timeout){
            pthread_cancel( threadRunner );
            break;
        }
        // pthread_mutex_unlock(&mutex);
        sched_yield();
    }
    pthread_exit(NULL);
}

// Waits for an elapsed time (global timeout variable) or a signal in a pthread_condition variable 
void *pthreadWaitWithTimeoutAndCond(void* args){

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout;
    // pthread_cond_init(&cond, NULL);
    pthread_mutex_lock(&mutex);
    uint32_t n = pthread_cond_timedwait(&cond, &mutex, &ts);
    pthread_mutex_unlock(&mutex);

    if (n == 0 || n == ETIMEDOUT){
        pthread_cancel( *(pthread_t*) args );
    }
    // pthread_cond_destroy(&cond);
    pthread_exit(NULL);
}

// Inserts in *aux a random instruction (gene). It is defined here all the instructions that can be inserted
void selectRandomGene(Instruction &aux, uint32_t random_line){

    // rax (0) is avoided because we know that is the most used register
    // rsp (4) and rbp (5) are also avoided because they can mess with the stack
    uint8_t reg_y, reg_x;
    do{
        reg_x = generateRandomNumber(1, 15);
    }while(reg_x == 4 || reg_x == 5);

    do{
        reg_y = generateRandomNumber(1, 15);
    }while(reg_y == 4 || reg_y == 5);
    
    uint32_t randomValue = generateRandomNumber(0, RAND_MAX); // a random value to add when a IM32 is needed

    uint32_t randomInstruction = generateRandomNumber(0, 15);   // 15 is the number of decoded instructions 
    uint8_t ext;
    uint8_t regs;
    uint8_t byte;

    switch (randomInstruction){
        case 0: //INC reg
            //0100100X 11111111 11000XXX
            ext  = (0b01001000) | ((reg_x & 0x8) >> 3);
            regs = (0b11000000) | ((reg_x & 0x7));
            aux.instr = {ext , 0xFF , regs };
        break;
        case 1: //DEC reg
            //0100100X 11111111 11001XXX
            ext  = (0b01001000) | ((reg_x & 0x8) >> 3);
            regs = (0b11001000) | ((reg_x & 0x7));
            aux.instr = {ext , 0xFF , regs };
        break;

        case 2: //CMP reg, reg
            // 01001X0Y 00111001 11XXXYYY
            ext = (0b01001000) | ((reg_x & 0x8) >> 1) | ((reg_y & 0x8) >> 3 );
            regs = (0b11000000) | ((reg_x & 0x7) << 3) | (reg_y & 0x7);
            aux.instr = {ext , 0x39 , regs};
        break;
        
        case 3: // XOR reg, reg
            // 01001X0Y 00110001 11XXXYYY
            ext  = (0b01001000) | ((reg_x & 0x8) >> 1) | ((reg_y & 0x8) >> 3 );
            regs = (0b11000000) | ((reg_x & 0x7) << 3) | (reg_y & 0x7);
            aux.instr = {ext , 0x31 , regs};
        break;
        case 4: // XOR reg, im32
            if(reg_x == 1){
                aux.instr = {0x48 , 0x35};
            } else{
                ext  = (0b01001000) | ((reg_x & 0x8) >> 3);
                regs = (0b11110000) | ((reg_x & 0x7));
                aux.instr = {ext , 0x81 , regs};
            }
            
            for(uint8_t idx = 0; idx < 4; idx++){
                byte = ((0xFF000000 >> 8*idx) & randomValue)>>(8*(3-idx));
                aux.instr.push_back(byte);
            }
        break;

        case 5: //ADD reg, reg
            ext  = (0b01001000) | ((reg_x & 0x8) >> 1) | ((reg_y & 0x8) >> 3 );
            regs = (0b11000000) | ((reg_x & 0x7) << 3) | (reg_y & 0x7);
            aux.instr = {ext , 0x01 , regs};
        break;
        case 6: //ADD reg, im32
            if(reg_x == 1){
                aux.instr = {0x48 , 0x05};
            } else{
                ext  = (0b01001000) | ((reg_x & 0x8) >> 3);
                regs = (0b11000000) | ((reg_x & 0x7));
                aux.instr = {ext , 0x81 , regs};
            }
            
            for(uint8_t idx = 0; idx < 4; idx++){
                byte = ((0xFF000000 >> 8*idx) & randomValue)>>(8*(3-idx));
                aux.instr.push_back(byte);
            }
        break;

        case 7: //BSWAP reg - Byte Swap
            ext  = (0b01001000) | ((reg_x & 0x8) >> 3);
            regs = (0b11001000) | ((reg_x & 0x7));
            aux.instr = {ext , 0x0F , regs };
        break;

        case 8: //NOT reg - One's Complement Negation
            ext  = (0b01001000) | ((reg_x & 0x8) >> 3);
            regs = (0b11010000) | ((reg_x & 0x7));
            aux.instr = {ext , 0xF7 , regs };
        break;

        case 9: //NEG reg - Two's Complement Negation
            ext  = (0b01001000) | ((reg_x & 0x8) >> 3);
            regs = (0b11011000) | ((reg_x & 0x7));
            aux.instr = {ext , 0xF7 , regs };
        break;

        case 10: //NEG reg - Two's Complement Negation
            ext  = (0b01001000) | ((reg_x & 0x8) >> 3);
            regs = (0b11001000) | ((reg_x & 0x7));
            aux.instr = {ext , 0xFF , regs };
        break;

        case 11: //AND
            ext  = (0b01001000) | ((reg_x & 0x8) >> 1) | ((reg_y & 0x8) >> 3 );
            regs = (0b11000000) | ((reg_x & 0x7) << 3) | (reg_y & 0x7);
            aux.instr = {ext , 0x21 , regs};
        break;
        case 12: // AND reg, im32
            if(reg_x == 1){
                aux.instr = {0x48 , 0x0D};
            } else{
                ext  = (0b01001000) | ((reg_x & 0x8) >> 3);
                regs = (0b11001000) | ((reg_x & 0x7));
                aux.instr = {ext , 0x81 , regs};
            }
            
            for(uint8_t idx = 0; idx < 4; idx++){
                byte = ((0xFF000000 >> 8*idx) & randomValue)>>(8*(3-idx));
                aux.instr.push_back(byte);
            }
        break;

        case 13: //OR reg, reg
            ext  = (0b01001000) | ((reg_x & 0x8) >> 1) | ((reg_y & 0x8) >> 3 );
            regs = (0b11000000) | ((reg_x & 0x7) << 3) | (reg_y & 0x7);
            aux.instr = {ext , 0x09 , regs};
        break;
        case 14: // OR reg, im32

            if(reg_x == 1){
                aux.instr = {0x48 , 0x25};
            } else{
                ext  = (0b01001000) | ((reg_x & 0x8) >> 3);
                regs = (0b11100000) | ((reg_x & 0x7));
                aux.instr = {ext , 0x81 , regs};
            }
            
            for(uint8_t idx = 0; idx < 4; idx++){
                byte = ((0xFF000000 >> 8*idx) & randomValue)>>(8*(3-idx));
                aux.instr.push_back(byte);
            }

        break;

        case 15: // CLC — Clear Carry Flag
            aux.instr = {0xF8};
        break;
    }

    aux.size = aux.instr.size();
}

// Mutate the chromossome adding a new gene
void mutate(Chromossome &current){

        //FIXME: 11 is a hardcoded number representing the offset where the new instructions should be added
        uint32_t random_line = generateRandomNumber(11, current.chromossome.size()-11);
        Instruction newGene;

        selectRandomGene(newGene, random_line);
        remapJumpLocations(random_line, newGene.size, current.chromossome, current.metadata);
        current.chromossome.insert(current.chromossome.begin() + random_line, newGene);
}

// Returns how many bytes a chromossome has
uint32_t getChromossomeSize(Chromossome &chromossome){
    uint32_t size = 0;
    for(uint32_t k = 0; k < chromossome.chromossome.size(); k++){
        size += chromossome.chromossome[k].size;
    }
    return size;
}

// Sets the action that the signal handler is going to make when it catches a signal
static void sigaction_sigfpe(int signal, siginfo_t *si, void *arg){
    ucontext_t *ctx = (ucontext_t *)arg;

    printf("Caught SIGFPE, address %p, RIP 0x%llx\n", si->si_addr, ctx->uc_mcontext.gregs[REG_RIP]);
    pthread_cancel(pthread_self());
    isThreadRunnerAlive = 0;
    ctx->uc_mcontext.gregs[REG_RIP] += 6; // skipping the "div reg" instruction

}

// Catches the SIGFPE signal and jumps 6 bytes (the action is in the fucntion referenced by sa_sigaction) 
void setSignalHanlder(int32_t signo){
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = sigaction_sigfpe;
    sa.sa_flags = SA_SIGINFO;
    sigaction(signo, &sa, NULL);
}

// NOT USED. This functions was supposed to add in execution time the hex code for the "pusha/popa" 
void addSourceCodeToArray(uint8_t *sourcecode, FILE *file){
    
    uint32_t bytes;
    uint32_t i = 0;

    // Push/Pop non-preseverd registers (without rax). 
    uint8_t prologue[] = {0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41 ,0x51 ,0x41 ,0x52 ,0x41, 0x53 };
    uint8_t epilogue[] = {0x59 ,0x5A ,0x5E ,0x5F ,0x41 ,0x58 ,0x41 ,0x59 ,0x41 , 0x5A ,0x41 ,0x5B };

    while ((fscanf(file, "%2x", &bytes)) != EOF) {
        
        if((uint8_t)bytes == 0x55){
            for (auto &elem : prologue){
                sourcecode[i++] = elem;
            }
        }

        if((uint8_t)bytes == 0xC3){
            for(auto &elem : epilogue){
                sourcecode[i++] = elem;
            }
        }
        sourcecode[i++] = (uint8_t) bytes;
    }
}

int main(int argc, char *argv[]){

    if(argc != 4){
        printf("Correct usage: ./main.bin N_GENERATIONS N_MUTATIONS N_ALLOWED_GENES\n");
        exit(EINVAL);
    }else{
        N_GENERATIONS = atoi(argv[1]);
        N_MUTATIONS = atoi(argv[2]);
        N_ALLOWED_GENES = atoi(argv[3]);
    }

    FILE *file;

    std::vector <Instruction> origin_vector;
    std::vector <Chromossome> population_list;
    
    uint32_t bytes;
    void* retval = 0;
    uint32_t n = 0;
    Chromossome aux;
    population_list.push_back(aux); // just so it initializes 

    file = fopen("code.hex", "r");
    if (file == NULL){ printf("Erro: nao foi possivel abrir o arquivo\n"); return 0; }
    else while ((fscanf(file, "%2x", &bytes)) != EOF) n++;
    rewind(file);
    
    uint8_t* origin_code = (uint8_t*) malloc(n*sizeof( uint8_t ));  
    uint32_t l = 0;
    while ((fscanf(file, "%2x", &bytes)) != EOF) origin_code[l++] = (uint8_t) bytes;
    
    fclose(file);

    addSourceCodeToVector(origin_code, population_list[0].chromossome, n);
    printf("Cromossomo inicial: \n");
    printInstructionVector(population_list[0].chromossome);
    mapJumpLocations(population_list[0].chromossome, population_list[0].metadata );

    uint32_t tamanho_original = population_list[0].chromossome.size();

    srand((uint32_t) time(0));

    // repeat for N_GENERATIONS
    for(uint32_t gen = 0; gen < N_GENERATIONS; gen++){

        std::vector<Chromossome> apt_list; 

        uint32_t popSize = population_list.size();

        // for each chromossome
        for (uint32_t i = 0; i < popSize; i++){

            // mutate N_MUTATIONS times
            for (uint32_t j = 0; j < N_MUTATIONS; j++){
                    Chromossome currentChromossome = population_list[i];

                    mutate(currentChromossome);

                    //thread related
                    uint32_t chrom_size = getChromossomeSize(currentChromossome);
                    uint8_t *code2memory = (uint8_t*) malloc(sizeof(uint8_t)*chrom_size); 
                    copyVectorToArray(code2memory, currentChromossome.chromossome);
                    thread_arg_t thread_args = {
                        .ptr = code2memory,
                        .n = chrom_size
                    };

                    isThreadRunnerAlive = 1;
                    pthread_create( &threadRunner, NULL, pthreadExecuteInMemory, &thread_args);
                    pthread_create( &threadWatcher, NULL, pthreadWaitOrKill, NULL);
                    pthread_join(threadRunner, &retval);
                    pthread_join(threadWatcher, NULL);
                    //thread related

                    // compares with the expected result
                    if((uint64_t) retval == 6){
                        apt_list.push_back(currentChromossome);
                        printf("Generation(%u) Chromossome(%u) Mutation(%u)\n",gen, i, j);
                        if(apt_list.size() >= N_ALLOWED_GENES){
                            free(code2memory);
                            break;
                        }
                    }
                    free(code2memory);

            }
        }

        // if there is any apt mutated chromossome, make them the new population since they have more instructions
        if(apt_list.size() > 0){
            population_list = apt_list;
            // printInstructionVector(population_list[0].chromossome);
        }
    }
    printf("\n\n////////////////////////////////////////////////////////////////\n");
    printf("Output code:\n");
    printInstructionVector(population_list[0].chromossome);
    printf("How many instructions were inserted: %lu \n", population_list[0].chromossome.size()-tamanho_original);
    printf("Execution test: ");
    executeInMemory(population_list[0].chromossome);
    // free(origin_code);

    return 0;
}

// The original code is as follows:
// rdi = b , rsi = n , rdx = p
// uint8_t origin_code [] = {
//     0x55 ,                                          // 0 push rbp
//     0x48 , 0x89 , 0xE5 ,                            // 1 mov rbp, rsp
//     0x48 , 0x89 , 0xD1 ,                            // 2 mov rcx, rdx (p)
//     0x66 , 0xB8 , 0x01 , 0x00,                      // 3 mov(movi) ax, 1 (a=1)
//     0x4D , 0x31 , 0xC0 ,                            // 4 xor r8, r8 (i=0)
//     0x49 , 0x39 , 0xF0 ,                            // 5 cmp r8, rsi (i?n)
//     0x0F , 0x83 , 0x11 , 0x00 , 0x00 , 0x00 ,       // 6 jae 0x11 (i>=n end)
//     0x48 , 0xF7 , 0xE7 ,                            // 7 mul rdi (a = a * b)
//     0x48 , 0xF7 , 0xF1 ,                            // 8 div rcx (rdx = a % p)
//     0x48 , 0x89 , 0xD0 ,                            // 9 mov rax, rdx (rax = rdx)
//     0x49 , 0xFF , 0xC0 ,                            // 10 inc r8 (i ++)
//     0xE9 , 0xE6 , 0xFF , 0xFF , 0xFF ,              // 11 jmp  -0x1A (loop again) // 0x1110 0110 -> 0001 1010
//     0x5D ,                                          // 12 pop rbp
//     0xC3 ,                                          // 13 ret
// };