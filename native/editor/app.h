/* The editor's window: the HD2R Editor's layout and look (its ui/app.lua, ui/views.lua and ui/search_view.lua),
 * over HellForge's catalogue and SDK. */
#pragma once
#include <string>
#include "imgui.h"
#include "hellforge_mod.h"

namespace editorapp
{
struct Host
{
    const HfApi* api;
    HfMod* mod;
    std::string modFolder, modsFolder;
    void (*close)(void);       /* closes the window (Escape, the X) */
    void (*saveSoon)(int ms);  /* saves the Local Stats changes in ms */
    void (*modal)(bool on);    /* the MOD SYNC prompt over the game, the window closed: shown (the game's input held) or gone */
};

void Init(const Host& host);
/* Content mods added (SDK 7, HfApi.content), once every mod has registered it: a page for each custom stratagem, and
 * its saved values (mod-stats.json) applied. Before the editor first draws (its icons go into the atlas). */
void AddModContent();
/* Pixels per 1080p unit for a screen: the editor's scale (the fonts are baked for it). */
float PixelScale(float width, float height);
/* Every frame, open or not (the window's lock held): HellForge's messages, the co-op role, and the mod content values
 * it calls for. */
void Tick();
/* One frame: input, then drawing into the list. */
void Frame(ImDrawList* list, float width, float height, float dt);
/* HellForge's message for the player (SDK 12 notice) is showing: the window draws it over the game while closed too. */
bool NoticeShown();
/* A frame of the closed window: only the message, no input. */
void Overlay(ImDrawList* list, float width, float height);
/* A frame of the MOD SYNC prompt alone over the game (the window closed): its input, then drawing into the list. */
void Modal(ImDrawList* list, float width, float height, float dt);
void Opened();
void Closed();
/* The status of the last Local Stats save, shown in the window. */
void SaveStatus(bool ok, const std::string& text);
} // namespace editorapp
