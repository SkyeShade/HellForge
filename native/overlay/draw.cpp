/* HfDraw on ImGui's foreground draw list: screen units (1080 high) scaled to back-buffer pixels, anti-aliased. Text
 * uses the game's two faces baked at a set of sizes for the window's height; a size between them scales the nearest. */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "host_internal.h"

namespace overlay
{
bool* g_backdropTaken;

namespace
{
const float SIZES[] = {10, 12, 14, 16, 18, 20, 24, 28, 32, 40, 48};
const int SIZE_COUNT = (int)(sizeof SIZES / sizeof SIZES[0]);

/* How the face's own numbers turn an em size into ImGui's pixel height (its line: ascender - descender) and where its
 * capitals stand, from its hhea, head and OS/2 tables. */
struct FaceMetrics { float lineToEm = 1.2f, capToEm = 0.7f; };

struct Fonts
{
    std::string file[2];
    FaceMetrics metrics[2];
    ImFont* baked[2][SIZE_COUNT];
    float scale;
    bool told;
};
Fonts g_fonts;

ImDrawList* g_drawList;
float g_s = 1; /* pixels per unit */
int g_clips;
std::vector<ImVec2> g_points;

uint32_t BigEndian(const std::vector<uint8_t>& b, size_t at, int bytes)
{
    uint32_t v = 0;
    for (int i = 0; i < bytes && at + i < b.size(); i++) v = v << 8 | b[at + i];
    return v;
}

std::wstring Wide(const std::string& utf8)
{
    wchar_t path[MAX_PATH * 2];
    return MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, path, MAX_PATH * 2) > 0 ? path : L"";
}

std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::vector<uint8_t> file;
    FILE* f = _wfopen(Wide(path).c_str(), L"rb");
    if (f == NULL) return file;
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    file.resize(length > 0 ? (size_t)length : 0);
    file.resize(fread(file.data(), 1, file.size(), f));
    fclose(f);
    return file;
}

FaceMetrics ReadFace(const std::string& path)
{
    FaceMetrics m;
    std::vector<uint8_t> file = ReadFile(path);
    float unitsPerEm = 0, line = 0, cap = 0;
    uint32_t tables = BigEndian(file, 4, 2);
    for (uint32_t t = 0; t < tables && file.size() >= 12 + 16 * (t + 1); t++)
    {
        size_t entry = 12 + 16 * t, at = BigEndian(file, entry + 8, 4);
        std::string tag(file.begin() + (long)entry, file.begin() + (long)entry + 4);
        if (tag == "head") unitsPerEm = (float)BigEndian(file, at + 18, 2);
        if (tag == "hhea") line = (float)(int16_t)BigEndian(file, at + 4, 2) - (float)(int16_t)BigEndian(file, at + 6, 2);
        if (tag == "OS/2" && BigEndian(file, at, 2) >= 2) cap = (float)(int16_t)BigEndian(file, at + 88, 2);
    }
    if (unitsPerEm > 0 && line > 0) m.lineToEm = line / unitsPerEm;
    if (unitsPerEm > 0 && cap > 0) m.capToEm = cap / unitsPerEm;
    return m;
}

bool Exists(const std::string& path) { return !path.empty() && GetFileAttributesW(Wide(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

const ImWchar* Ranges()
{
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2010, 0x2027, 0x2030, 0x203A, 0x20AC, 0x20AC, 0x2122, 0x2122,
                                     0x2190, 0x2193, 0x2212, 0x2212, 0};
    return ranges;
}

ImFont* BakeOne(int face, int size, float s)
{
    ImGuiIO& io = ImGui::GetIO();
    float pixels = SIZES[size] * s * g_fonts.metrics[face].lineToEm;
    ImFontConfig config;
    config.OversampleH = SIZES[size] * s < 24 ? 2 : 1; /* large text gains nothing from it and doubles its atlas space */
    config.OversampleV = 1;
    config.PixelSnapH = true;
    config.SizePixels = pixels < 6 ? 6 : pixels;
    if (!g_fonts.file[face].empty())
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(g_fonts.file[face].c_str(), config.SizePixels, &config, Ranges())) return font;
    return io.Fonts->AddFontDefault(&config);
}

int Nearest(float size)
{
    int best = 0;
    for (int i = 1; i < SIZE_COUNT; i++)
        if (fabsf(SIZES[i] - size) < fabsf(SIZES[best] - size)) best = i;
    return best;
}

ImVec2 Px(float x, float y) { return ImVec2(x * g_s, y * g_s); }

bool Visible(HfColour colour) { return (colour >> 24) != 0; }

/* ---- the table ---- */

void Rect(float x0, float y0, float x1, float y1, HfColour fill, HfColour outline, float thickness)
{
    if (x1 < x0) { float t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { float t = y0; y0 = y1; y1 = t; }
    if (Visible(fill)) g_drawList->AddRectFilled(Px(x0, y0), Px(x1, y1), fill);
    float half = fminf(thickness, fminf(x1 - x0, y1 - y0)) / 2; /* the outline lies inside the box */
    if (Visible(outline) && half > 0) g_drawList->AddRect(Px(x0 + half, y0 + half), Px(x1 - half, y1 - half), outline, 0, 0, 2 * half * g_s);
}

/* The points in pixels, clockwise on screen (ImGui's anti-aliased fill wants that order). */
const ImVec2* Points(const HfPoint* points, uint32_t count, bool clockwise)
{
    g_points.resize(count);
    float area = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        g_points[i] = Px(points[i].x, points[i].y);
        const HfPoint& next = points[(i + 1) % count];
        area += points[i].x * next.y - next.x * points[i].y;
    }
    if (clockwise && area < 0)
        for (uint32_t i = 0; i < count / 2; i++) std::swap(g_points[i], g_points[count - 1 - i]);
    return g_points.data();
}

void Lines(const HfPoint* points, uint32_t count, HfColour colour, float thickness, int closed)
{
    if (points == NULL || count < 2 || !Visible(colour)) return;
    g_drawList->AddPolyline(Points(points, count, false), (int)count, colour, closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness * g_s);
}

bool Convex(const ImVec2* p, uint32_t count)
{
    int sign = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        const ImVec2 &a = p[i], &b = p[(i + 1) % count], &c = p[(i + 2) % count];
        float turn = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
        int now = turn > 0 ? 1 : turn < 0 ? -1 : 0;
        if (now != 0 && sign != 0 && now != sign) return false;
        if (now != 0) sign = now;
    }
    return true;
}

void Polygon(const HfPoint* points, uint32_t count, HfColour fill)
{
    if (points == NULL || count < 3 || !Visible(fill)) return;
    const ImVec2* p = Points(points, count, true);
    if (Convex(p, count)) g_drawList->AddConvexPolyFilled(p, (int)count, fill);
    else g_drawList->AddConcavePolyFilled(p, (int)count, fill);
}

void Circle(float x, float y, float radius, HfColour fill, HfColour outline, float thickness)
{
    if (radius <= 0) return;
    if (Visible(fill)) g_drawList->AddCircleFilled(Px(x, y), radius * g_s, fill);
    float t = fminf(thickness, radius);
    if (Visible(outline) && t > 0) g_drawList->AddCircle(Px(x, y), (radius - t / 2) * g_s, outline, 0, t * g_s);
}

ImFont* FontFor(float size, uint32_t face, float* pixels)
{
    int i = Nearest(size);
    ImFont* font = g_fonts.baked[face > 1 ? 0 : face][i];
    if (font == NULL) font = ImGui::GetFont();
    *pixels = font->FontSize * size / SIZES[i];
    return font;
}

float TextWidth(const char* utf8, float size, uint32_t face)
{
    if (utf8 == NULL || size <= 0) return 0;
    float pixels;
    ImFont* font = FontFor(size, face, &pixels);
    return font->CalcTextSizeA(pixels, FLT_MAX, 0, utf8).x / g_s;
}

float Text(float x, float y, const char* utf8, float size, uint32_t face, uint32_t align, HfColour colour)
{
    float width = TextWidth(utf8, size, face);
    if (width <= 0 || !Visible(colour)) return width;
    x -= align == HF_ALIGN_RIGHT ? width : align == HF_ALIGN_CENTRE ? width / 2 : 0;
    float pixels;
    ImFont* font = FontFor(size, face, &pixels);
    float baseline = y * g_s + size * g_s * g_fonts.metrics[face > 1 ? 0 : face].capToEm / 2;
    ImVec2 at(floorf(x * g_s + 0.5f), floorf(baseline - font->Ascent * pixels / font->FontSize + 0.5f));
    g_drawList->AddText(font, pixels, at, colour, utf8);
    return width;
}

void Image(HfImage* image, const HfRect* box, const HfRect* part, HfColour tint)
{
    static const HfRect all = {0.0f, 0.0f, 1.0f, 1.0f};
    ImTextureID texture = ImageTexture(image);
    if (box == nullptr || texture == (ImTextureID)0 || !Visible(tint)) return;
    if (part == nullptr) part = &all;
    g_drawList->AddImage(texture, Px(box->x0, box->y0), Px(box->x1, box->y1), ImVec2(part->x0, part->y0), ImVec2(part->x1, part->y1), tint);
}

void ClipPush(float x0, float y0, float x1, float y1)
{
    g_drawList->PushClipRect(Px(x0, y0), Px(x1, y1), true);
    g_clips++;
}

void ClipPop()
{
    if (g_clips <= 0) return;
    g_drawList->PopClipRect();
    g_clips--;
}

void BackdropCall(float dim, float desaturate, float blur)
{
    bool spare = true;
    Backdrop(g_drawList, dim < 0 ? 0 : dim > 1 ? 1 : dim, desaturate < 0 ? 0 : desaturate > 1 ? 1 : desaturate, blur > 0 ? blur * g_s : 0,
             g_backdropTaken != NULL ? g_backdropTaken : &spare);
}
} // namespace

void SetFontFiles(const char* body, const char* title)
{
    g_fonts.file[0] = body != NULL && Exists(body) ? body : "";
    g_fonts.file[1] = title != NULL && Exists(title) ? title : g_fonts.file[0];
    for (int face = 0; face < 2; face++)
        if (!g_fonts.file[face].empty()) g_fonts.metrics[face] = ReadFace(g_fonts.file[face]);
    if (g_fonts.file[0].empty()) Log("overlay: the game's fonts were not found (%s); text uses ImGui's own font", body != NULL ? body : "no path");
    else Log("overlay: fonts %s and %s", g_fonts.file[0].c_str(), g_fonts.file[1].c_str());
}

bool BakeFonts(float s)
{
    if (fabsf(g_fonts.scale - s) < 0.005f) return false;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_fonts.scale = s;
    for (int face = 0; face < 2; face++)
        for (int i = 0; i < SIZE_COUNT; i++) g_fonts.baked[face][i] = BakeOne(face, i, s);
    io.Fonts->Build();
    io.FontDefault = g_fonts.baked[0][3];
    Log("overlay: fonts baked at %.3f pixels a unit (atlas %dx%d)", s, io.Fonts->TexWidth, io.Fonts->TexHeight);
    return true;
}

void BeginDraw(HfDraw* draw, float width, float height)
{
    g_drawList = ImGui::GetForegroundDrawList();
    g_s = height / 1080.0f;
    g_clips = 0;
    *draw = HfDraw{};
    draw->size = sizeof *draw;
    draw->width = width / g_s;
    draw->height = 1080;
    draw->pixels = g_s;
    draw->rect = Rect;
    draw->lines = Lines;
    draw->polygon = Polygon;
    draw->circle = Circle;
    draw->text = Text;
    draw->text_width = TextWidth;
    draw->image_create = ImageCreate;
    draw->image_update = ImageUpdate;
    draw->image_free = ImageFree;
    draw->image = Image;
    draw->clip_push = ClipPush;
    draw->clip_pop = ClipPop;
    draw->backdrop = BackdropCall;
}

void EndScreenDraw()
{
    static bool told;
    if (g_clips > 0 && !told) Log("overlay: a screen left %d clip(s) pushed; the host pops them (said once)", g_clips);
    told = told || g_clips > 0;
    while (g_clips > 0) ClipPop();
}
} // namespace overlay
