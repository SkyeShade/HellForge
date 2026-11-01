/* Game events (SDK 7, subscribe): what happens in the game, delivered to mods on the game thread in priority order.
 * Included by hellforge_modloader.c after packages.c (the game thread's update delivers them).
 *
 *   listeners  per kind, highest priority first, then in subscription order. A listener sees the event's data during
 *              its call only.
 *   delivery   always on the game thread: events that sources raise there go out at once; others wait in a queue the
 *              next game update empties (EventsTick), in the order they were raised.
 *   sources    event_sources.c; each is installed only once its code or data reads as reviewed for build 25480438,
 *              and only when a mod listens to its kind (nothing is hooked for kinds no mod uses). */

#define EVT_MAX_LISTENERS 128
#define EVT_QUEUE 256
#define EVT_KINDS 32
#define EVT_DATA 128 /* the largest event's size */

typedef struct Listener { HfMod* mod; uint32_t kind; HfListener listener; void* user; int32_t priority; uint32_t order; } Listener;
static Listener g_listeners[EVT_MAX_LISTENERS];
static int g_listenerCount;
static uint32_t g_listenerOrder;
static uint64_t g_updates;                    /* game updates since the loader started (HfEventInfo.update) */
static DWORD g_gameThread;                    /* the game thread's id, once its update has run */
static SRWLOCK g_eventsLock = SRWLOCK_INIT;
typedef struct QueuedEvent { uint8_t data[EVT_DATA]; } QueuedEvent;
static QueuedEvent g_queue[EVT_QUEUE];
static int g_queued;
static uint32_t g_queueDropped;
static volatile LONG g_listening;            /* bit k: a mod listens to kind k (read on the hooks' hot paths) */

static const char* EventName(uint32_t kind)
{
    static const char* names[] = {"?", "state", "mission start", "mission end", "player spawn", "player down", "damage", "stratagem",
        "kill", "spawn", "heal", "stat", "weapon", "explosion", "bug hole", "damage (before)"};
    return kind < sizeof names / sizeof names[0] ? names[kind] : "?";
}

/* Every listener of the event's kind, in order; on the game thread. */
static void Dispatch(HfEventInfo* event)
{
    event->update = g_updates;
    Listener copy[EVT_MAX_LISTENERS];
    int n = 0;
    AcquireSRWLockShared(&g_eventsLock);
    for (int i = 0; i < g_listenerCount; i++) if (g_listeners[i].kind == event->kind && g_listeners[i].mod->loaded) copy[n++] = g_listeners[i];
    ReleaseSRWLockShared(&g_eventsLock);
    for (int i = 0; i < n; i++) copy[i].listener(copy[i].mod, event, copy[i].user);
}

/* Raises an event: delivered now on the game thread, else queued for its next update. */
static void Raise(HfEventInfo* event)
{
    if (event->size > EVT_DATA) return;
    if (g_gameThread != 0 && GetCurrentThreadId() == g_gameThread) { Dispatch(event); return; }
    AcquireSRWLockExclusive(&g_eventsLock);
    if (g_queued < EVT_QUEUE) memcpy(g_queue[g_queued++].data, event, event->size);
    else g_queueDropped++;
    ReleaseSRWLockExclusive(&g_eventsLock);
}

static int AnyListener(uint32_t kind) { return (g_listening >> kind) & 1; }

static void EventSourcesTick(void); /* EVENT SOURCES */
static void ActionsTick(void);      /* actions.c */

/* The game thread's update (TickDetour): the queue, then the polled sources. */
static void EventsTick(void)
{
    g_gameThread = GetCurrentThreadId();
    g_updates++;
    if (g_queued)
    {
        static QueuedEvent pending[EVT_QUEUE];
        AcquireSRWLockExclusive(&g_eventsLock);
        int n = g_queued;
        memcpy(pending, g_queue, sizeof pending[0] * n);
        g_queued = 0;
        uint32_t dropped = g_queueDropped;
        g_queueDropped = 0;
        ReleaseSRWLockExclusive(&g_eventsLock);
        if (dropped) Note("events: %u event(s) dropped (the queue was full)", dropped);
        for (int i = 0; i < n; i++) Dispatch((HfEventInfo*)pending[i].data);
    }
    EventSourcesTick();
    ActionsTick();
}

/* SDK 7: a listener for one kind of event. */
static int ApiSubscribe(HfMod* mod, uint32_t kind, HfListener listener, void* user, int32_t priority)
{
    if (mod == NULL || listener == NULL || ModGone(mod)) return 0;
    if (kind == 0 || kind >= EVT_KINDS || EventName(kind)[0] == '?') { ApiLog(mod, "subscribe: no event kind %u", kind); return 0; }
    if (g_tickState < 0) { ApiLog(mod, "subscribe(%s): the game thread's update is not hooked (modloader.log says why)", EventName(kind)); return 0; }
    AcquireSRWLockExclusive(&g_eventsLock);
    int ok = g_listenerCount < EVT_MAX_LISTENERS;
    if (ok)
    {
        int at = g_listenerCount;
        while (at > 0 && g_listeners[at - 1].priority < priority) { g_listeners[at] = g_listeners[at - 1]; at--; }
        g_listeners[at] = (Listener){mod, kind, listener, user, priority, g_listenerOrder++};
        g_listenerCount++;
        InterlockedOr(&g_listening, 1L << kind);
    }
    ReleaseSRWLockExclusive(&g_eventsLock);
    if (!ok) ApiLog(mod, "subscribe(%s): mods hold %d listeners already", EventName(kind), EVT_MAX_LISTENERS);
    else Note("events: %s listens to %s (priority %d)", mod->id, EventName(kind), priority);
    return ok;
}

#include "event_sources.c"
