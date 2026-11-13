/* hellforge_rpcs.h compiles, as C and as C++ (Build-Engine.ps1 builds this file both ways): every generated struct,
 * reader and sender, used here through two of them. Never loaded. */
#include "hellforge_mod.h"
#include "hellforge_rpcs.h"

static int OnPickup(HfMod* mod, const HfRpcEvent* event, void* user)
{
    (void)user;
    HfRpc_pickup_entity pickup;
    if (hf_rpc_pickup_entity_read(event, &pickup) != 0) return HF_RPC_KEEP;
    return hf_rpc_pickup_entity_send(mod, HF_TO_ALL, &pickup) == 0 ? HF_RPC_KEEP : HF_RPC_DROP;
}

#ifdef __cplusplus
extern "C"
#endif
HF_MOD_EXPORT int hellforge_mod_init(const HfApi* hf, HfMod* mod)
{
    if (hf->size < offsetof(HfApi, rpc_send) + sizeof(void*)) return 0;
    return hf->rpc_listen(mod, HF_RPC_PICKUP_ENTITY, OnPickup, NULL, 0);
}
