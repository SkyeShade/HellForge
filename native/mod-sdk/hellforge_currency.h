/* HellForge mod SDK 21: the player's currency (spend_currency, currency_balance; docs/modding/currency.md).
 *
 * A mod's own shop: it takes what the player pays from the same balances the game shows. The session's backend holds the
 * saved balance, so every spend goes through it first and the game's copy follows; a mod can take currency, never add
 * it. Included by hellforge_ui.h, so hellforge_mod.h has it. */
#pragma once
#include <stdint.h>

/* The currencies a mod may spend: the ones a mod's warbond may price in (their catalogue ids). */
#define HF_CURRENCY_MEDALS 0x4C19589Fu
#define HF_CURRENCY_REQUISITION 0xD7150EECu
#define HF_CURRENCY_COMMON_SAMPLES 0xEDF6EAF5u
#define HF_CURRENCY_RARE_SAMPLES 0xB1ED1C41u
#define HF_CURRENCY_SUPER_SAMPLES 0xDAC0E9DBu
#define HF_CURRENCY_FORGE_CREDITS 0x6C3C83C1u

#define HF_SPEND_MAX 1000000 /* the most one spend takes */

/* A spend's answer. */
#define HF_SPEND_SPENT 1u      /* taken; balance is what is left */
#define HF_SPEND_TOO_LITTLE 2u /* nothing taken: the player has balance, less than the amount */
#define HF_SPEND_FAILED 3u     /* nothing taken: the backend did not answer or refused (the mod's log says why) */

/* A spend's answer, on the game thread: the request spend_currency returned, HF_SPEND_..., and the balance after it
 * (-1 when HF_SPEND_FAILED). */
typedef void (*HfSpendCallback)(struct HfMod* mod, uint32_t request, uint32_t result, int64_t balance, void* user);
