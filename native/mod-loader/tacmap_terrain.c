/* The tactical map's picture from the level's heights (tacmap.h): a hillshade lit from the north-west, an elevation tint
 * and contours, as the audited mockups (KB xl-tactical-map-spec). Runs on a worker thread; reads only its inputs. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "tacmap.h"

float TacHeightAt(const TacHeights* h, float x, float y)
{
    float c = (x + h->size * 0.5f) * (float)(h->columns - 1) / h->size;
    float r = (h->size * 0.5f - y) * (float)(h->rows - 1) / h->size;
    if (!(c >= 0.0f && r >= 0.0f && c <= (float)(h->columns - 1) && r <= (float)(h->rows - 1))) return NAN;
    uint32_t c0 = (uint32_t)c, r0 = (uint32_t)r;
    if (c0 >= h->columns - 1) c0 = h->columns - 2;
    if (r0 >= h->rows - 1) r0 = h->rows - 2;
    float fc = c - (float)c0, fr = r - (float)r0;
    const float* p = h->data + (size_t)r0 * h->columns + c0;
    return (p[0] * (1 - fc) + p[1] * fc) * (1 - fr) + (p[h->columns] * (1 - fc) + p[h->columns + 1] * fc) * fr;
}

static int CompareFloat(const void* a, const void* b)
{
    float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}

void TacHeightRange(TacHeights* h)
{
    /* every 16th sample of every 16th row is enough for the tint's range */
    uint32_t n = 0, cap = (h->columns / 16 + 1) * (h->rows / 16 + 1);
    float* s = (float*)malloc(sizeof(float) * cap);
    if (s == NULL) { h->low = 0.0f; h->high = 300.0f; return; }
    for (uint32_t r = 0; r < h->rows; r += 16)
        for (uint32_t c = 0; c < h->columns; c += 16) s[n++] = h->data[(size_t)r * h->columns + c];
    qsort(s, n, sizeof(float), CompareFloat);
    h->low = s[n / 100];
    h->high = s[n - 1 - n / 100];
    if (h->high - h->low < 20.0f) h->high = h->low + 20.0f;
    free(s);
}

/* One output sample of a blur pass: the kernel over `count` samples `stride` apart, centred on sample `at`, the ends
 * clamped. */
static float TacBlurTap(const float* f, size_t stride, int at, int count, const float* k, int radius)
{
    float a = 0.0f;
    for (int i = -radius; i <= radius; i++)
    {
        int j = at + i;
        j = j < 0 ? 0 : j >= count ? count - 1 : j;
        a += f[(size_t)(uint32_t)j * stride] * k[i + radius];
    }
    return a;
}

/* A separable Gaussian blur of a w x h float field, in place (edges clamped). */
static void Blur(float* f, uint32_t w, uint32_t h, float sigma, float* tmp)
{
    int radius = (int)ceilf(sigma * 3.0f);
    float k[64];
    if (radius > 31) radius = 31;
    float sum = 0.0f;
    for (int i = -radius; i <= radius; i++) sum += k[i + radius] = expf(-0.5f * (float)(i * i) / (sigma * sigma));
    for (int i = 0; i <= 2 * radius; i++) k[i] /= sum;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) tmp[(size_t)y * w + x] = TacBlurTap(f + (size_t)y * w, 1, (int)x, (int)w, k, radius);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) f[(size_t)y * w + x] = TacBlurTap(tmp + x, w, (int)y, (int)h, k, radius);
}

/* How much of a contour line of width `width` px covers this pixel: z's distance to the nearest multiple of step, in
 * pixels along the slope (g metres per pixel). Flat ground (g near 0) gets no line, so noise does not draw loops. */
static float Contour(float z, float g, float step, float width)
{
    float d = fabsf(z - step * roundf(z / step));
    if (g < 1e-4f) return 0.0f;
    float c = width * 0.5f + 0.5f - d / g;
    return c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c;
}

/* The fields TacTerrain shades a picture from, one sample per pixel. */
typedef struct TacTerrainFields
{
    const TacHeights* h;
    const TacPictureRequest* q;
    float* z;           /* the height, 0 outside the world */
    float* zc;          /* z generalised, for the contours */
    uint8_t* outside;
} TacTerrainFields;

static void TacTerrainSample(const TacTerrainFields* t)
{
    uint32_t width = t->q->width, height = t->q->height;
    float mpp = t->q->metres_per_px;
    for (uint32_t v = 0; v < height; v++)
        for (uint32_t u = 0; u < width; u++)
        {
            float x = t->q->cx + ((float)u + 0.5f - (float)width * 0.5f) * mpp;
            float y = t->q->cy - ((float)v + 0.5f - (float)height * 0.5f) * mpp;
            float s = TacHeightAt(t->h, x, y);
            size_t i = (size_t)v * width + u;
            t->outside[i] = isnan(s);
            t->z[i] = t->outside[i] ? 0.0f : s;
        }
}

static void TacTerrainHatch(uint32_t u, uint32_t v, uint8_t* o)
{
    int dark = ((u + v) / 6) % 2 == 0;
    o[0] = dark ? 24 : 14; o[1] = dark ? 24 : 15; o[2] = dark ? 26 : 17; o[3] = 255;
}

static uint8_t TacTerrainByte(float c) { return (uint8_t)(c < 0.0f ? 0.0f : c > 255.0f ? 255.0f : c + 0.5f); }

/* One pixel inside the world: the elevation tint, shaded by the hillshade, under the contour lines. */
static void TacTerrainPixel(const TacTerrainFields* f, uint32_t u, uint32_t v, uint8_t* o)
{
    const float lx = -0.55f, ly = 0.55f, lz = 0.63f;
    const float flat = lz / sqrtf(lx * lx + ly * ly + lz * lz);
    const float* z = f->z;
    const float* zc = f->zc;
    uint32_t width = f->q->width, height = f->q->height;
    float mpp = f->q->metres_per_px, span = f->h->high - f->h->low;
    size_t i = (size_t)v * width + u;
    uint32_t ul = u > 0 ? u - 1 : u, ur = u + 1 < width ? u + 1 : u;
    uint32_t vu = v > 0 ? v - 1 : v, vd = v + 1 < height ? v + 1 : v;
    float gx = (z[(size_t)v * width + ur] - z[(size_t)v * width + ul]) / ((float)(ur - ul) * mpp);
    float gy = (z[(size_t)vd * width + u] - z[(size_t)vu * width + u]) / ((float)(vd - vu) * mpp);
    /* slope exaggerated 2x; screen y runs south, so the north-west light is (-x, +screen y) */
    float nx = -gx * 2.0f, ny = gy * 2.0f, nz = 1.0f;
    float shade = (nx * lx + ny * ly + nz * lz) / sqrtf(nx * nx + ny * ny + nz * nz);
    shade = shade < 0.0f ? 0.0f : shade;
    float rel = shade - flat;
    float t = (z[i] - f->h->low) / span;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float col[3] = {31.0f + 20.0f * t, 37.0f + 15.0f * t, 37.0f + 8.0f * t};
    for (int k = 0; k < 3; k++) col[k] *= 1.0f + 1.6f * rel;
    float cgx = (zc[(size_t)v * width + ur] - zc[(size_t)v * width + ul]) / (float)(ur - ul);
    float cgy = (zc[(size_t)vd * width + u] - zc[(size_t)vu * width + u]) / (float)(vd - vu);
    float g = sqrtf(cgx * cgx + cgy * cgy); /* metres per pixel along the slope */
    float a = 0.16f * Contour(zc[i], g, f->q->minor, 0.75f);
    float b = 0.42f * Contour(zc[i], g, f->q->major, 1.4f);
    a = a > b ? a : b;
    static const float line[3] = {234.0f, 234.0f, 228.0f};
    for (int k = 0; k < 3; k++) o[k] = TacTerrainByte(col[k] * (1.0f - a) + line[k] * a);
    o[3] = 255;
}

void TacTerrain(const TacHeights* h, const TacPictureRequest* request, uint8_t* rgba)
{
    uint32_t width = request->width, height = request->height;
    size_t n = (size_t)width * height;
    float* z = (float*)malloc(sizeof(float) * n);
    float* zc = (float*)malloc(sizeof(float) * n);
    float* tmp = (float*)malloc(sizeof(float) * n);
    uint8_t* outside = (uint8_t*)malloc(n);
    if (z == NULL || zc == NULL || tmp == NULL || outside == NULL)
    {
        memset(rgba, 0, n * 4);
        free(z); free(zc); free(tmp); free(outside);
        return;
    }
    const TacTerrainFields f = {h, request, z, zc, outside};
    TacTerrainSample(&f);
    /* contours on a generalised field (about 3 px): raw 4 m samples wiggle at map scale */
    memcpy(zc, z, sizeof(float) * n);
    Blur(zc, width, height, 3.0f, tmp);
    for (uint32_t v = 0; v < height; v++)
        for (uint32_t u = 0; u < width; u++)
        {
            size_t i = (size_t)v * width + u;
            if (outside[i]) TacTerrainHatch(u, v, rgba + i * 4);
            else TacTerrainPixel(&f, u, v, rgba + i * 4);
        }
    free(z); free(zc); free(tmp); free(outside);
}
