#ifndef MUTUA_EVENTS_QUEUEEVENTDISPATCHER_H_
#define MUTUA_EVENTS_QUEUEEVENTDISPATCHER_H_

#include <iostream>
#include <thread>
#include <mutex>
#include <pthread.h>
#include <type_traits>
#include <stdexcept>
using namespace std;

#include <BetterExceptions.h>
//using namespace mutua::cpputils;


namespace mutua::events {

	/**
     * QueueEventDispatcher.h
     * ======================
     * created (in C++) by luiz, Nov 5, 2018
     *
     * Responsible for dispatching events to consumers and observers.
     *
     * Implementation of 'EventDispatcher' specialized to deal with 'QueueEventLinks'.
     *
    */
	template <class _QueueEventLink>
	struct QueueEventDispatcher {

		// types from _QueueEventLink:
		typedef  decltype(_QueueEventLink::QueueElement::answerObjectReference) _AnswerTypePointer;
		typedef std::remove_pointer<_AnswerTypePointer>                         _AnswerType;
		typedef decltype(_QueueEventLink::QueueElement::eventParameter)         _ArgumentType;

		bool             isActive;
		_QueueEventLink& el;
		int              nThreads;
		thread*          threads;

		string (*eventParameterToStringSerializer) (const _ArgumentType&);

		/** Instantiate a QueueEventDispatcher with the given  */
		QueueEventDispatcher(_QueueEventLink& el,
		                     int              nThreads,
		                     int              threadsPriority,
							 bool             zeroCopy,
							 bool             notifyEvents,
							 bool             consumeAnswerlessEvents,
							 bool             consumeAnswerfullEvents,
							 bool             debug)
				: isActive(true)
				, el(el)
				, nThreads(nThreads+(debug ? 1 : 0)) {

			// checks
			if (threadsPriority != 0) {
                THROW_EXCEPTION(invalid_argument, "QueueEventDispatcher: Attempting to create a dispatcher for event '"+el.eventName+"' with custom " +
                                                  "'threadsPriority', and this is not implemented yet -- it must be zero in the meantime.");
			}
			if ( consumeAnswerlessEvents && (el.answerlessConsumerProcedureReference == nullptr) ) {
				THROW_EXCEPTION(runtime_error, "QueueEventDispatcher: Attempting instantiate 'QueueEventDispatcher' before a consumer was set in QueueEventLink. This limitation might be improved in the future.");
			}
			if ( consumeAnswerlessEvents && (nThreads > el.nAnswerlessConsumerThese) ) {
				THROW_EXCEPTION(runtime_error, "QueueEventDispatcher: Attempting to create a dispatcher for event '"+el.eventName+"' with "+to_string(nThreads)+" threads, " +
                                               "but the given QueueEventLink is set to have only "+to_string(el.nAnswerlessConsumerThese)+" consumer objects on the instance pool " +
                                               "and this combination is not optimal. Please, arrange that -- most probably by increasing the array of objects given to 'setAnswerlessConsumer(...)'.\n" +
											   "note: by now you must only instantiate a 'QueueEventDispatcher' after you have set the QueueEventLink consumer. This limitation might be improved in the future.");
			}
			if ( consumeAnswerfullEvents && (nThreads > el.nAnswerfullConsumerThese) ) {
				THROW_EXCEPTION(runtime_error, "QueueEventDispatcher: Attempting to create a dispatcher for event '"+el.eventName+"' with "+to_string(nThreads)+" threads, " +
                                               "but the given QueueEventLink is set to have only "+to_string(el.nAnswerfullConsumerThese)+" consumer objects on the instance pool " +
                                               "and this combination is not optimal. Please, arrange that -- most probably by increasing the array of objects given to 'setAnswerfullConsumer(...)'.\n" +
											   "note: by now you must only instantiate a 'QueueEventDispatcher' after you have set the QueueEventLink consumer. This limitation might be improved in the future.");
			}

			threads = new thread[nThreads+(debug ? 1 : 0)];

			for (int i=0; i<nThreads; i++) {
				/**/ if ( zeroCopy &&  notifyEvents &&  consumeAnswerlessEvents && !consumeAnswerfullEvents )
					threads[i] = thread(&QueueEventDispatcher::dispatchZeroCopyListeneableAndConsumableAnswerlessEventsLoop, this, i, el.answerlessConsumerThese[i%el.nAnswerlessConsumerThese]);
				else if ( zeroCopy &&  notifyEvents && !consumeAnswerlessEvents &&  consumeAnswerfullEvents )
					threads[i] = thread(&QueueEventDispatcher::dispatchZeroCopyListeneableAndConsumableAnswerfullEventsLoop, this, i, el.answerfullConsumerThese[i%el.nAnswerfullConsumerThese]);
				else if ( zeroCopy && !notifyEvents &&  consumeAnswerlessEvents && !consumeAnswerfullEvents )
					threads[i] = thread(&QueueEventDispatcher::dispatchZeroCopyConsumableAnswerlessEventsLoop,               this, i, el.answerlessConsumerThese[i%el.nAnswerlessConsumerThese]);
				else if ( zeroCopy && !notifyEvents && !consumeAnswerlessEvents &&  consumeAnswerfullEvents )
					threads[i] = thread(&QueueEventDispatcher::dispatchZeroCopyConsumableAnswerfullEventsLoop,               this, i, el.answerfullConsumerThese[i%el.nAnswerfullConsumerThese]);
				else if ( zeroCopy &&  notifyEvents && !consumeAnswerlessEvents && !consumeAnswerfullEvents )
					threads[i] = thread(&QueueEventDispatcher::dispatchZeroCopyListeneableEventsLoop,                        this, i);
				else
	                THROW_EXCEPTION(invalid_argument, "QueueEventDispatcher: Attempting to create a dispatcher for event '"+el.eventName+"' with a not implemented combination of " +
	                                                  "'zeroCopy' (" +                to_string(zeroCopy)+"), " +
	                                                  "'notifyEvents' (" +            to_string(notifyEvents)+"), " +
	                                                  "'consumeAnswerlessEvents' (" + to_string(consumeAnswerlessEvents)+") and " +
	                                                  "'consumeAnswerfullEvents' (" + to_string(consumeAnswerfullEvents)+")");
			}

			// start debugger thread?
			if (debug) {
				threads[nThreads] = thread(&QueueEventDispatcher::debugTracker, this);
			}

			setArgumentSerializer();
		}

		// default serializers
		static string defaultEventParameterToStringSerializer(const unsigned int&  argument) { return to_string(argument); }
		static string defaultEventParameterToStringSerializer(const       string&  argument) { return argument; }
		template <unsigned N>
		static string defaultEventParameterToStringSerializer(const    char (&argument) [N]) { return string(argument); }

		void setArgumentSerializer() {
			if constexpr (std::is_integral<_ArgumentType>::value || std::is_constructible<std::string, _ArgumentType>::value) {
				eventParameterToStringSerializer = static_cast<string (*) (const _ArgumentType&)>(defaultEventParameterToStringSerializer);
			} else if (std::is_class<_ArgumentType>::value) {
				eventParameterToStringSerializer = _ArgumentType::toString;
			} else {
                THROW_EXCEPTION(invalid_argument, "QueueEventDispatcher: Don't know how to serialize the given _ArgumentType for event '"+el.eventName+"'.");
			}
		}

		~QueueEventDispatcher() {

			stopASAP();

			// we will now unlock all mutexes. this may cause false wakeups. to prevent
			// calling listeners & consumers with wrong data, we will reset them
			// note: we cannot help if any external thread is waiting for an answer -- a false wakeup will occur for them
			// note 2: this wired mechanism is to prevent the usage of condition variables, which would produce false wakeups
			//         and the need for (slow) code to deal with it.

			// remove all listeners
			el.nListenerProcedureReferences = 0;

			// unset original & set dummy consumers to receive false wakeups
			el.unsetConsumer();
			el.setAnswerlessConsumer(&_QueueEventLink::dummyAnswerlessConsumer, {&el});
			el.setAnswerfullConsumer(&_QueueEventLink::dummyAnswerfullConsumer, {&el});

			// unlock any locked mutexes, allowing full & empty queue locks do proceed -- and wait a little until no mutex is locked again
			mutex* guardMutexes[] = {&el.reservationGuard, &el.dequeueGuard, &el.queueGuard};
			unsigned retries = 0;
			while (retries < nThreads*5) {	// wait a minimum of ~ 40ms without any new locks on ~4 consumers
				retries++;
				for (unsigned i=0; i<sizeof(guardMutexes)/sizeof(guardMutexes[0]); i++) {
					// prevent further lockings
					el.isFull            = false;
					el.isEmpty           = false;
					el.queueHead         = 0;
					el.queueTail         = 1;
					el.queueReservedHead = 0;
					el.queueReservedTail = 1;
					if (isMutexLocked(*guardMutexes[i])) {
						guardMutexes[i]->unlock();
						retries = 0;
					}
				}
				// unlock any answer mutexes
				for (unsigned i=0; i<el.numberOfQueueSlots; i++) {
					if (isMutexLocked(el.events[i].answerMutex)) {
						el.events[i].answerMutex.unlock();
					}
				}
				this_thread::sleep_for(chrono::milliseconds(2));
			}

			delete[] threads;
		}

		inline bool isMutexLocked(mutex& m) {
			bool isLocked = !m.try_lock();
			if (!isLocked) m.unlock();
			return isLocked;
		}

		/** Cause all threads not to process any further elements from this point on */
		void stopASAP() {
			if (isActive) {
				isActive = false;
				for (int i=0; i<nThreads; i++) {
					threads[i].detach();
				}
			}
		}

		/** Wait until queue is empty to stop all threads */
		void stopWhenEmpty() {
			int retries = 0;
			int lastQueueHead         = el.queueHead;
			int lastQueueTail         = el.queueTail;
			int lastQueueReservedHead = el.queueReservedHead;
			int lastQueueReservedTail = el.queueReservedTail;
			while (retries < nThreads*5) {	// wait a minimum of ~ 40ms without any new events on ~4 consumers
				if (el.isEmpty && (el.getQueueLength() == 0) && (el.getQueueReservedLength() == 0) &&
					(lastQueueHead         == el.queueHead)         && (lastQueueTail         == el.queueTail) &&
					(lastQueueReservedHead == el.queueReservedHead) && (lastQueueReservedTail == el.queueReservedTail)) {
					retries++;
				} else {
					retries = 0;
					lastQueueHead         = el.queueHead;
					lastQueueTail         = el.queueTail;
					lastQueueReservedHead = el.queueReservedHead;
					lastQueueReservedTail = el.queueReservedTail;
				}
				this_thread::sleep_for(chrono::milliseconds(2));
			}
			stopASAP();
		}

		inline void consumeAnswerlessEvent(
				unsigned int                                                    threadId,
				decltype(_QueueEventLink::answerlessConsumerProcedureReference) consumerMethod,
				void*                                                           consumerThis,
				const _ArgumentType&                                            eventParameter) {
			try {
				consumerMethod(consumerThis, eventParameter);
			} catch (const exception& e) {
				DUMP_EXCEPTION(runtime_error("Exception in answerless consumer: "s + e.what()),
						       "QueueEventDispatcher for event '"+el.eventName+"', thread #"+to_string(threadId)+": exception in answerless consumer " +
					           "with parameter: "+eventParameterToStringSerializer(eventParameter)+". Event consumption will not be retried, " +
					           "since a fall-back queue is not yet implemented.\n" +
				               "Caused by: "+e.what(),
				               "threadId",       to_string(threadId),
				               "consumerMethod", to_string((size_t)consumerMethod),
				               "consumerThis",   to_string((size_t)consumerThis),
				               "eventParameter", eventParameterToStringSerializer(eventParameter));
			} catch (...) {
				DUMP_EXCEPTION(runtime_error("Unknown exception in answerless consumer"),
				               "QueueEventDispatcher for event '"+el.eventName+"', thread #"+to_string(threadId)+": exception in answerless consumer " +
				               "with parameter: "+eventParameterToStringSerializer(eventParameter)+". Event consumption will not be retried, " +
				               "since a fall-back queue is not yet implemented.\n" +
				               "Caused by: <<unknown cause>>",
				               "threadId",       to_string(threadId),
				               "consumerMethod", to_string((size_t)consumerMethod),
				               "consumerThis",   to_string((size_t)consumerThis),
				               "eventParameter", eventParameterToStringSerializer(eventParameter));
			}
		}

		inline void consumeAnswerfullEvent(
				unsigned int                                                    threadId,
				decltype(_QueueEventLink::answerfullConsumerProcedureReference) consumerMethod,
				void*                                                           consumerThis,
				typename _QueueEventLink::QueueElement*                         dequeuedEvent) {
			try {
				consumerMethod(consumerThis, dequeuedEvent->eventParameter, dequeuedEvent->answerObjectReference, dequeuedEvent->answerMutex);
			} catch (const exception& e) {
				dequeuedEvent->exception = std::current_exception();
				DUMP_EXCEPTION(runtime_error("Exception in answerfull consumer: "s + e.what()),
				               "QueueEventDispatcher for event '"+el.eventName+"', thread #"+to_string(threadId)+": exception in answerfull consumer " +
				               "with parameter: "+eventParameterToStringSerializer(dequeuedEvent->eventParameter)+". Event consumption will not be retried, " +
				               "since a fall-back queue is not yet implemented.\n" +
				               "Caused By: "+e.what(),
				               "threadId",              to_string(threadId),
				               "consumerMethod",        to_string((size_t)consumerMethod),
				               "consumerThis",          to_string((size_t)consumerThis),
				               "answerObjectReference", to_string((size_t)dequeuedEvent->answerObjectReference),
				               "eventParameter",        eventParameterToStringSerializer(dequeuedEvent->eventParameter));
			} catch (...) {
				dequeuedEvent->exception = std::current_exception();
				DUMP_EXCEPTION(runtime_error("Unknown exception in answerless consumer"),
				               "QueueEventDispatcher for event '"+el.eventName+"', thread #"+to_string(threadId)+": exception in answerfull consumer " +
				               "with parameter: "+eventParameterToStringSerializer(dequeuedEvent->eventParameter)+". Event consumption will not be retried, " +
				               "since a fall-back queue is not yet implemented.\n" +
				               "Caused By: <<unknown cause>>",
				               "threadId",              to_string(threadId),
				               "consumerMethod",        to_string((size_t)consumerMethod),
				               "consumerThis",          to_string((size_t)consumerThis),
				               "answerObjectReference", to_string((size_t)dequeuedEvent->answerObjectReference),
				               "eventParameter",        eventParameterToStringSerializer(dequeuedEvent->eventParameter));

				// prepare the exception to be visible when the caller issues an 'waitForAnswer'
				if (isMutexLocked(dequeuedEvent->answerMutex)) {
					// the exception happened before the answer was issued
					dequeuedEvent->answerObjectReference = nullptr;
					dequeuedEvent->answerMutex.unlock();
				}
			}
		}

		inline void notifyEventObservers(
				unsigned int                                           threadId,
				decltype(_QueueEventLink::listenerProcedureReferences) listenerMethods,
				void*                                                 *listenersThis,
				const _ArgumentType&                                   eventParameter) {
			for (unsigned int i=0; i<el.nListenerProcedureReferences; i++) try {
				listenerMethods[i](listenersThis[i], eventParameter);
			} catch (const exception& e) {
				DUMP_EXCEPTION(runtime_error("Exception in listener: "s + e.what()),
				               "QueueEventDispatcher for event '"+el.eventName+"', thread #"+to_string(threadId)+": exception in event listener #"+to_string(i)+
				               "with parameter: "+eventParameterToStringSerializer(eventParameter)+".\n" +
				               "Caused by: "+e.what(),
				               "threadId",                         to_string(threadId),
				               "listenerMethod["+to_string(i)+"]", to_string((size_t)listenerMethods[i]),
				               "listenerThis["+to_string(i)+"]",   to_string((size_t)listenersThis[i]),
				               "eventParameter",                   eventParameterToStringSerializer(eventParameter));
			} catch (...) {
				std::exception_ptr e = std::current_exception();
				DUMP_EXCEPTION(runtime_error("Unknown Exception in listener"),
				               "QueueEventDispatcher for event '"+el.eventName+"', thread #"+to_string(threadId)+": exception in event listener #"+to_string(i)+
				               "with parameter: "+eventParameterToStringSerializer(eventParameter)+".\n" +
				               "Caused by: <<unknown cause>>",
				               "threadId",                         to_string(threadId),
				               "listenerMethod["+to_string(i)+"]", to_string((size_t)listenerMethods[i]),
				               "listenerThis["+to_string(i)+"]",   to_string((size_t)listenersThis[i]),
				               "eventParameter",                   eventParameterToStringSerializer(eventParameter));
			}
		}

		// if (zeroCopy && notifyEvents &&  consumeAnswerlessEvents && !consumeAnswerfullEvents)
		void dispatchZeroCopyListeneableAndConsumableAnswerlessEventsLoop(int threadId, void* consumerThis) {
			typename _QueueEventLink::QueueElement* dequeuedEvent;
			int                                     eventId;
			while (isActive) {
				eventId = el.reserveEventForDispatching(dequeuedEvent);
				consumeAnswerlessEvent(threadId, el.answerlessConsumerProcedureReference, consumerThis,     dequeuedEvent->eventParameter);
				notifyEventObservers  (threadId, el.listenerProcedureReferences,          el.listenersThis, dequeuedEvent->eventParameter);
				el.releaseEvent(eventId);
			}
		}

		// if (zeroCopy && notifyEvents && !consumeAnswerlessEvents &&  consumeAnswerfullEvents)
		void dispatchZeroCopyListeneableAndConsumableAnswerfullEventsLoop(int threadId, void* consumerThis) {
			typename _QueueEventLink::QueueElement* dequeuedEvent;
			int                                     eventId;
			while (isActive) {
				eventId = el.reserveEventForDispatching(dequeuedEvent);
				consumeAnswerfullEvent(threadId, el.answerfullConsumerProcedureReference, consumerThis,     dequeuedEvent);
				notifyEventObservers  (threadId, el.listenerProcedureReferences,          el.listenersThis, dequeuedEvent->eventParameter);
				el.releaseEvent(eventId);
			}
		}

		// if ( zeroCopy && !notifyEvents &&  consumeAnswerlessEvents && !consumeAnswerfullEvents)
		void dispatchZeroCopyConsumableAnswerlessEventsLoop(int threadId, void* consumerThis) {
			typename _QueueEventLink::QueueElement* dequeuedEvent;
			int                                     eventId;
			while (isActive) {
				eventId = el.reserveEventForDispatching(dequeuedEvent);
				consumeAnswerlessEvent(threadId, el.answerlessConsumerProcedureReference, consumerThis, dequeuedEvent->eventParameter);
				el.releaseEvent(eventId);
			}
		}

		// if ( zeroCopy && !notifyEvents && !consumeAnswerlessEvents &&  consumeAnswerfullEvents )
		void dispatchZeroCopyConsumableAnswerfullEventsLoop(int threadId, void* consumerThis) {
			typename _QueueEventLink::QueueElement* dequeuedEvent;
			int                                     eventId;
			while (isActive) {
				eventId = el.reserveEventForDispatching(dequeuedEvent);
				consumeAnswerfullEvent(threadId, el.answerfullConsumerProcedureReference, consumerThis, dequeuedEvent);
				el.releaseEvent(eventId);
			}
		}

		// if ( zeroCopy &&  notifyEvents && !consumeAnswerlessEvents && !consumeAnswerfullEvents )
		void dispatchZeroCopyListeneableEventsLoop(int threadId) {
			typename _QueueEventLink::QueueElement* dequeuedEvent;
			int                                     eventId;
			while (isActive) {
				eventId = el.reserveEventForDispatching(dequeuedEvent);
				notifyEventObservers(threadId, el.listenerProcedureReferences, el.listenersThis, dequeuedEvent->eventParameter);
				el.releaseEvent(eventId);
			}
		}


		void debugTracker() {
			bool isReservationGuardLocked;
			bool isFull;
			bool isQueueGuardLocked;
			bool isDequeueGuardLocked;
			bool isEmpty;
			while (isActive) {

				isReservationGuardLocked = !el.reservationGuard.try_lock();
				isFull                   = el.isFull;
				isQueueGuardLocked       = !el.queueGuard.try_lock();
				isDequeueGuardLocked     = !el.dequeueGuard.try_lock();
				isEmpty                  = el.isEmpty;

				if (!isReservationGuardLocked) el.reservationGuard.unlock();
				if (!isQueueGuardLocked)       el.queueGuard.unlock();
				if (!isDequeueGuardLocked)     el.dequeueGuard.unlock();

				cerr << "\nQueueEventDispatcher('" << el.eventName << "'): rHead=" << el.queueReservedHead << "; rTail=" << el.queueReservedTail << "; reservedLength: " << el.getQueueReservedLength() << " | qHead=" << el.queueHead << "; qTail=" << el.queueTail << "; queueLength: " << el.getQueueLength() << " | isReservationGuardLocked=" << isReservationGuardLocked << "; isFull=" << isFull << "; isQueueGuardLocked=" << isQueueGuardLocked << "; isDequeueGuardLocked=" << isDequeueGuardLocked << "; isEmpty=" << isEmpty << endl << flush;
				this_thread::sleep_for(chrono::milliseconds(1000));
			}
		}
	};


}
#endif /* MUTUA_EVENTS_QUEUEEVENTDISPATCHER_H_ */
