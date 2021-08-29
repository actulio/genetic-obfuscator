# Genetic Obfuscator

This project aims to obfuscate a given binary using the evolutionary approach present in Genetics Algorithms. This project is a proof of concept and was designed in a way to ease the development process rather than being focused on performance. The input assembly code used for testing was one of Modular Exponentiation.

To compile it, use:

```
g++ main.cpp -O3 -pthread -o main.bin
```

GCC has a bug that for the code related to execution in memory to work properly, you need to add a optimization directive such as -O3. This is not an issue when compiling with Clang. Also, the -pthread flag is required to link the multithreading library. You may also want to add -Wall and -g flags to improve debugging experience.

To run it, use:

```
./main.bin N_GENERATIONS N_MUTATIONS N_ALLOWED_GENES
```

Where _N_x_ are integer numbers.

---

The flow of the code is as follows:

- The hex code representing the x86 assembly instructions is read
- The code is saved in an suitable c++ structure (In our case, a Vector of structs), along with a structure containing metadata about jump instructions and their related jump destinations
- For N_GENERATIONS generations

  - For each chromosome
    - For N_MUTATIONS times OR a number of allowed apt genes happen
      - Add one random instruction (Mutate)
      - Remap the metadata about jump locations
      - Launch a thread to run the code (current mutated chromosome) in memory
      - Launch a thread to verify if the previous thread is in loop (waits for a given time or a flag)
      - Compare the expected result and add to a temporary Vector if the chromosome is apt ( the result is equal).
  - Substitute the parents with the children that are apt, if there is any

- Print one successful chromosome

---

Following there is a list with some problems and their solutions, so it may help in the understanding of the code:

1.  Instruction sizes:
    A map was created with hardcoded sizes of the instructions existing in the input code. This is far from ideal, but we figured that a "x86 assembler" would be a project itself. If you wish to do it the correct way, you would only need to replace the **getSizeOfInstruction()** function.
    Nevertheless, a tool that can do just that can be found at [Defuse](https://defuse.ca/) online-x64-assembler.

2.  Instructions to add:
    Using Defuse online assembler already mentioned to decipher them and an awesome [online instruction reference](https://www.felixcloutier.com/x86/index.html), the following instructions were added to the gene pool:

        |          Instructions         |
        |---------------|---------------|
        | inc reg       | bswap reg     |
        | dec reg       | not reg       |
        | cmp reg, reg  | neg reg       |
        | xor reg, reg  | and reg, reg  |
        | xor reg, im32 | and reg, im32 |
        | add reg, reg  | or reg, reg   |
        | add reg, im32 | or reg, im32  |
        |               | clc           |

3.  Program loop:
    A watcher thread was required because a program does not know whether it is in loop or just taking a long time. So a _timeout_ value based in some heuristic was placed.

4.  Thread Cancellation:
    Firstly, we approached the problem using **pthread_cond_timedwait()**, for a thread would wait a condition or a timeout. But this approach proved too slow (probably due to thread synchronization) so we shifted to a verifying loop with clock()/time(). Another problem was that if the thread was in a tight loop, with no cancellation points, it could not answer a pthread_cancel() command since the default cancellation mode is of type "deferred". So we changed to asynchronous cancel, meaning that it could be cancelled at any time. This brought yet another problem, as it can (and did) leave some structures in a inconsistent state. The solution was to allocate most of what was needed outside and pass only the references to the thread.

5.  SIGFPE
    Because there is a **div** instruction in the input code, a signal handler "for a zero division" was created so the main program wouldn't stop.

---

Known issues:
For larger N_GENERATIONS numbers, some SIGSEGV (segmentation fault) happens in random parts of the code, such as in the library function time()/clock() and in some cases in the "push rbp" code executed in memory. At first, we push'ed and pop'ed all registers (except rax) as it could've been used for something before and consequently after we executed the code in memory. Although this fix "delayed" the SIGSEGVs, for we could run sometimes for even 500 generations, the problem still ocurred. We could not solve nor understand what was causing those errors.

This was a course project (COMP0418 - INTERFACE HARDWARE/SOFTWARE) at Universidade Federal de Sergipe. The idea came from a friend and work duo [Bruno Rodrigues](https://github.com/BrunoRodriguesDev). If this project was useful for you in any way, you should consider giving it a star.
