# Spending and granting the player's currency

<!-- diataxis: how-to -->

A code mod can charge the player for something it sells in its own screen (a kit upgrade, a reroll, a pull) or pay a
reward. It takes from or adds to the medals, requisition, samples or Forge Credits the game shows. The calls are SDK 21
(`spend_currency`, `grant_currency`, `currency_balance`); the types are in `hellforge_currency.h`, which
`hellforge_mod.h` includes.

To sell fixed items in the Acquisition Center instead, with no code, see [Warbonds](warbonds.md).

## Contents

- [A first spend](#a-first-spend)
- [Granting](#granting)
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

## Granting

`grant_currency` takes the same arguments and adds instead:

```c
hf->grant_currency(mod, HF_CURRENCY_MEDALS, 5, "bounty: 10 Chargers", Paid, NULL);
```

The callback gets `HF_SPEND_GRANTED` and the new balance. At a currency's cap the backend adds only what fits, so the
balance may rise by less than the amount.

## What happens

1. `spend_currency` or `grant_currency` checks the request and returns its number, or 0 when it refuses it at once.
2. A HellForge thread asks the session's backend, which changes the saved profile: a spend only if the balance covers
   it.
3. At the next game update, HellForge changes the game's copy the same way, so the HUD and the terminal show the new
   balance at once.
4. Your callback runs on the game thread with the answer and the new balance.

The backend decides, so a change counts only once it answers. Save what the player bought in your own files:
HellForge keeps the balance, not what your mod sold.

## Currencies

| Constant | Currency |
| --- | --- |
| `HF_CURRENCY_MEDALS` | medals |
| `HF_CURRENCY_REQUISITION` | requisition |
| `HF_CURRENCY_COMMON_SAMPLES` | common samples |
| `HF_CURRENCY_RARE_SAMPLES` | rare samples |
| `HF_CURRENCY_SUPER_SAMPLES` | super samples |
| `HF_CURRENCY_FORGE_CREDITS` | Forge Credits |

Super Credits are not on the list: nothing in HellForge spends or grants them. Forge Credits buy only premium tokens
the player already owns on the live game, so a grant never reaches paid content.

## Answers

| Answer | Meaning | `balance` |
| --- | --- | --- |
| `HF_SPEND_SPENT` | taken from the profile and the game | what is left |
| `HF_SPEND_TOO_LITTLE` | nothing taken: the player has less | what the player has |
| `HF_SPEND_GRANTED` | added to the profile and the game | the new balance |
| `HF_SPEND_FAILED` | nothing changed that HellForge knows of: the backend did not answer or refused the request | -1 |

The mod's log has a line for every answer, with the reason you gave.

## Limits

- Only the player on this PC, and only on the ship: a spend or grant in a mission is refused.
- 1 to `HF_SPEND_MAX` (1,000,000) a call, and a reason of 1 to 128 bytes.
- At most 64 changes wait for the backend at once. They are sent one at a time, in the order asked.
- `currency_balance` reads the game's copy. It answers 1 until the game has loaded the player's inventory.
- A session with no backend, such as a test of the loader alone, refuses every change.
