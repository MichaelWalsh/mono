/*
 * mono-wsq.c: work-stealing queue
 *
 * Authors:
 *   Gonzalo Paniagua Javier (gonzalo@novell.com)
 *
 * Copyright (c) 2010 Novell, Inc (http://www.novell.com)
 */

#include <string.h>
#include <mono/metadata/object.h>
#include <mono/metadata/mono-wsq.h>
#include <mono/utils/mono-semaphore.h>

#define INITIAL_LENGTH	32
#define WSQ_DEBUG(...)
//#define WSQ_DEBUG(...) g_message(__VA_ARGS__)

struct _MonoWSQ {
	volatile gint head;
	volatile gint tail;
	MonoArray *queue;
	gint32 mask;
	MonoSemType lock;
};

static guint32 wsq_tlskey = -1;

void
mono_wsq_init ()
{
	wsq_tlskey = TlsAlloc ();
}

void
mono_wsq_cleanup ()
{
	if (wsq_tlskey == -1)
		return;
	TlsFree (wsq_tlskey);
	wsq_tlskey = -1;
}

MonoWSQ *
mono_wsq_create ()
{
	MonoWSQ *wsq;
	MonoDomain *root;

	if (wsq_tlskey == -1)
		return NULL;

	wsq = g_new0 (MonoWSQ, 1);
	wsq->mask = INITIAL_LENGTH - 1;
	MONO_GC_REGISTER_ROOT (wsq->queue);
	root = mono_get_root_domain ();
	wsq->queue = mono_array_new_cached (root, mono_defaults.object_class, INITIAL_LENGTH);
	MONO_SEM_INIT (&wsq->lock, 1);
	TlsSetValue (wsq_tlskey, wsq);
	return wsq;
}

void
mono_wsq_destroy (MonoWSQ *wsq)
{
	if (wsq == NULL || wsq->queue == NULL)
		return;

	g_assert (mono_wsq_count (wsq) == 0);
	MONO_GC_UNREGISTER_ROOT (wsq->queue);
	MONO_SEM_DESTROY (&wsq->lock);
	if (wsq_tlskey != -1 && TlsGetValue (wsq_tlskey) == wsq)
		TlsSetValue (wsq_tlskey, NULL);
	memset (wsq, 0, sizeof (MonoWSQ));
	g_free (wsq);
}

gint
mono_wsq_count (MonoWSQ *wsq)
{
	if (!wsq)
		return 0;
	return ((wsq->tail - wsq->head) & wsq->mask);
}

gboolean
mono_wsq_local_push (void *obj)
{
	int tail;
	int head;
	int count;
	MonoWSQ *wsq;

	if (obj == NULL || wsq_tlskey == -1)
		return FALSE;

	wsq = (MonoWSQ *) TlsGetValue (wsq_tlskey);
	if (wsq == NULL) {
		WSQ_DEBUG ("local_push: no wsq\n");
		return FALSE;
	}

	tail = wsq->tail;
	if (tail < wsq->head + wsq->mask) {
		mono_array_setref (wsq->queue, tail & wsq->mask, (MonoObject *) obj);
		wsq->tail = tail + 1;
		WSQ_DEBUG ("local_push: OK %p %p\n", wsq, obj);
		return TRUE;
	}

	MONO_SEM_WAIT (&wsq->lock);
	head = wsq->head;
	count = wsq->tail - wsq->head;
	if (count >= wsq->mask) {
		MonoArray *new_array;
		int length;
		int i;

		length = mono_array_length (wsq->queue);
		new_array = mono_array_new_cached (mono_get_root_domain (), mono_defaults.object_class, length * 2);
		for (i = 0; i < length; i++)
			mono_array_setref (new_array, i, mono_array_get (wsq->queue, MonoObject*, (i + head) & wsq->mask));

		memset (mono_array_addr (wsq->queue, MonoObject *, 0), 0, sizeof (MonoObject*) * length);
		wsq->queue = new_array;
		wsq->head = 0;
		wsq->tail = tail = count;
		wsq->mask = (wsq->mask << 1) | 1;
	}
	mono_array_setref (wsq->queue, tail & wsq->mask, obj);
	wsq->tail = tail + 1;
	MONO_SEM_POST (&wsq->lock);
	WSQ_DEBUG ("local_push: LOCK %p  %p\n", wsq, obj);
	return TRUE;
}

gboolean
mono_wsq_local_pop (void **ptr)
{
	int tail;
	gboolean res;
	MonoWSQ *wsq;

	if (ptr == NULL || wsq_tlskey == -1)
		return FALSE;

	wsq = (MonoWSQ *) TlsGetValue (wsq_tlskey);
	if (wsq == NULL) {
		WSQ_DEBUG ("local_pop: no wsq\n");
		return FALSE;
	}

	tail = wsq->tail;
	if (wsq->head >= tail) {
		WSQ_DEBUG ("local_pop: empty\n");
		return FALSE;
	}
	tail--;
	InterlockedExchange (&wsq->tail, tail);
	if (wsq->head <= tail) {
		*ptr = mono_array_get (wsq->queue, void *, tail & wsq->mask);
		mono_array_set (wsq->queue, void *, tail & wsq->mask, NULL);
		WSQ_DEBUG ("local_pop: GOT ONE %p %p\n", wsq, *ptr);
		return TRUE;
	}

	MONO_SEM_WAIT (&wsq->lock);
	if (wsq->head <= tail) {
		*ptr = mono_array_get (wsq->queue, void *, tail & wsq->mask);
		mono_array_set (wsq->queue, void *, tail & wsq->mask, NULL);
		res = TRUE;
	} else {
		wsq->tail = tail + 1;
		res = FALSE;
	}
	MONO_SEM_POST (&wsq->lock);
	WSQ_DEBUG ("local_pop: LOCK %d %p %p\n", res, wsq, *ptr);
	return res;
}

void
mono_wsq_try_steal (MonoWSQ *wsq, void **ptr, guint32 ms_timeout)
{
	if (wsq == NULL || ptr == NULL || *ptr != NULL || wsq_tlskey == -1)
		return;

	if (TlsGetValue (wsq_tlskey) == wsq)
		return;

	if (MONO_SEM_TIMEDWAIT (&wsq->lock, ms_timeout)) {
		int head;

		head = wsq->head;
		InterlockedExchange (&wsq->head, head + 1);
		if (head < wsq->tail) {
			*ptr = mono_array_get (wsq->queue, void *, head & wsq->mask);
			mono_array_set (wsq->queue, void *, head & wsq->mask, NULL);
			WSQ_DEBUG ("STEAL %p %p\n", wsq, *ptr);
		} else {
			wsq->head = head;
		}
		MONO_SEM_POST (&wsq->lock);
	}
}

