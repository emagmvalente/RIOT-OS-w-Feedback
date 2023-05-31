#include "sched.h"																			// Inclusa per usufruire degli stati dei thread e degli stati delle code
#include "thread.h"																			// Definisce i thread
#include "ztimer.h"																			// Definisce i timer
#include "sched_round_robin.h"																// Definisce macro utili per i timer

#define ENABLE_DEBUG 0
#include "debug.h"																			// Fornisce messaggi di errore utili

#define MAX_Q 3

static void _sched_feedback_cb(void *d);

static ztimer_t _fb_timer = { .callback = _sched_feedback_cb };										// Quando il timer termina viene chiamata questa funzione

static uint8_t _current_fb_priority = 0;															// La prima coda di priorità è la zero, poiché essa non è runnabile

void sched_runq_callback(uint8_t prio);														// Viene definita la funzione di callback, questa funzione è legata alla funzione in sched.c







void _sched_feedback_cb(void *d)															// Questa è la funzione che viene chiamata quando il timer termina, quindi si presuppone che si arrivi a questo punto quando un thread conclude il suo quantum
{

    	(void)d;
	uint8_t prio = _current_fb_priority;															// Preleva la coda corrente
	thread_t *active_thread = thread_get_active();												// Preleva il PID del thread che era in running
	uint8_t active_priority = active_thread->priority;												// Ne prelevo la priorità



	if(prio != 0xff ){
		_current_fb_priority = 0xff;															// Disattiva la coda corrente per evitare che venga eseguita durante l'esecuzione della funzione
	}


	if(active_thread->service_time == 0){
		active_thread->status = 0;
		sched_runq_advance(active_thread->priority);
		thread_yield_higher();
	}


	if(active_thread && active_thread->service_time !=0) {										// Se il thread non è nullo
		if (active_priority == prio && active_priority < MAX_Q) {									// Se la priorità del thread è uguale a quella che era corrente
			
			sched_change_priority(active_thread, active_priority+1);								// Allora faccio cambiare coda al thread aumentando la priorità

			if(sched_runq_is_empty(prio)) {													// Se dopo il cambio di priorità la coda è vuota
				prio++;																	// Prio incrementa
			}
			

		}

		else if(active_priority == MAX_Q){													// Se la priorità del thread è 3
			sched_runq_advance(MAX_Q);													// FIFO
			thread_yield_higher();
		}
		active_thread->service_time -= 500000;
	}

	sched_runq_callback(prio);																// Richiamo la funzione di callback con la priorità passata

}



static inline void _sched_feedback_set(uint8_t prio)
{
    if (prio == 0) {
        return;
    }
    _current_fb_priority = prio;
    ztimer_set(SCHED_RR_TIMERBASE, &_fb_timer, 500000);
}








void sched_runq_callback(uint8_t prio)
{
    if (prio == 0) {
        return;
    }
    if (_current_fb_priority == 0xff) {
        _sched_feedback_set(prio);
    }

}









void sched_feedback_init(void)
{

    	_current_fb_priority = 0xff;																// Viene settata la priorità come non inizializzata
    	thread_t *active_thread = thread_get_active();
	sched_change_priority(active_thread, 1);
    	if (active_thread) {
        	sched_runq_callback(active_thread->priority);
    	}
}
