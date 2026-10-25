/* The editor's images in one texture: the game's weapon and throwable silhouettes and faction emblems (game-icons.bin,
 * extracted from the player's own game by HellForge's EditorIcons.cs; nothing of them ships), the HD2R Editor's marker
 * icons (ui_marks.h), and HellForge's icon (ui_logo.h). The masks are drawn in a colour, the icon as it is. */
#pragma once
#include <d3d11.h>
#include <d3d12.h>
#include <string>
#include "imgui.h"

namespace icons
{
/* the kinds of game-icons.bin (EditorIcons.cs), then the editor's own */
enum Kind { WEAPON = 0, THROWABLE = 1, STRATAGEM = 2, STRATAGEM_WHITE = 3, BOOSTER = 4, PASSIVE = 5, MOD = 6, FACTION = 7, DIFFICULTY = 8, MARK = 16 };
struct Icon { ImVec2 uv0, uv1; int width, height; };

/* Reads game-icons.bin (when there is one; version 1 or 2) and lays every image out in the atlas. Returns how many game
 * icons. A stratagem is two masks (STRATAGEM, its category layer, and STRATAGEM_WHITE), a booster a colour image. */
int Load(const std::wstring& gameIcons);
const Icon* Find(Kind kind, const std::string& name);
/* A mod's own image (a custom stratagem's icon: the PNG its data builds the game's icon from), in colour, by name; read
 * with Windows' image decoder and made at most 128 pixels. Only before Upload. False when it cannot be read. */
bool AddImage(Kind kind, const std::string& name, const std::wstring& path);
/* A mod's stratagem icon as the game draws one: the image's red is its category layer (STRATAGEM, drawn in the
 * category's colour), its green the white layer (STRATAGEM_WHITE), the stratagem pack's icon format. */
bool AddStratagem(const std::string& name, const std::wstring& path);
const Icon* Mark(int mark); /* UI_MARK_... */
const Icon* Logo(); /* HellForge's icon, in colour (ui_logo.h) */

typedef void(STDMETHODCALLTYPE* ExecuteFn)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
/* Makes the texture on the game's device and queue (once), its view at the given descriptor. */
bool Upload(ID3D12Device* device, ID3D12CommandQueue* queue, ExecuteFn execute, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);
/* The same on a Direct3D 11 game's device (--use-d3d11): the texture and its view, which is the ImTextureID. */
bool Upload11(ID3D11Device* device);
ImTextureID Texture();
} // namespace icons
