/* HellForge mod SDK: co-op messages as a mod sees them. The mod channel's caps, targets and events (SDK 16: send,
 * share_set, HF_ON_MESSAGE, HF_ON_SHARE) and the game's network messages, its RPCs (SDK 17: their arguments, a received
 * message and a listener for rpc_listen and rpc_send). Included by hellforge_mod.h, after HfEventInfo; hellforge_rpcs.h,
 * which mods include themselves, names every RPC of the game build and gives each a struct, a reader and a sender. */
#ifndef HELLFORGE_NET_H
#define HELLFORGE_NET_H

/* SDK 16: the mod channel's caps and targets (send, share_set). A message holds at most HF_MESSAGE_MAX bytes; a shared
 * row's value at most HF_SHARE_VALUE_MAX bytes; a mod's table at most HF_SHARE_ROWS rows. */
#define HF_MESSAGE_MAX 1024u
#define HF_SHARE_VALUE_MAX 64u
#define HF_SHARE_ROWS 512u
/* send's to: every other machine of the lobby, or its host. A peer id from HfMessageEvent.from names one machine. */
#define HF_TO_ALL 0xFFFFFFFFFFFFFFFFull
#define HF_TO_HOST 0xFFFFFFFFFFFFFFFEull

/* SDK 16: a message from the same mod on another machine of the lobby (send, HF_ON_MESSAGE). `from` is the sender's peer
 * id (the host's when it came from the host); data holds `bytes` bytes, at most HF_MESSAGE_MAX. Valid during the call. */
typedef struct HfMessageEvent
{
    HfEventInfo info;
    uint64_t from;
    uint32_t bytes;
    uint8_t data[HF_MESSAGE_MAX];
} HfMessageEvent;
/* SDK 16: one row of this mod's shared table (share_set, share_get, share_row; HF_ON_SHARE), raised on the game thread
 * when a row arrives, changes or goes (bytes 0: removed). Valid during the call. */
typedef struct HfShareEvent
{
    HfEventInfo info;
    uint64_t key;
    uint32_t bytes;
    uint8_t value[HF_SHARE_VALUE_MAX];
} HfShareEvent;

/* SDK 17: the game's own RPCs, its network messages (rpc_listen, rpc_send; hellforge_rpcs.h names every one and gives
 * it a typed struct, reader and sender; docs/modding/networking.md). An argument as the game passes it: its kind, its
 * size (a string's length in bytes; 0 for an array) and where its value is. An array is one HF_RPC_KIND_ARRAY entry,
 * whose value is not a pointer but the element count, followed by that many element entries. Values: bool a uint8_t
 * 0 or 1, int an int32_t, float a float, vector3 and direction 3 floats, vector2 2, quaternion 4 (x, y, z, w), string a
 * NUL-terminated char array, u32 a uint32_t, u64 a uint64_t (an id, or a peer), id64 a uint64_t name hash (an
 * id_string64: a resource's or an item's name, MurmurHash64A). */
#define HF_RPC_KIND_BOOL 0u
#define HF_RPC_KIND_INT 1u
#define HF_RPC_KIND_FLOAT 2u
#define HF_RPC_KIND_VECTOR3 3u
#define HF_RPC_KIND_QUATERNION 4u
#define HF_RPC_KIND_STRING 5u
#define HF_RPC_KIND_U32 6u
#define HF_RPC_KIND_ID64 7u
#define HF_RPC_KIND_U64 9u
#define HF_RPC_KIND_ARRAY 10u
#define HF_RPC_KIND_DIRECTION 11u
#define HF_RPC_KIND_VECTOR2 12u
typedef struct HfRpcArg
{
    uint32_t kind;
    uint32_t size;
    const void* value;
} HfRpcArg;
/* One RPC this machine received, on the game thread before the game's own receiver runs. sender: the peer that sent it;
 * rpc: its id (HF_RPC_...); args: count entries, valid during the listener's call only. */
typedef struct HfRpcEvent
{
    uint64_t sender;
    uint32_t rpc;
    uint32_t count;
    const HfRpcArg* args;
} HfRpcEvent;
/* What an RPC listener returns: HF_RPC_KEEP lets the message go on (later listeners, then the game's receiver);
 * HF_RPC_DROP stops it here, as if it never arrived. */
#define HF_RPC_KEEP 0
#define HF_RPC_DROP 1
/* rpc_listen's rpc for every RPC (a log or a filter of all messages). */
#define HF_RPC_ALL 0u
typedef int (*HfRpcListener)(HfMod* mod, const HfRpcEvent* event, void* user);

#endif
