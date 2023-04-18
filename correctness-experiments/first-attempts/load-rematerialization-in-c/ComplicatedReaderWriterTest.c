#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define UNUSED(x) (void)(x)

//-------------------------------------
//  MersenneTwister
//  A thread-safe random number generator with good randomness
//  in a small number of instructions. We'll use it to introduce
//  random timing delays.
//-------------------------------------
#define MT_IA 397
#define MT_LEN 624

typedef struct MersenneTwister {
    unsigned int m_buffer[MT_LEN];
    int m_index;
} MyRandom;

unsigned int nextInt(MyRandom *myRandom) {
    // Indices
    int i = myRandom->m_index;
    int i2 = myRandom->m_index + 1;
    if (i2 >= MT_LEN) i2 = 0;  // wrap-around
    int j = myRandom->m_index + MT_IA;
    if (j >= MT_LEN) j -= MT_LEN;  // wrap-around

    // Twist
    unsigned int s = (myRandom->m_buffer[i] & 0x80000000) |
                     (myRandom->m_buffer[i2] & 0x7fffffff);
    unsigned int r = myRandom->m_buffer[j] ^ (s >> 1) ^ ((s & 1) * 0x9908B0DF);
    myRandom->m_buffer[myRandom->m_index] = r;
    myRandom->m_index = i2;

    // Swizzle
    r ^= (r >> 11);
    r ^= (r << 7) & 0x9d2c5680UL;
    r ^= (r << 15) & 0xefc60000UL;
    r ^= (r >> 18);
    return r;
}

MyRandom initMyRandom(unsigned int seed) {
    MyRandom newRandom;
    // Initialize by filling with the seed, then iterating
    // the algorithm a bunch of times to shuffle things up.
    for (int i = 0; i < MT_LEN; i++) newRandom.m_buffer[i] = seed;
    newRandom.m_index = 0;
    for (int i = 0; i < MT_LEN * 100; ++i) {
        nextInt(&newRandom);
    }
    return newRandom;
}

//-------------------------------------

#define TEST_ITERS 1000
#define MAIN_RANDOM_START_SEED 65
#define READ_RANDOM_START_SEED 6565
#define WRITE_RANDOM_START_SEED 651

#define VALS_SIZE 100000
#define READER_ITERS 50000
#define WRITER_ITERS 50000
#define CHECK_EACH_ITER 50
#define CHECK_FIRST_VALS VALS_SIZE
#define READER_SLEEPS_EACH_ITER_MICROS 1
#define WRITER_SLEEPS_EACH_ITER_MICROS 3

#define READER_CPU 2
#define WRITER_CPU 3
#define PREPARATION_WAIT_MILLIS 10

int vals[VALS_SIZE];
int lastValsScan[VALS_SIZE];
int changedTimes[VALS_SIZE];
bool wasRead[VALS_SIZE];
bool wasWritten[VALS_SIZE];
int savedRead = 0;

// start semaphores are used to wait for setaffinity to suceed
sem_t startReaderSema;
sem_t startWriterSema;
// end semaphore lets be sure both threads have finished and semaphores can be
// freed
sem_t endSema;

inline unsigned int getRandomIndex(MyRandom *myRandom) {
    return nextInt(myRandom) % VALS_SIZE;
}

void clearState(int iter) {
    MyRandom mainRandom = initMyRandom(MAIN_RANDOM_START_SEED + iter);
    for (int i = 0; i < VALS_SIZE; ++i) {
        vals[i] = nextInt(&mainRandom);
    }
}

void checkVals() {
    for (int i = 0; i < CHECK_FIRST_VALS; ++i) {
        int v = vals[i];
        if (v != lastValsScan[i]) {
            // fprintf(stderr, "Index changed: %d\n", i);
            ++changedTimes[i];
            if (changedTimes[i] > 1) {
                fprintf(stderr, "\nUB was caught!\n");
                exit(0);
            }
            lastValsScan[i] = v;
        }
    }
}

void *doReads(void *args) {
    // prepare and wait
    int iter = *((int *)args);
    for (int i = 0; i < VALS_SIZE; ++i) {
        wasRead[i] = false;
        lastValsScan[i] = vals[i];
        changedTimes[i] = 0;
    }
    savedRead = 0;
    MyRandom readRandom = initMyRandom(READ_RANDOM_START_SEED + iter);
    sem_wait(&startReaderSema);

    // act
    for (int i = 0; i < READER_ITERS; ++i) {
        unsigned int readIndex = getRandomIndex(&readRandom);
        while (wasRead[readIndex]) {
            readIndex = getRandomIndex(&readRandom);
        }
        wasRead[readIndex] = true;
        int read = vals[readIndex];
        savedRead = (read + read - 1) + (savedRead - 1 + read) * read;
        if (i % CHECK_EACH_ITER == 0) {
            checkVals();
        }
        usleep(READER_SLEEPS_EACH_ITER_MICROS);
    }
    fprintf(stderr, "Reader has finished\n");
    sem_post(&endSema);
    return NULL;
}

void *doWrites(void *args) {
    // prepare and wait
    int iter = *((int *)args);
    for (int i = 0; i < VALS_SIZE; ++i) {
        wasWritten[i] = false;
    }
    MyRandom writeRandom = initMyRandom(WRITE_RANDOM_START_SEED + iter);
    sem_wait(&startWriterSema);

    // act
    for (int i = 0; i < WRITER_ITERS; ++i) {
        unsigned int writeIndex = getRandomIndex(&writeRandom);
        while (wasWritten[writeIndex]) {
            writeIndex = getRandomIndex(&writeRandom);
        }
        wasWritten[writeIndex] = true;
        vals[writeIndex] = nextInt(&writeRandom);
        usleep(WRITER_SLEEPS_EACH_ITER_MICROS);
    }
    fprintf(stderr, "Writer has finished\n");
    sem_post(&endSema);
    return NULL;
}

// TODO: handle possible errors
void test(int iter) {
    // create and start preparing threads
    sem_init(&startReaderSema, 0, 0);
    sem_init(&startWriterSema, 0, 0);
    sem_init(&endSema, 0, 0);

    pthread_t reader;
    pthread_t writer;
    pthread_create(&reader, NULL, doReads, (void *)&iter);
    pthread_create(&writer, NULL, doWrites, (void *)&iter);

    // force threads to be placed on different cpu-s
    cpu_set_t readerCpus;
    CPU_ZERO(&readerCpus);
    CPU_SET(READER_CPU, &readerCpus);
    pthread_setaffinity_np(reader, sizeof(cpu_set_t), &readerCpus);

    cpu_set_t writerCpus;
    CPU_ZERO(&writerCpus);
    CPU_SET(WRITER_CPU, &writerCpus);
    pthread_setaffinity_np(writer, sizeof(cpu_set_t), &writerCpus);

    // wait a bit to be sure threads are prepared
    usleep(PREPARATION_WAIT_MILLIS * 1000);

    // start actual threads execution, i.e. test
    sem_post(&startReaderSema);
    sem_post(&startWriterSema);

    // join threads and free resources
    pthread_join(writer, NULL);
    pthread_join(reader, NULL);
    sem_wait(&endSema);
    sem_wait(&endSema);

    sem_destroy(&endSema);
    sem_destroy(&startWriterSema);
    sem_destroy(&startReaderSema);

    fprintf(stderr, "Iteration %d finished (%d)\n", iter, savedRead);
}

int main() {
    for (int iter = 0; iter < TEST_ITERS; ++iter) {
        clearState(iter);
        test(iter);
    }
    return 0;
}
