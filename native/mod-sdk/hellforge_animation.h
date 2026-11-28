/* HellForge mod SDK: animation events as a mod sees them (HF_ON_ANIMATION, hf_on_animation). Included by
 * hellforge_mod.h, after the typed events' helpers. The calls that send an entity's or a unit's state machine an event
 * are game functions by name (hellforge_functions.h: TriggerAnimationEvent, UnitAnimationHasEvent and the rest). */
#ifndef HELLFORGE_ANIMATION_H
#define HELLFORGE_ANIMATION_H

/* Known by its kind alone: a HellForge without it refuses the subscription, and the mod's log says so. */
#define HF_ON_ANIMATION 18 /* HfAnimationEvent: the game sent an entity's animation state machine an event */

/* An event the game sent an entity's animation state machine: every call of TriggerAnimationEvent, the way most of the
 * game's code fires them, told after the call. It is the request: an entity without an animation record, or whose state
 * machine has no such event, is told too (UnitAnimationHasEvent says whether it has). Not told: events sent with a start
 * or a speed (UnitAnimationEventArgs and its callers: the Helldiver's reloads and draws, AbilityAnimationEvent's) and
 * events sent to a unit directly (a few of the Helldiver's aim events, abilities' events). A hook on those functions by
 * name sees them. */
typedef struct HfAnimationEvent
{
    HfEventInfo info;
    uint32_t entity;    /* the entity */
    uint32_t event;     /* the event's name hash, as the state machine stores it */
    int32_t mode;       /* 0 this machine only; 1 or 2: also sent to the other machines (rpc_animation_event) */
    uint32_t behaviour; /* 1: also handed to the entity's behaviour */
} HfAnimationEvent;
HF_ASSERT_OFFSET(HfAnimationEvent, info, 0);

typedef void (*HfAnimationListener)(HfMod* mod, const HfAnimationEvent* event, void* user);
/* HF_ON_ANIMATION: the game sent an entity's animation state machine an event (TriggerAnimationEvent). */
static inline int hf_on_animation(HfMod* mod, HfAnimationListener listener, void* user, int32_t priority)
{ return hf_subscribe_(mod, HF_ON_ANIMATION, HF_LISTENER_(listener), user, priority); }

#endif
