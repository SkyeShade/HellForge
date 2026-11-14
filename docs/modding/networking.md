# The game's network messages

<!-- diataxis: how-to -->
The game's machines talk in RPCs: named messages with typed arguments, such as `rpc_pickup_entity` or
`rpc_from_host_respawn`. Build 25480438 has 592 of them. A code mod can listen to any of them as this machine
receives them, stop one, and send them itself (SDK 17). Your mod's own messages to its copies on other machines go
through the [mod channel](concepts.md#co-op) instead.

The [RPC reference](../rpcs.md) lists every RPC with its arguments.

## Listening

Include `hellforge_rpcs.h` after `hellforge_mod.h`. It is large (every RPC of the game), so `hellforge_mod.h` does not
include it for you. Each RPC has an id (`HF_RPC_<NAME>`), a struct of its arguments (`HfRpc_<name>`), a reader and a
sender:

```c
#include "hellforge_mod.h"
#include "hellforge_rpcs.h"

static int OnPickup(HfMod* mod, const HfRpcEvent* event, void* user)
{
    HfRpc_pickup_entity pickup;
    if (hf_rpc_pickup_entity_read(event, &pickup) == 0)
        HF_API(mod)->log(mod, "peer %llx picks up entity %d", (unsigned long long)event->sender, pickup.entity);
    return HF_RPC_KEEP;
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* hf, HfMod* mod)
{
    if (hf->size < offsetof(HfApi, rpc_send) + sizeof(void*)) return 0;   /* an older HellForge */
    hf->rpc_listen(mod, HF_RPC_PICKUP_ENTITY, OnPickup, NULL, 0);
    return 0;
}
```

- Listeners run on the game thread, as the message arrives and before the game's own code for it. The highest
  priority runs first; equal priorities run in the order they listened.
- `HF_RPC_ALL` listens to every RPC. Use it for a log or a filter, and keep it cheap: it runs for every message.
- The event's arguments, and any string in them, are valid during the listener's call only. Copy what you keep.
- A reader returns 1 when the event is another RPC or its arguments are not the declared ones (a newer game or a broken
  sender). Nothing is filled then.

## Dropping a message

A listener that returns `HF_RPC_DROP` stops the message: later listeners and the game's own code never see it. The
other machines still did whatever they did, so a dropped message can leave this machine out of step with them. Drop only
what you understand, and log what you drop.

## Sending

`hf_rpc_<name>_send(mod, to, &args)` builds the arguments and calls `rpc_send`. `to` is `HF_TO_ALL` (every other machine
of the session), `HF_TO_HOST`, or a peer (an event's `sender`).

HellForge checks every send before the game sees it, because the game itself does not:

- the RPC must exist in this build (the game crashes on an unknown one);
- every declared argument must be there, of its declared kind, and no more (the game reads past a short list);
- an array holds at most its declared number of elements, a string at most 2047 bytes;
- the whole message must fit the game's 1200-byte message buffer (the game would send it cut short).

A send is refused (1, with the reason in the mod's log) on another thread, in solo, or to `HF_TO_HOST` on the host.
Values travel in each argument's declared width and range (the reference lists them): an int outside its range wraps and
a float is clamped to it.

Sending an RPC makes the other machines run the game's own code for it, as if the game had sent it. Their game trusts
it the same way. Use the RPCs the game itself sends in that situation, with the values it would use.

## What arguments look like

`HfRpcArg` is an argument as the game passes it: its kind (`HF_RPC_KIND_...`), its size and where its value is. An array
is one `HF_RPC_KIND_ARRAY` entry, whose value is the element count instead of a pointer, followed by that many element
entries. The generated readers and senders handle all of this. `rpc_listen` and `rpc_send` also work with the raw list,
for an RPC you decode yourself.

Arguments are named after their network type where that name is known (`entity`, `peer_id`, `position`), else `arg0`,
`arg1`, and so on. A second argument of the same type gets `_2`.

## How it works

For contributors. On build 25480438 every RPC a machine receives goes through one game function, the RPC dispatch
(`game.dll` 0xBC2D10), on the game thread inside the frame's network receive. It hands the receiver the sender's peer
id and the decoded arguments. HellForge hooks it once a mod listens and runs the listeners before it.

Sending goes through the game's RPC send (0xBDE430), which looks the RPC up by its hash and encodes each declared
argument. It trusts its caller, which is why `rpc_send` checks every message against `native/mod-loader/sdk_rpcs.h`
first. Both functions' first bytes are checked before HellForge uses them, and nothing happens on another build.

`scripts/ExportRpcs.py` writes `mappings/rpcs.json` from the game's network configuration, and `hellforge sdk-headers`
generates the headers and the [reference](../rpcs.md) from it.
