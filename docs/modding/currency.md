# Spending the player's currency

<!-- diataxis: how-to -->

A code mod can charge the player for something it sells in its own screen: a kit upgrade, a reroll, a pull. It takes
medals, requisition, samples or Forge Credits from the balances the game shows. The calls are SDK 21
(`spend_currency`, `currency_balance`); the types are in `hellforge_currency.h`, which `hellforge_mod.h` includes.

To sell fixed items in the Acquisition Center instead, with no code, see [Warbonds](warbonds.md).

## Contents

- [A first spend](#a-first-spend)
- [What happens](#what-happens)
- [Currencies](#currencies)
- [Answers](#answers)
- [Limits](#limits)

## A first spend

Ask for the spend, then give the item when the answer says it was spent:

```c
static void Paid(HfMod* mod, uint32_t request, uint32_t result, int64_t balance, void* user)
{
    if (result == HF_SPEND_SPENT) UnlockLevel(2);          /* your own state: the player paid */
    else if (result == HF_SPEND_TOO_LITTLE) ShowShort(balance); /* nothing was taken */
}

/* from a click in your shop (ui_on), on the ship */
if (hf->spend_currency(mod, HF_CURRENCY_REQUISITION, 5000, "Lenna kit level 2", Paid, NULL) == 0)
    hf->log(mod, "the spend was refused: %s", hf->last_log(mod));
```

The manifest needs `"sdk": 21`. Check `hf->size` before calling, so the mod still loads on an older HellForge.

To show the price against what the player has, read the balance:

```c
int64_t requisition;
if (hf->currency_balance(mod, HF_CURRENCY_REQUISITION, &requisition) == 0)
    hf->ui_set(mod, shop, "Balance", "Text", FormatAmount(requisition));
```

## What happens

1. `spend_currency` checks the request and returns its number, or 0 when it refuses it at once.
2. A HellForge thread asks the session's backend, which takes the amount from the saved profile if the balance covers
   it.
3. At the next game update, HellForge takes the same amount from the game's copy, so the HUD and the terminal show the
   new balance at once.
4. Your callback runs on the game thread with the answer and the balance left.

The backend decides, so a spend counts only once it answers `HF_SPEND_SPENT`. Save what the player bought in your own
files: HellForge keeps the balance, not what your mod sold.

## Currencies

| Constant | Currency |
| --- | --- |
| `HF_CURRENCY_MEDALS` | medals |
| `HF_CURRENCY_REQUISITION` | requisition |
| `HF_CURRENCY_COMMON_SAMPLES` | common samples |
| `HF_CURRENCY_RARE_SAMPLES` | rare samples |
| `HF_CURRENCY_SUPER_SAMPLES` | super samples |
| `HF_CURRENCY_FORGE_CREDITS` | Forge Credits |

Super Credits are not on the list: nothing in HellForge spends or grants them.

## Answers

| Answer | Meaning | `balance` |
| --- | --- | --- |
| `HF_SPEND_SPENT` | taken from the profile and the game | what is left |
| `HF_SPEND_TOO_LITTLE` | nothing taken: the player has less | what the player has |
| `HF_SPEND_FAILED` | nothing taken that HellForge knows of: the backend did not answer or refused the request | -1 |

The mod's log has a line for every answer, with the reason you gave.

## Limits

- Only the player on this PC, and only on the ship: a spend in a mission is refused.
- 1 to `HF_SPEND_MAX` (1,000,000) a spend, and a reason of 1 to 128 bytes.
- At most 64 spends wait for the backend at once. They are sent one at a time, in the order asked.
- A mod can take currency, never add it.
- `currency_balance` reads the game's copy. It answers 1 until the game has loaded the player's inventory.
- A session with no backend, such as a test of the loader alone, refuses every spend.
