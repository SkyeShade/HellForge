/* Screens in the game's own UI (SDK 19, ui_*; docs/modding/ui.md): NoesisGUI 3.2.5, linked into game.dll (25480438;
 * research\ui-api). A screen is the mod's XAML, parsed by the game (ParseXaml), so the game's styles, fonts and themed
 * controls are there. An overlay screen is a child of ui::Root's Containers grid (on the ship and in missions); a named
 * element's property is set from a string through the property's own TypeConverter; a Button's clicks reach the mod
 * through a DelegateCommand of the game's whose Execute delegate is ours ({game delegate impl, entry, fn}). Every call
 * is queued and run by UiTick on the game thread, the only thread that may touch Noesis. A menu screen takes over the
 * game's unused menu presenter 40 (DebugPlayground, research\ui-api\stage2.md): the game opens it as any menu (cursor,
 * the player's input held, Back closes), with our element for its view and none of the playground's own work. Included
 * by packages.c. */

#define UI_SCREENS 64
#define UI_COMMANDS 256
#define UI_OPS 1024
#define UI_MENU_STATES 0x347CE28u   /* MenuStates* */
#define UI_ROOT 0x4658u             /* MenuStates: ui::Root */
#define UI_CONTAINERS 0x2C0u        /* ui::Root: Grid "Containers" */
#define UI_PARSE 0x1C9FE70u         /* Ptr* (Ptr* out, const char* xaml) */
#define UI_FIND_NAME 0x1BA03E0u     /* BaseComponent* (FrameworkElement*, const char*) */
#define UI_CHILDREN 0x1B27080u      /* UIElementCollection* (Panel*) */
#define UI_CONVERTER 0x1C072E0u     /* TypeConverter* (const Type*); slot 8: bool TryConvertFromString(conv, text, Ptr* out) */
#define UI_SET_VALUE 0x1AB7A20u     /* (DependencyObject*, const DependencyProperty*, BaseComponent* boxed) */
#define UI_ALLOC 0x1B04BC0u         /* void* (size_t): Noesis's allocator, which its objects are freed with */
#define UI_SYMBOL 0x1AE5B60u        /* uint32_t (const char*) */
#define UI_PROPERTY 0x1B3FC30u      /* const DependencyProperty* (const TypeClass*, Symbol): walks the base types */
#define UI_COMMAND_VTBL 0x22E6EE8u  /* DelegateCommand, 0x90 bytes: refcount +8, ICommand +0x20, CanExecute +0x48, Execute +0x68 */
#define UI_ICOMMAND_VTBL 0x22E30A8u
#define UI_DELEGATE_VTBL 0x331C138u /* a one-argument delegate impl: Invoke = fn(object, argument), returning fn's value */
#define UI_MENUS 0x4288u            /* MenuStates: the menu presenters (+0xC the open one, +0x28 one opening) */
#define UI_MENU_SLOT 0x160u         /* the presenters' slots (each step's first argument): presenter 40's state */
#define UI_MENU_ID 40               /* DebugPlayground, never opened by the game */
#define UI_MENU_OPEN 0x14C0350u     /* void (menus*, int id, void* argument) */
#define UI_MENU_CREATE 0x14F6C50u   /* void (state*): presenter 40's own steps, hooked */
#define UI_MENU_ENTER 0x14F6FD0u    /* void (state*) */
#define UI_MENU_UPDATE 0x14B7240u   /* void (slots*, int id, float dt): every presenter's */
#define UI_MENU_INPUT 0x14B6C50u    /* bool (slots*, int id): the top presenter's */
#define UI_MENU_EXIT 0x14B5F20u     /* void (slots*, int id) */
#define UI_MENU_DESTROY 0x14BA2C0u  /* void (slots*, int id) */
#define UI_INPUT 0x347CF18u         /* InputManager*: +0x328 + action*0x20, the action's pressed byte */
#define UI_ACTION_BACK 9
#define UI_STATE_ALLOCATOR 0x33263B8u /* AllocatorApi*: +0x60 deallocate(heap, p), which presenter states are freed with */
#define UI_STATE_HEAP 0x3326378u

static const struct { uint32_t rva; uint8_t bytes[16]; uint8_t n; } kUiPins[] = {
    {UI_PARSE, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57}, 16},
    {UI_FIND_NAME, {0x48, 0x83, 0xec, 0x38, 0x4c, 0x8b, 0xc2, 0x48, 0x8d, 0x54, 0x24, 0x20, 0xe8}, 13},
    {UI_CHILDREN, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0x99, 0x02, 0x00, 0x00, 0x00}, 13},
    {UI_CONVERTER, {0x40, 0x53, 0x41, 0x56, 0x48, 0x81, 0xec, 0x38, 0x02, 0x00, 0x00}, 11},
    {UI_SET_VALUE, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x40}, 15},
    {UI_ALLOC, {0x4c, 0x8b, 0xdc, 0x41, 0x56, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0xf1}, 12},
    {UI_SYMBOL, {0x48, 0x89, 0x4c, 0x24, 0x08, 0x53, 0x56, 0x48, 0x83, 0xec, 0x38, 0x48}, 12},
    {UI_PROPERTY, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x20}, 16},
};
/* Presenter 40's steps and the presenter calls around them (24 bytes: exit and open begin as other functions do). */
static const struct { uint32_t rva; uint8_t bytes[24]; } kUiMenuPins[] = {
    {UI_MENU_OPEN, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x41, 0x28, 0x49, 0x8b, 0xf0, 0x83, 0xe8, 0x01}},
    {UI_MENU_CREATE, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xec, 0x40, 0x02, 0x00, 0x00, 0x48, 0x8b, 0xf1, 0xe8, 0x2a, 0xff, 0xff, 0xff, 0x48, 0x8b}},
    {UI_MENU_ENTER, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {UI_MENU_UPDATE, {0x48, 0x8b, 0xc4, 0x48, 0x81, 0xec, 0x78, 0x02, 0x00, 0x00, 0xff, 0xca, 0x0f, 0x29, 0x70, 0xe8, 0x0f, 0x28, 0xf2, 0x83, 0xfa, 0x33, 0x0f, 0x87}},
    {UI_MENU_INPUT, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0xff, 0xca, 0x4c, 0x8b, 0xd1, 0x83, 0xfa, 0x33, 0x0f}},
    {UI_MENU_EXIT, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x63, 0xda, 0x48, 0x8d, 0x35, 0xc7, 0xa0, 0xb4}},
    {UI_MENU_DESTROY, {0x40, 0x57, 0x48, 0x83, 0xec, 0x40, 0xff, 0xca, 0x48, 0x8b, 0xf9, 0x83, 0xfa, 0x33, 0x0f, 0x87, 0xed, 0x13, 0x00, 0x00, 0x48, 0x89, 0x5c, 0x24}},
};
/* The first slot of each vtable the commands are made of, as the game holds it (relative to game.dll). */
static const struct { uint32_t vtable, slot0; } kUiVtables[] = {
    {UI_COMMAND_VTBL, 0x14AF100u}, {UI_ICOMMAND_VTBL, 0x172E9F4u}, {UI_DELEGATE_VTBL, 0x1703280u}};

typedef struct UiCommand
{
    HfMod* mod;
    int screen;
    char element[64];
    HfUiCallback callback;
    void* user;
    uint8_t* command; /* the game's object; ours holds a reference */
} UiCommand;

typedef struct UiScreen
{
    HfMod* mod;
    uint32_t kind;
    int state;        /* 0 queued, 1 shown (a menu: parsed, maybe still waiting for the presenter), 2 closed, -1 refused */
    void* element;    /* the parsed root; one reference ours */
    HfUiCallback closed; /* ui_on with no element */
    void* closedUser;
} UiScreen;

enum { UI_OPEN, UI_CLOSE, UI_SET, UI_ON, UI_ON_CLOSE };
typedef struct UiOp
{
    int kind, screen;
    char* text;            /* OPEN: the XAML (malloc'd) */
    char element[64], property[64];
    char* value;           /* SET (malloc'd) */
    HfUiCallback callback;
    void* user;
} UiOp;

static UiScreen g_uiScreens[UI_SCREENS + 1]; /* by id, 1..UI_SCREENS */
static int g_uiScreenCount;
static UiCommand g_uiCommands[UI_COMMANDS];
static int g_uiCommandCount;
static UiOp g_uiOps[UI_OPS];
static int g_uiOpCount;
static SRWLOCK g_uiLock = SRWLOCK_INIT;
static int g_uiProven;      /* 0 not yet (game.dll still packed, or not loaded), 1 proven, -1 not the reviewed build */
static int g_uiProveTries;
static int g_uiMenuHooks;      /* 0 not yet, 1 in, -1 could not */
static int g_uiMenuScreen;     /* the menu screen shown or waiting for the presenter, or 0 */
static void* g_uiMenuPending;  /* the element presenter 40's create takes, while it is being opened */
static uint8_t* g_uiMenuState; /* presenter 40's state while it shows a mod's screen */
typedef void (*UiStateFn)(uint8_t*);
typedef void (*UiSlotFn)(uint8_t*, int);
typedef void (*UiUpdateFn)(uint8_t*, int, float);
typedef uint8_t (*UiInputFn)(uint8_t*, int);
static UiStateFn g_uiCreate, g_uiEnter;
static UiUpdateFn g_uiUpdate;
static UiInputFn g_uiInput;
static UiSlotFn g_uiExit, g_uiDestroy;

static uintptr_t UiGame(void) { return g_game; }

/* The pins and vtables, checked once game.dll is unpacked (it is not when mods start): 1 proven. */
static int UiProven(void)
{
    uintptr_t game = UiGame();
    if (g_uiProven || game == 0) return g_uiProven > 0;
    for (size_t i = 0; i < sizeof kUiPins / sizeof kUiPins[0]; i++)
        if (!PinMatches(game + kUiPins[i].rva, kUiPins[i].bytes, kUiPins[i].n))
        {
            if (++g_uiProveTries == 3600) { g_uiProven = -1; Note("ui: the game's Noesis functions are not the reviewed ones (game.dll+0x%x); no mod screens", kUiPins[i].rva); }
            return 0;
        }
    for (size_t i = 0; i < sizeof kUiVtables / sizeof kUiVtables[0]; i++)
        if (*(const uintptr_t*)(game + kUiVtables[i].vtable) != game + kUiVtables[i].slot0)
        { g_uiProven = -1; Note("ui: the game's command classes are not the reviewed ones; no mod screens"); return 0; }
    g_uiProven = 1;
    return 1;
}

static void UiRelease(void* object)
{
    if (object != NULL && InterlockedDecrement((volatile LONG*)((uint8_t*)object + 8)) == 0)
        ((void (*)(void*))(*(void***)object)[4])(object);
}

/* A screen gone (ui_close, or Back on a menu): its commands and element let go, the mod told. */
static void UiClosed(int screen)
{
    UiScreen* s = &g_uiScreens[screen];
    for (int i = 0; i < g_uiCommandCount; i++)
        if (g_uiCommands[i].screen == screen && g_uiCommands[i].command) { UiRelease(g_uiCommands[i].command); g_uiCommands[i].command = NULL; }
    UiRelease(s->element);
    s->element = NULL;
    s->state = 2;
    if (s->closed != NULL) s->closed(s->mod, screen, "", s->closedUser);
}

/* ui::Root's Containers grid, or NULL before the UI is up. */
static void* UiContainers(void)
{
    uint8_t* states = *(uint8_t* const*)(UiGame() + UI_MENU_STATES);
    uint8_t* root = states != NULL ? *(uint8_t* const*)(states + UI_ROOT) : NULL;
    return root != NULL ? *(void* const*)(root + UI_CONTAINERS) : NULL;
}

static void* UiFind(UiScreen* s, const char* name)
{
    return name[0] ? ((void* (*)(void*, const char*))(UiGame() + UI_FIND_NAME))(s->element, name) : s->element;
}

/* A dependency property of an element by name (its type and base types), or NULL. */
static void* UiProperty(void* element, const char* name)
{
    void* type = ((void* (*)(void*))(*(void***)element)[2])(element);
    uint32_t symbol = ((uint32_t (*)(const char*))(UiGame() + UI_SYMBOL))(name);
    return type != NULL ? ((void* (*)(void*, uint32_t))(UiGame() + UI_PROPERTY))(type, symbol) : NULL;
}

/* A property set from a string, as XAML would: why not, or NULL. */
static const char* UiSetString(void* element, const char* name, const char* text)
{
    void* property = UiProperty(element, name);
    if (property == NULL) return "it has no such property";
    void* converter = ((void* (*)(void*))(UiGame() + UI_CONVERTER))(*(void**)((uint8_t*)property + 0x20));
    if (converter == NULL) return "the property takes no text";
    void* boxed = NULL;
    if (!(((int (*)(void*, const char*, void**))(*(void***)converter)[8])(converter, text, &boxed) & 0xFF) || boxed == NULL)
        return "the text is not a value of the property";
    ((void (*)(void*, void*, void*))(UiGame() + UI_SET_VALUE))(element, property, boxed);
    UiRelease(boxed);
    return NULL;
}

/* A click (Execute) or a question (CanExecute: always yes) from a command of ours. */
static int UiExecute(UiCommand* c, void* parameter)
{
    (void)parameter;
    if (c->callback != NULL && g_uiScreens[c->screen].state == 1) c->callback(c->mod, c->screen, c->element, c->user);
    return 1;
}
static int UiCanExecute(UiCommand* c, void* parameter) { (void)c; (void)parameter; return 1; }

/* A DelegateCommand of the game's whose delegates are ours, set as the element's Command: why not, or NULL. */
static const char* UiCommandOn(UiScreen* s, void* element, UiCommand* c)
{
    void* property = UiProperty(element, "Command");
    if (property == NULL) return "it takes no Command (a Button, a MenuItem or a Hyperlink does)";
    uintptr_t game = UiGame();
    uint8_t* command = ((uint8_t* (*)(size_t))(game + UI_ALLOC))(0x90);
    if (command == NULL) return "no memory";
    memset(command, 0, 0x90);
    void* vtables[2] = {(void*)(game + UI_COMMAND_VTBL), (void*)(game + UI_ICOMMAND_VTBL)};
    memcpy(command, &vtables[0], 8);
    *(LONG*)(command + 8) = 1;
    memcpy(command + 0x20, &vtables[1], 8);
    void* canExecute[3] = {(void*)(game + UI_DELEGATE_VTBL), c, (void*)UiCanExecute};
    void* execute[3] = {(void*)(game + UI_DELEGATE_VTBL), c, (void*)UiExecute};
    memcpy(command + 0x48, canExecute, sizeof canExecute);
    memcpy(command + 0x68, execute, sizeof execute);
    c->command = command;
    ((void (*)(void*, void*, void*))(game + UI_SET_VALUE))(element, property, command);
    (void)s;
    return NULL;
}

/* Presenter 40's steps: while it shows a mod's screen (g_uiMenuState) none of the playground's runs. */
static int UiMenuOurs(uint8_t* slots, int id)
{
    return id == UI_MENU_ID && g_uiMenuState != NULL && *(uint8_t* const*)(slots + UI_MENU_SLOT) == g_uiMenuState;
}

static void UiMenuCreate(uint8_t* state)
{
    if (g_uiMenuPending == NULL) { g_uiCreate(state); return; }
    void* element = g_uiMenuPending;
    g_uiMenuPending = NULL;
    InterlockedIncrement((volatile LONG*)((uint8_t*)element + 8));
    memset(state, 0, 0x18);                   /* no view model: the game sets no DataContext */
    memcpy(state + 8, &element, 8);           /* the view, released by our destroy */
    *(uint32_t*)(state + 0x18) = 0xFFFFFFFFu; /* no thumbnail group */
    g_uiMenuState = state;
}
static void UiMenuEnter(uint8_t* state) { if (state != g_uiMenuState) g_uiEnter(state); }
static void UiMenuUpdate(uint8_t* slots, int id, float dt) { if (!UiMenuOurs(slots, id)) g_uiUpdate(slots, id, dt); }
static uint8_t UiMenuInput(uint8_t* slots, int id)
{
    if (!UiMenuOurs(slots, id)) return g_uiInput(slots, id);
    uint8_t* input = *(uint8_t* const*)(UiGame() + UI_INPUT);
    if (input == NULL || !input[0x328 + UI_ACTION_BACK * 0x20]) return 0;
    g_uiMenuState[0x10] = 1; /* closed by the game at the end of this UI frame */
    return 1;
}
static void UiMenuExit(uint8_t* slots, int id) { if (!UiMenuOurs(slots, id)) g_uiExit(slots, id); }
static void UiMenuDestroy(uint8_t* slots, int id)
{
    if (!UiMenuOurs(slots, id)) { g_uiDestroy(slots, id); return; }
    uint8_t* state = g_uiMenuState;
    void* view;
    memcpy(&view, state + 8, 8);
    UiRelease(view);
    uintptr_t game = UiGame();
    void** allocator = *(void***)(game + UI_STATE_ALLOCATOR);
    ((void (*)(void*, void*))allocator[0x60 / 8])(*(void**)(game + UI_STATE_HEAP), state);
    memset(slots + UI_MENU_SLOT, 0, 8);
    g_uiMenuState = NULL;
    int screen = g_uiMenuScreen;
    g_uiMenuScreen = 0;
    Note("ui: %s's menu screen %d closed", g_uiScreens[screen].mod->id, screen);
    UiClosed(screen);
}

/* The hooks on presenter 40, the first time a mod opens a menu screen: 1 in. */
static int UiMenuHooks(void)
{
    if (g_uiMenuHooks) return g_uiMenuHooks > 0;
    uintptr_t game = UiGame();
    g_uiMenuHooks = -1;
    for (size_t i = 0; i < sizeof kUiMenuPins / sizeof kUiMenuPins[0]; i++)
        if (!PinMatches(game + kUiMenuPins[i].rva, kUiMenuPins[i].bytes, sizeof kUiMenuPins[i].bytes))
        { Note("ui: the game's menu presenters are not the reviewed ones (game.dll+0x%x); no menu screens", kUiMenuPins[i].rva); return 0; }
    if (LoaderHook("ui menu create", game + UI_MENU_CREATE, (void*)UiMenuCreate, (void**)&g_uiCreate) ||
        LoaderHook("ui menu enter", game + UI_MENU_ENTER, (void*)UiMenuEnter, (void**)&g_uiEnter) ||
        LoaderHook("ui menu update", game + UI_MENU_UPDATE, (void*)UiMenuUpdate, (void**)&g_uiUpdate) ||
        LoaderHook("ui menu input", game + UI_MENU_INPUT, (void*)UiMenuInput, (void**)&g_uiInput) ||
        LoaderHook("ui menu exit", game + UI_MENU_EXIT, (void*)UiMenuExit, (void**)&g_uiExit) ||
        LoaderHook("ui menu destroy", game + UI_MENU_DESTROY, (void*)UiMenuDestroy, (void**)&g_uiDestroy))
    { Note("ui: presenter 40 could not be hooked; no menu screens"); return 0; }
    g_uiMenuHooks = 1;
    return 1;
}

/* The waiting menu screen opened as presenter 40, once no game menu is open or opening. */
static void UiMenuShow(void)
{
    if (g_uiMenuScreen == 0 || g_uiMenuState != NULL) return;
    uint8_t* states = *(uint8_t* const*)(UiGame() + UI_MENU_STATES);
    if (states == NULL) return;
    uint8_t* menus = states + UI_MENUS;
    if (*(uint32_t*)(menus + 0xC) != 0 || *(uint32_t*)(menus + 0x28) != 0) return;
    UiScreen* s = &g_uiScreens[g_uiMenuScreen];
    g_uiMenuPending = s->element;
    ((void (*)(void*, int, void*))(UiGame() + UI_MENU_OPEN))(menus, UI_MENU_ID, NULL);
    g_uiMenuPending = NULL;
    if (g_uiMenuState != NULL) { Note("ui: %s's menu screen %d is up", s->mod->id, g_uiMenuScreen); return; }
    ApiLog(s->mod, "ui_open(%d): the game did not open the menu", g_uiMenuScreen);
    int screen = g_uiMenuScreen;
    g_uiMenuScreen = 0;
    UiClosed(screen);
}

/* ui_open on the game thread: a menu waits for UiMenuShow, an overlay joins the Containers grid. */
static void UiRunOpen(UiOp* op, UiScreen* s)
{
    void* element = NULL;
    ((void** (*)(void**, const char*))(UiGame() + UI_PARSE))(&element, op->text);
    void* grid = UiContainers();
    void* children = s->kind != HF_UI_MENU && element && grid ? ((void* (*)(void*))(UiGame() + UI_CHILDREN))(grid) : NULL;
    const char* why = element == NULL ? "the XAML does not parse (the game's Noesis log says why)" :
        s->kind != HF_UI_MENU ? (children == NULL ? "the game's UI is not up" : NULL) :
        g_uiMenuScreen != 0 ? "a menu screen of a mod is open already" :
        !UiMenuHooks() ? "this game's menus are not the reviewed ones" : NULL;
    if (why != NULL) { s->state = -1; ApiLog(s->mod, "ui_open(%d): %s", op->screen, why); UiRelease(element); return; }
    if (s->kind == HF_UI_MENU) g_uiMenuScreen = op->screen; /* shown by UiMenuShow */
    else ((void (*)(void*, unsigned, void*))(*(void***)children)[5])(children, *(unsigned*)((uint8_t*)children + 0x30), element);
    s->element = element;
    s->state = 1;
}

/* ui_close on the game thread: a menu closes as the game closes its own (UiMenuDestroy lets go), an overlay at once. */
static void UiRunClose(UiOp* op, UiScreen* s)
{
    if (s->kind == HF_UI_MENU && g_uiMenuState != NULL) { g_uiMenuState[0x10] = 1; return; }
    if (s->kind == HF_UI_MENU) g_uiMenuScreen = 0; /* still waiting for the presenter */
    void* grid = s->kind != HF_UI_MENU ? UiContainers() : NULL;
    uint8_t* children = grid ? ((uint8_t* (*)(void*))(UiGame() + UI_CHILDREN))(grid) : NULL;
    unsigned n = children ? *(unsigned*)(children + 0x30) : 0;
    for (unsigned i = 0; i < n; i++)
        if ((*(void***)(children + 0x28))[i] == s->element) { ((void (*)(void*, unsigned))(*(void***)children)[7])(children, i); break; }
    UiClosed(op->screen);
}

/* ui_set or ui_on on the game thread: why not, or NULL. */
static const char* UiRunElement(UiOp* op, UiScreen* s, void* element)
{
    if (op->kind == UI_SET) return UiSetString(element, op->property, op->value);
    if (g_uiCommandCount == UI_COMMANDS) return "mods set 256 click handlers already";
    UiCommand* c = &g_uiCommands[g_uiCommandCount++];
    *c = (UiCommand){s->mod, op->screen, "", op->callback, op->user, NULL};
    snprintf(c->element, sizeof c->element, "%s", op->element);
    return UiCommandOn(s, element, c);
}

/* A queued call, run on the game thread. */
static void UiRun(UiOp* op)
{
    UiScreen* s = &g_uiScreens[op->screen];
    if (op->kind == UI_OPEN) { UiRunOpen(op, s); return; }
    if (s->state != 1) return;
    if (op->kind == UI_ON_CLOSE) { s->closed = op->callback; s->closedUser = op->user; return; }
    if (op->kind == UI_CLOSE) { UiRunClose(op, s); return; }
    void* element = UiFind(s, op->element);
    if (element == NULL) { ApiLog(s->mod, "ui(%d): no element named \"%s\"", op->screen, op->element); return; }
    const char* why = UiRunElement(op, s, element);
    if (why != NULL) ApiLog(s->mod, "ui(%d): %s.%s: %s", op->screen, op->element, op->kind == UI_SET ? op->property : "click", why);
}

/* On the game thread: the queued calls, once the game's UI is up and proven. */
static void UiTick(void)
{
    if ((g_uiOpCount == 0 && g_uiMenuScreen == 0) || !UiProven() || UiContainers() == NULL) return;
    UiMenuShow();
    if (g_uiOpCount == 0) return;
    static UiOp ops[UI_OPS];
    AcquireSRWLockExclusive(&g_uiLock);
    int n = g_uiOpCount;
    memcpy(ops, g_uiOps, sizeof(UiOp) * (size_t)n);
    g_uiOpCount = 0;
    ReleaseSRWLockExclusive(&g_uiLock);
    for (int i = 0; i < n; i++) { UiRun(&ops[i]); free(ops[i].text); free(ops[i].value); }
    UiMenuShow();
}

/* A call queued for UiTick: 0 queued, 1 refused (logged). */
static int UiQueue(HfMod* mod, UiOp* op, const char* what)
{
    AcquireSRWLockExclusive(&g_uiLock);
    int full = g_uiOpCount == UI_OPS;
    if (!full) g_uiOps[g_uiOpCount++] = *op;
    ReleaseSRWLockExclusive(&g_uiLock);
    if (full) { ApiLog(mod, "%s: too many UI calls at once", what); free(op->text); free(op->value); }
    return full;
}

static int UiMine(HfMod* mod, int screen, const char* what)
{
    if (screen < 1 || screen > g_uiScreenCount || g_uiScreens[screen].mod != mod) { ApiLog(mod, "%s: %d is not a screen of this mod", what, screen); return 0; }
    return g_uiProven >= 0;
}

/* The XAML a mod names: a file of its folder ("ui/shop.xaml"), or XAML text (starting with '<'). malloc'd, or NULL. */
static char* UiXaml(HfMod* mod, const char* source)
{
    while (*source == ' ' || *source == '\t' || *source == '\r' || *source == '\n') source++;
    if (*source == '<') return _strdup(source);
    if (strstr(source, "..") != NULL || strchr(source, ':') != NULL) { ApiLog(mod, "ui_open: %s is not a file of the mod's folder", source); return NULL; }
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\%s", mod->folder, source);
    FILE* file = fopen(path, "rb");
    if (file == NULL) { ApiLog(mod, "ui_open: %s cannot be read", source); return NULL; }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* text = size > 0 && size < (4 << 20) ? (char*)malloc((size_t)size + 1) : NULL;
    size_t got = text ? fread(text, 1, (size_t)size, file) : 0;
    fclose(file);
    if (text == NULL || got != (size_t)size) { free(text); ApiLog(mod, "ui_open: %s is empty or over 4 MB", source); return NULL; }
    text[size] = 0;
    return text;
}

static int ApiUiOpen(HfMod* mod, const char* xaml, uint32_t kind)
{
    if (mod == NULL || xaml == NULL) return 0;
    if (kind != HF_UI_OVERLAY && kind != HF_UI_MENU) { ApiLog(mod, "ui_open: %u is not a kind of screen (HF_UI_OVERLAY, HF_UI_MENU)", kind); return 0; }
    if (g_uiProven < 0) { ApiLog(mod, "ui_open: this game's UI is not the reviewed one"); return 0; }
    if (g_uiScreenCount == UI_SCREENS) { ApiLog(mod, "ui_open: mods opened %d screens already", UI_SCREENS); return 0; }
    char* text = UiXaml(mod, xaml);
    if (text == NULL) return 0;
    int id = ++g_uiScreenCount;
    g_uiScreens[id] = (UiScreen){mod, kind, 0, NULL, NULL, NULL};
    UiOp op = {UI_OPEN, id, text, "", "", NULL, NULL, NULL};
    return UiQueue(mod, &op, "ui_open") ? 0 : id;
}

static int ApiUiClose(HfMod* mod, int screen)
{
    if (!UiMine(mod, screen, "ui_close")) return 1;
    UiOp op = {UI_CLOSE, screen, NULL, "", "", NULL, NULL, NULL};
    return UiQueue(mod, &op, "ui_close");
}

static int ApiUiSet(HfMod* mod, int screen, const char* element, const char* property, const char* value)
{
    if (!UiMine(mod, screen, "ui_set") || property == NULL || value == NULL) return 1;
    UiOp op = {UI_SET, screen, NULL, "", "", _strdup(value), NULL, NULL};
    snprintf(op.element, sizeof op.element, "%s", element ? element : "");
    snprintf(op.property, sizeof op.property, "%s", property);
    return UiQueue(mod, &op, "ui_set");
}

static int ApiUiOn(HfMod* mod, int screen, const char* element, HfUiCallback callback, void* user)
{
    if (!UiMine(mod, screen, "ui_on") || callback == NULL) return 1;
    UiOp op = {element ? UI_ON : UI_ON_CLOSE, screen, NULL, "", "", NULL, callback, user};
    snprintf(op.element, sizeof op.element, "%s", element ? element : "");
    return UiQueue(mod, &op, "ui_on");
}
