/*
 * Copyright (C) 2014-2017 Freie Universität Berlin
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     core_sched
 * @{
 *
 * @file
 * @brief       Scheduler implementation
 *
 * @author      Kaspar Schleiser <kaspar@schleiser.de>
 * @author      René Kijewski <rene.kijewski@fu-berlin.de>
 * @author      Hauke Petersen <hauke.petersen@fu-berlin.de>
 *
 * @}
 */

#include <stdint.h>
#include <inttypes.h>

#include "assert.h"
#include "bitarithm.h"
#include "clist.h"
#include "irq.h"
#include "log.h"
#include "sched.h"
#include "thread.h"
#include "panic.h"

#ifdef MODULE_MPU_STACK_GUARD
#include "mpu.h"
#endif

#define ENABLE_DEBUG 0
#include "debug.h"

#ifdef PICOLIBC_TLS
#include <picotls.h>
#endif

/* Needed by OpenOCD to read sched_threads */
#if defined(__APPLE__) && defined(__MACH__)
 #define FORCE_USED_SECTION __attribute__((used)) __attribute__((section( \
                                                                     "__OPENOCD,__openocd")))
#else
 #define FORCE_USED_SECTION __attribute__((used)) __attribute__((section( \
                                                                     ".openocd")))
#endif

/**
 * @brief   Symbols also used by OpenOCD, keep in sync with src/rtos/riot.c
 * @{
 */
volatile kernel_pid_t sched_active_pid = KERNEL_PID_UNDEF;											// Inizializzazione del thread attivo (KERNEL_PID_UNDEF = 0, un valore non valido, deve essere diverso da zero)
volatile thread_t *sched_threads[KERNEL_PID_LAST + 1];												// Definisce un array di puntatori ai threads nello scheduler
volatile int sched_num_threads = 0;																// Inizializza i threads nello scheduler (0 all'inizio ovviamente)

static_assert(SCHED_PRIO_LEVELS <= 32, "SCHED_PRIO_LEVELS may at most be 32");						// Si assicura che i livelli di priorità possono essere totalpiù 32, se così non fosse viene stampata la frase tra virgolette come errore. Di default sono 16.

FORCE_USED_SECTION
const uint8_t max_threads = ARRAY_SIZE(sched_threads);											// La variabile "max_threads" riporta la dimensione dell'array sched_threads

#ifdef DEVELHELP
/* OpenOCD can't determine struct offsets and additionally this member is only
 * available if compiled with DEVELHELP */
FORCE_USED_SECTION
const uint8_t _tcb_name_offset = offsetof(thread_t, name);											// Salva l'offset della variabile "name" (di tipo const char) della struttura thread_t
#endif
/** @} */

volatile thread_t *sched_active_thread;															// Definisce un puntatore al thread attivo
volatile unsigned int sched_context_switch_request;													// Segnala una richiesta di cambio di contesto

clist_node_t sched_runqueues[SCHED_PRIO_LEVELS];												// Definisce un array di liste, le quali sono code di esecuzione
static uint32_t runqueue_bitcache = 0;																// Definisce una cache per memorizzare lo stato delle code di esecuzione. Se il bit 0 è impostato su 1, indica che c'è almeno un thread nella coda di priorità 0,
																							// se il bit 1 è impostato su 1, indica che c'è almeno un thread nella coda di priorità 1, e così via.

#ifdef MODULE_SCHED_CB
static void (*sched_cb)(kernel_pid_t active_thread,													// Puntatore a funzione, restituisce void. Pezzo letto solo se esiste un modulo di callback
                        kernel_pid_t next_thread) = NULL;
#endif

/* Depending on whether the CLZ instruction is available, the order of the
 * runqueue_bitcache is reversed. When the instruction is available, it is
 * faster to determine the MSBit set. When it is not available it is faster to
 * determine the LSBit set. These functions abstract the runqueue modifications
 * and readout away, switching between the two orders depending on the CLZ
 * instruction availability
 */
static inline void _set_runqueue_bit(uint8_t priority)													// Modifica il bit nella posizione "priority" della variabile "runqueue_bitcache" definita prima, impostandolo a 1
{
#if defined(BITARITHM_HAS_CLZ)
    runqueue_bitcache |= BIT31 >> priority;															// Se CLZ (libreria) è disponibile allora parte dal bit più significativo e shifta di "priority" posizioni
#else
    runqueue_bitcache |= 1UL << priority;															// Altrimenti parte dal bit meno significativo e shifta di "priority" posizioni
#endif
}

static inline void _clear_runqueue_bit(uint8_t priority)												// Modifica il bit nella posizione "priority" della variabile "runqueue_bitcache" definita prima, impostandolo a 0
{
#if defined(BITARITHM_HAS_CLZ)
    runqueue_bitcache &= ~(BIT31 >> priority);
#else
    runqueue_bitcache &= ~(1UL << priority);
#endif
}

static inline unsigned _get_prio_queue_from_runqueue(void)											// Restituisce la priorità della coda corrente sulla base del valore di runqueue_bitcache. La priorità viene determinata 
																							// in base al bit più significativo (MSB) impostato a 1 nel valore di runqueue_bitcache se CLZ è disponibile, altrimenti
																							// in base al bit meno significativo (LSB)
{
#if defined(BITARITHM_HAS_CLZ)
    return 31 - bitarithm_msb(runqueue_bitcache);
#else
    return bitarithm_lsb(runqueue_bitcache);
#endif
}

static void _unschedule(thread_t *active_thread)													// Mette un thread in stato di pending ed esegue delle verifiche sullo stack
{
    if (active_thread->status == STATUS_RUNNING) {
        active_thread->status = STATUS_PENDING;
    }

#if IS_ACTIVE(SCHED_TEST_STACK)
    /* All platforms align the stack to word boundaries (possible wasting one
     * word of RAM), so this access is not unaligned. Using an intermediate
     * cast to uintptr_t to silence -Wcast-align
     */
    if (*((uintptr_t *)(uintptr_t)active_thread->stack_start) !=
        (uintptr_t)active_thread->stack_start) {
        LOG_ERROR(
            "scheduler(): stack overflow detected, pid=%" PRIkernel_pid "\n",
            active_thread->pid);
        core_panic(PANIC_STACK_OVERFLOW, "STACK OVERFLOW");										//  Confronta il valore iniziale dello stack del thread con il valore memorizzato nella posizione di memoria corrispondente. 
																							// Se i due valori sono diversi, viene rilevato un potenziale "overflow" dello stack e viene stampato un errore.
    }
#endif
#ifdef MODULE_SCHED_CB
    if (sched_cb) {
        sched_cb(active_thread->pid, KERNEL_PID_UNDEF);
    }
#endif
}																							// Se è presente il modulo di callback dello scheduler e la variabile sched_cb è stata definita, viene chiamata la funzione di callback con i 
																							// parametri active_thread->pid e KERNEL_PID_UNDEF

thread_t *__attribute__((used)) sched_run(void)														// Questa funzione serve per gestire il cambio di contesto tra thread
{
    thread_t *active_thread = thread_get_active();														// La funzione prende il PID del thread attivo e lo immagazzina in una variabile
    thread_t *previous_thread = active_thread;														// Crea una variabile di "copia" del thread attivo

    if (!IS_USED(MODULE_CORE_IDLE_THREAD) && !runqueue_bitcache) {								// Se il modulo di idle thread non è utilizzato ed il bit di runqueue è vuoto
        if (active_thread) {																			// Viene deschedulato il thread se esistente
            _unschedule(active_thread);
            active_thread = NULL;
        }

        do {
            sched_arch_idle();
        } while (!runqueue_bitcache);																// Lo scheduler entra in idle mode finché non entra un thread nella coda
    }

    sched_context_switch_request = 0;																// A questo punto viene azzerata la richiesta di context switch

    unsigned nextrq = _get_prio_queue_from_runqueue();												// Restituisce la prossima coda
    thread_t *next_thread = container_of(sched_runqueues[nextrq].next->next,
                                         thread_t, rq_entry);														// Viene ottenuto il PID del prossimo thread

#if (IS_USED(MODULE_SCHED_RUNQ_CALLBACK))
    sched_runq_callback(nextrq);																	// Viene chiamata la funzione di callback se esiste il modulo
#endif

    DEBUG(
        "sched_run: active thread: %" PRIkernel_pid ", next thread: %" PRIkernel_pid "\n",
        (kernel_pid_t)((active_thread == NULL)
                       ? KERNEL_PID_UNDEF
                       : active_thread->pid),
        next_thread->pid);																			// Viene stampato un messaggio di debug con l'ID del thread attivo e del prossimo thread.

    next_thread->status = STATUS_RUNNING;														// Lo status del prossimo thread viene messo in running

    if (previous_thread == next_thread) {															// Si controlla se il thread attivo ed il prossimo thread sono gli stessi
#ifdef MODULE_SCHED_CB
        /* Call the sched callback again only if the active thread is NULL. When
         * active_thread is NULL, there was a sleep in between descheduling the
         * previous thread and scheduling the new thread. Call the callback here
         * again ensures that the time sleeping doesn't count as running the
         * previous thread
         */
        if (sched_cb && !active_thread) {
            sched_cb(KERNEL_PID_UNDEF, next_thread->pid);												// Se il thread attivo è NULL, viene chiamata la callback sched_cb con l'ID del prossimo thread.
        }
#endif
        DEBUG("sched_run: done, sched_active_thread was not changed.\n");								// Viene stampato un messaggio di debug.
    }
    else {																						// Se i due thread sono diversi
        if (active_thread) {
            _unschedule(active_thread);																// Il thread attivo (se esiste) viene deschedulato
        }

        sched_active_pid = next_thread->pid;															// Viene impostato il PID del prossimo thread
        sched_active_thread = next_thread;															// Viene impostato il prossimo thread

#ifdef MODULE_SCHED_CB
        if (sched_cb) {
            sched_cb(KERNEL_PID_UNDEF, next_thread->pid);												// Viene chiamata la funzione sched_cb con l'id del prossimo thread
        }
#endif

#ifdef PICOLIBC_TLS
        _set_tls(next_thread->tls);																	// Viene impostato il TLS (Thread Local Storage) del prossimo thread se abilitato
#endif

#ifdef MODULE_MPU_STACK_GUARD
        mpu_configure(
            2,                                              /* MPU region 2 */
            (uintptr_t)next_thread->stack_start + 31,       /* Base Address (rounded up) */
            MPU_ATTR(1, AP_RO_RO, 0, 1, 0, 1, MPU_SIZE_32B) /* Attributes and Size */
            );																						// Se è abilitata la MPU (Memory Protection Unit) per la protezione dello stack, viene configurata la regione MPU.
#endif
        DEBUG("sched_run: done, changed sched_active_thread.\n");										// Messaggio di debug
    }

    return next_thread;																			// Viene restituito il puntatore al prossimo thread.
}

/* Note: Forcing the compiler to inline this function will reduce .text for applications
 *       not linking in sched_change_priority(), which benefits the vast majority of apps.
 */
static inline __attribute__((always_inline)) void _runqueue_push(thread_t *thread, uint8_t priority)				// Inserisce un thread nella runqueue con la priorità specificata
{
    DEBUG("sched_set_status: adding thread %" PRIkernel_pid " to runqueue %" PRIu8 ".\n",
          thread->pid, priority);																		// Indica l'inserimento del thread nella coda
    clist_rpush(&sched_runqueues[priority], &(thread->rq_entry));										// Inserisce il puntatore all'elemento rq_entry del thread nella runqueue corrispondente alla priorità.
    _set_runqueue_bit(priority);																	// Imposta il bit corrispondente alla priorità nella variabile runqueue_bitcache.

    /* some thread entered a runqueue
     * if it is the active runqueue
     * inform the runqueue_change callback */
#if (IS_USED(MODULE_SCHED_RUNQ_CALLBACK))
    thread_t *active_thread = thread_get_active();														// Si prende il PID del thread attivo
    if (active_thread && active_thread->priority == priority) {												// Viene controllato se il thread attivo ha la stessa priorità della runqueue in cui è stato inserito il nuovo thread.
        sched_runq_callback(priority);																// Allora viene chiamata la funzione di callback
    }
#endif
}

/* Note: Forcing the compiler to inline this function will reduce .text for applications
 *       not linking in sched_change_priority(), which benefits the vast majority of apps.
 */
static inline __attribute__((always_inline)) void _runqueue_pop(thread_t *thread)							// A differenza della funzione precedente, fa il pop di un thread dalla coda
{
    DEBUG("sched_set_status: removing thread %" PRIkernel_pid " from runqueue %" PRIu8 ".\n",
          thread->pid, thread->priority);																// Indica la rimozione del thread dalla coda
    clist_lpop(&sched_runqueues[thread->priority]);													// Rimuove il thread dalla coda

    if (!sched_runqueues[thread->priority].next) {														// Se la runqueue è vuota viene chiamata la funzione di callback
        _clear_runqueue_bit(thread->priority);
#if (IS_USED(MODULE_SCHED_RUNQ_CALLBACK))
        sched_runq_callback(thread->priority);
#endif
    }
}

void sched_set_status(thread_t *process, thread_status_t status)										// Imposta lo status di un thread prendendo in argomento il thread e lo status da impostare
{
    if (status >= STATUS_ON_RUNQUEUE) {															// Se lo stato è maggiore o uguale a STATUS_ON_RUNQUEUE, significa che il thread deve essere inserito nella runqueue.
        if (!(process->status >= STATUS_ON_RUNQUEUE)) {												// Viene controllato se il thread non era già nella runqueue (cioè il suo stato non era già maggiore o uguale a STATUS_ON_RUNQUEUE).
            _runqueue_push(process, process->priority);													// In tal caso, la funzione _runqueue_push viene chiamata per aggiungere il thread alla runqueue con la priorità corrispondente al campo priority del thread.
        }
    }
    else {																						// Altrimenti deve essere rimosso dalla coda
        if (process->status >= STATUS_ON_RUNQUEUE) {
            _runqueue_pop(process);
        }
    }

    process->status = status;																		// Infine, lo stato del thread viene impostato al valore specificato.
}

void sched_switch(uint16_t other_prio)															// Funzione che gestisce i cambi di contesto tra thread
{
    thread_t *active_thread = thread_get_active();														// Prende il PID del thread attivo
    uint16_t current_prio = active_thread->priority;													// Prende la priorità del thread attivo
    int on_runqueue = (active_thread->status >= STATUS_ON_RUNQUEUE);								// Assegna alla variabile on_runqueue il valore booleano (0 o 1) che indica se il thread attivo è presente nella runqueue o meno.

    DEBUG("sched_switch: active pid=%" PRIkernel_pid " prio=%" PRIu16 " on_runqueue=%i "
          ", other_prio=%" PRIu16 "\n",
          active_thread->pid, current_prio, on_runqueue,
          other_prio);

    if (!on_runqueue || (current_prio > other_prio)) {													// Viene verificato se il thread attivo non è nella runqueue (!on_runqueue) o se ha una priorità maggiore dell'altro thread (current_prio > other_prio).
        if (irq_is_in()) {																			// Se uno di questi due casi è vero, viene verificato se la funzione è stata chiamata all'interno di un'interfaccia di interrupt
            DEBUG("sched_switch: setting sched_context_switch_request.\n");
            sched_context_switch_request = 1;															// Se è così, viene impostata la variabile sched_context_switch_request a 1 per richiedere un cambio di contesto.
        }
        else {
            DEBUG("sched_switch: yielding immediately.\n");
            thread_yield_higher();																	// Altrimenti, viene chiamata la funzione thread_yield_higher() per passare immediatamente il controllo a un thread di priorità superiore.
        }
    }
    else {
        DEBUG("sched_switch: continuing without yield.\n");												// Se il thread attivo è nella runqueue e ha una priorità inferiore o uguale all'altro thread, la funzione termina senza effettuare alcun cambio di contesto.
    }
}

NORETURN void sched_task_exit(void)															// Termina il thread corrente e gestisce le operazioni necessarie prima del termine.
{
    DEBUG("sched_task_exit: ending thread %" PRIkernel_pid "...\n",
          thread_getpid());

#if defined(MODULE_TEST_UTILS_PRINT_STACK_USAGE) && defined(DEVELHELP)						// Se è definita la macro MODULE_TEST_UTILS_PRINT_STACK_USAGE e DEVELHELP
    void print_stack_usage_metric(const char *name, void *stack, unsigned max_size);						// Viene chiamata la funzione print_stack_usage_metric per stampare l'utilizzo dello stack del thread corrente
    thread_t *me = thread_get_active();
    print_stack_usage_metric(me->name, me->stack_start, me->stack_size);
#endif

    (void)irq_disable();																			// Disabilita gli interrupt chiamando la funzione irq_disable(). Questo impedisce che vengano eseguiti interrupt durante la terminazione del thread.
    sched_threads[thread_getpid()] = NULL;															// Imposta l'elemento corrispondente dell'array sched_threads a NULL. Questo indica che il thread è terminato e non è più presente nell'array dei thread schedulabili.
    sched_num_threads--;																		// Decrementa il contatore sched_num_threads per tener traccia del numero totale di thread schedulabili.

    sched_set_status(thread_get_active(), STATUS_STOPPED);											// Imposta lo stato del thread corrente a STATUS_STOPPED chiamando la funzione sched_set_status. Questo indica che il thread è stato fermato.

    sched_active_thread = NULL;																	// Imposta il puntatore sched_active_thread a NULL per indicare che non c'è alcun thread attivo.
    cpu_switch_context_exit();																	// Chiama la funzione cpu_switch_context_exit() per gestire la terminazione del contesto del thread e passare ad un altro thread o alla logica di spegnimento del 
																							// sistema operativo, a seconda dell'implementazione specifica.


}

#ifdef MODULE_SCHED_CB																		// Questa macro serve per abilitare il modulo di callback di schedulazione. Se la macro non è definita, la funzione non viene compilata.
void sched_register_cb(void (*callback)(kernel_pid_t, kernel_pid_t))										// La funzione accetta un parametro callback, che è un puntatore a una funzione che prende due argomenti di tipo kernel_pid_t (identificatore di thread).
{
    sched_cb = callback;																			// La funzione assegna il valore del puntatore callback alla variabile globale sched_cb, che è un puntatore a una funzione di callback di schedulazione. 
																							// In questo modo, la funzione di callback viene registrata per essere utilizzata in seguito durante il processo di schedulazione.
}
#endif

void sched_change_priority(thread_t *thread, uint8_t priority)											// La funzione sched_change_priority viene utilizzata per cambiare la priorità di un thread specificato.
{
    assert(thread && (priority < SCHED_PRIO_LEVELS));												// Verifica che il puntatore thread non sia nullo e che la priorità priority sia inferiore al numero massimo di livelli di priorità SCHED_PRIO_LEVELS

    if (thread->priority == priority) {																	// Controlla se la priorità corrente del thread è uguale alla nuova priorità priority. Se lo sono, non fa nulla e termina la funzione.

        return;
    }

    unsigned irq_state = irq_disable();																// Disabilita le interruzioni con irq_disable() per garantire un'operazione atomica.

    if (thread_is_active(thread)) {																	// Se il thread è attivo
        _runqueue_pop(thread);																		// Lo rimuove dalla coda attuale
        _runqueue_push(thread, priority);																// E lo inserisce nella nuova
    }
    thread->priority = priority;																		// Quindi cambia la priorità del thread con quella nuova

    irq_restore(irq_state);																		// Riabilita le richieste di interrupt

    thread_t *active = thread_get_active();															// Ottiene il thread attivo corrente utilizzando thread_get_active().

    if ((active == thread)																			// Se il thread attivo è il thread specificato (active == thread), il che significa che la funzione sched_change_priority è stata chiamata per il thread attivo stesso.
        || ((active != NULL) && (active->priority > priority) && thread_is_active(thread))						// Se il thread attivo non è nullo (active != NULL) e la sua priorità è maggiore della nuova priorità priority, il che significa che un altro thread ha una priorità superiore 
																							// al thread attivo corrente e dovrebbe essere eseguito immediatamente.
        ) {
        /* If the change in priority would result in a different decision of
         * the scheduler, we need to yield to make sure the change in priority
         * takes effect immediately. This can be due to one of the following:
         *
         * 1) The priority of the thread currently running has been reduced
         *    (higher numeric value), so that other threads now have priority
         *    over the currently running.
         * 2) The priority of a pending thread has been increased (lower numeric value) so that it
         *    now has priority over the running thread.
         */
        thread_yield_higher();																		// Se uno di questi controlli è vero, chiama la funzione thread_yield_higher() per eseguire una commutazione di contesto immediata, in modo che la modifica
																							// di priorità abbia effetto immediato.
    }
}