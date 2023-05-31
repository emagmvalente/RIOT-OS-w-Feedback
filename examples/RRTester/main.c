#include <stdio.h>																			// Libreria di input output
#include <stdlib.h>																		// Libreria con dentro cose utili
#include <stdint.h>																		// Libreria con dentro cose utili
#include <unistd.h>
#include <string.h>

#include "thread.h"																			// Header di definizione dei thread
#include "sched.h"																			// Header di definizione dello scheduler
#include "xtimer.h"

#include "ztimer.h"																			// Header di definizione dei timer
#include "timex.h"																			// Utile per comparare due timestamps

#include "sched_round_robin.h"																// Utilizzo dell'header relativo al round robin


#define PRINT_STEPS 500																	// Rappresenta il numero di passi (steps) tra le stampe dei messaggi di debug o di log durante l'esecuzione del thread worker
#define WORK_SCALE  1000																	// Rappresenta un fattore di scala utilizzato per regolare la quantità di lavoro effettuata dal thread worker
#define STEPS_PER_SET 10																	// Rappresenta il numero totale di passi (steps) per ogni set di lavoro del thread worker



/* Tecniche di riposo */

__attribute__((unused))
static void bad_wait(uint32_t us)
{
    /* keep the CPU busy waiting for some time to pass simulate working */
    ztimer_spin(ZTIMER_USEC, us);															// Occupa la CPU
}

static void (* const do_work)(uint32_t us) = bad_wait;

__attribute__((unused))
static void nice_wait(uint32_t us)																// Attende
{
    /* be nice give the CPU some time to do other things or rest */
    ztimer_sleep(ZTIMER_USEC, us);
}


__attribute__((unused))
static void no_wait(uint32_t unused)															// Non fa niente di particolare
{
    (void) unused;
    /* do not wait */
}


/* Ogni thread avrà una tecnica di riposo tra quelle sopra ed un carico di lavoro da eseguire, qui li definisco */

struct worker_config {
    void (*waitfn)(uint32_t);  /**< the resting strategy */											// Puntatore a funzione, definisce la strategia di attesa usata dal thread worker (yield, no wait, nice wait ecc...)
    uint32_t workload;         /**< the amount of work to do per set */									// Rappresenta la quantità di lavoro da eseguire per ogni set
};



/* Definisco i campi di un thread in esecuzione */


void * thread_worker(void * d)
{
    nice_wait(200 *  US_PER_MS);  /* always be nice at start */										// Fornisce un ritardo al thread prima di iniziare la mansione 
#ifdef DEVELHELP
    //const char *name = thread_get_active()->name;												// Se è definita DEVELHELP viene preso il nome del thread attivo
#else
    int16_t pid = thread_getpid();																// Se non è definita viene preso il PID
#endif

    uint32_t w = 0;																			// w terrà traccia dell'avanzamento del thread worker
    struct worker_config *wc =  d;																// Puntatore a d, che punta alla struttura "worker_config" che contiene la configurazione del thread worker
    /* Each set consists of STEPS_PER_SET steps which are divided into work (busy waiting)
     * and resting.
     * E.g. if there are 10 steps per set, the maximum workload is 10, which means no rest.
     * If the given value is out of range work ratio is set to half of STEPS_PER_SET */
    uint32_t work = wc->workload;																// Quantità di lavoro da eseguire per ogni set
    if (work > STEPS_PER_SET) {
        work = STEPS_PER_SET / 2;																// Se il lavoro è maggiore di steps per set, allora viene diviso per due, così da avere un rapporto coerente tra lavoro e rest
    }
    uint32_t rest = (STEPS_PER_SET - work);														// La quantità di riposo è data da steps per set - work

    /* work some time and rest */
    for (;;) {																				// Ciclo infinito
        do_work(work * WORK_SCALE);															// Rappresenta il lavoro effettivo che deve essere svolto dal thread
        w += work;																			// w viene incrementato di work
        wc->waitfn(rest * WORK_SCALE);															// Rappresenta la funzione di attesa o di riposo del thread
    }
}


/* Definisco i thread, la loro tecnica di riposo ed il carico di lavoro */


#ifndef  THREAD_1
#define  THREAD_1 {no_wait, 2000000}														// Il thread 1 non utilizza alcun tipo di tecnica di riposo ed il suo carico di lavoro è di 5.
#define S_TIME1 3000000
#endif

#ifndef  THREAD_2
#define  THREAD_2 {no_wait, 2000000}
#define S_TIME2 6000000
#endif

#ifndef  THREAD_3
#define  THREAD_3 {no_wait, 2000000}
#define S_TIME3 4000000
#endif

#ifndef  THREAD_4
#define  THREAD_4 {no_wait, 2000000}
#define S_TIME4 5000000
#endif

#ifndef  THREAD_5
#define  THREAD_5 {no_wait, 2000000}
#define S_TIME5 2000000
#endif

#ifndef WORKER_STACKSIZE
#define WORKER_STACKSIZE (THREAD_STACKSIZE_SMALL+THREAD_EXTRA_STACKSIZE_PRINTF)		// Dato che i thread sono 5, alloco uno spazio un po' più grande di TINY (come era nel codice originale)
#endif

thread_t* TA;
thread_t* TB;
thread_t* TC;
thread_t* TD;
thread_t* TE;

void stampa(void);
void callback_timer(void *d){
	(void)d;
	stampa();
}

static ztimer_t tout = { .callback = callback_timer};

static inline void tset(void)
{
    ztimer_set(SCHED_RR_TIMERBASE, &tout, 500000);
}

int min(int a, int b, int c, int d, int e)
{
    int minValue = a;
    if (b < minValue) {
        minValue = b;
    }
    if (c < minValue) {
        minValue = c;
    }
    if (d < minValue) {
        minValue = d;
    }
    if (e < minValue) {
        minValue = e;
    }
    return minValue;
}



const char* return_state(int state) {
    switch (state) {
        case 11:
            return "Running";
        case 12:
            return "Pending";
        case 0:
            return "Stopped";
        default:
            return "Unknown";
    }
}



void stampa(void){
	tset();
	system("clear");
	printf("Threads switches are visible by watching their status changing.\n\n");
	printf("In Order: Thread Name - Actual Queue - Remaining Time (ms) - Status\n\n");
	printf(" %s: %d %" PRIu32 " %s \n", TA->name, TA->priority, TA->service_time / 1000, return_state(TA->status));
	printf(" %s: %d %" PRIu32 " %s \n", TB->name, TB->priority, TB->service_time / 1000, return_state(TB->status));
	printf(" %s: %d %" PRIu32 " %s \n", TC->name, TC->priority, TC->service_time / 1000, return_state(TC->status));
	printf(" %s: %d %" PRIu32 " %s \n", TD->name, TD->priority, TD->service_time / 1000, return_state(TD->status));
	printf(" %s: %d %" PRIu32 " %s \n\n", TE->name, TE->priority, TE->service_time / 1000, return_state(TE->status));

	if(min(TA->priority, TB->priority, TC->priority, TD->priority, TE->priority) == 1){
		printf("Currently in the 1st queue.\n");
	}

	else if(min(TA->priority, TB->priority, TC->priority, TD->priority, TE->priority) == 2){
		printf("Currently in the 2nd queue.\n");
	}

	else if(min(TA->priority, TB->priority, TC->priority, TD->priority, TE->priority) == 3 && (TA->status!=0 || TB->status!=0 || TC->status!=0 || TD->status!=0 || TE->status!=0)){
		printf("Currently in the 3rd queue.\n");
	}

	else if(TA->service_time==0 && TB->service_time==0 && TC->service_time==0 && TD->service_time==0 && TE->service_time==0 &&
	    TA->status==0 && TB->status==0 && TC->status==0 && TD->status==0 && TE->status==0){
		printf("Terminated. Removing threads from scheduler.");
		sched_task_exit();
	}
}


int main(void)
{
    {
        static char stack[WORKER_STACKSIZE];
        static struct worker_config wc = THREAD_1;   /* 0-10 workness */
	short int PIDTA = thread_create_feedback(stack, sizeof(stack), 1, THREAD_CREATE_STACKTEST,
                      thread_worker, &wc, "TA", S_TIME1);
	TA = thread_get(PIDTA);
    }
    {
        static char stack[WORKER_STACKSIZE];
        static struct worker_config wc = THREAD_2;   /* 0-10 workness */
        short int PIDTB = thread_create_feedback(stack, sizeof(stack), 1, THREAD_CREATE_STACKTEST,
                      thread_worker, &wc, "TB", S_TIME2);
	TB = thread_get(PIDTB);
    }
    {
        static char stack[WORKER_STACKSIZE];
        static struct worker_config wc = THREAD_3;   /* 0-10 workness */
        short int PIDTC = thread_create_feedback(stack, sizeof(stack), 1, THREAD_CREATE_STACKTEST,
                      thread_worker, &wc, "TC", S_TIME3);
	TC = thread_get(PIDTC);
    }
    {
        static char stack[WORKER_STACKSIZE];
        static struct worker_config wc = THREAD_4;   /* 0-10 workness */
        short int PIDTD = thread_create_feedback(stack, sizeof(stack), 1, THREAD_CREATE_STACKTEST,
                      thread_worker, &wc, "TD", S_TIME4);
	TD = thread_get(PIDTD);
    }
    {
        static char stack[WORKER_STACKSIZE];
        static struct worker_config wc = THREAD_5;   /* 0-10 workness */
        short int PIDTE = thread_create_feedback(stack, sizeof(stack), 1, THREAD_CREATE_STACKTEST,
                      thread_worker, &wc, "TE", S_TIME5);
	TE = thread_get(PIDTE);
    }
	stampa();
}