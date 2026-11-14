/* WinLicense unpacks game.dll section by section, each upward, and the build's pin is half way up .text: when it reads,
 * the code above it and the sections after it can still be packed for about 0.1 s. A pass that checked them then refused
 * for the session (bug reports 0299, 0346, 0429, 0496: Forge Credits' colours at 0x21cae80; 0401: the catalogue move at
 * 0x175b030). So with the build's own pin, the game counts as decoded once the top of each section HellForge reviews
 * reads as unpacked too (the bytes of the unpacked image); past DECODE_TOP_WAIT without that, as it did before. */
#define DECODE_TOP_WAIT 3000u
static const struct { uint32_t rva; uint8_t bytes[16]; } kDecodeTop[] = {
    {0x2110A70u, {0xc6, 0x20, 0x01, 0x48, 0x89, 0x1d, 0x36, 0x3b, 0x21, 0x01, 0x48, 0x8b, 0xc3, 0x48, 0x85, 0xdb}}, /* .text's last function */
    {0x21CAE80u, {0x00, 0x00, 0x80, 0x3f, 0xe3, 0xe2, 0xe2, 0x3e, 0xf6, 0xf5, 0x75, 0x3f, 0x00, 0x00, 0x80, 0x3f}}, /* .rdata: the Super Credits cyan (forge_credits.c) */
    {0x3997560u, {0x98, 0x26, 0x63, 0x02, 0x87, 0x0a, 0x11, 0x02, 0x8c, 0x0a, 0x11, 0x02, 0xac, 0x26, 0x63, 0x02}}, /* .pdata's last entries */
};
static int g_decodeTop; /* the build's own pin: the sections' tops count too */
static ULONGLONG g_decodePinRead;

/* Whether game.dll is unpacked to the top, the pin having read; noted when it took a while, or never came. */
static int DecodedToTheTop(uintptr_t base)
{
    if (!g_decodeTop) return 1;
    ULONGLONG now = GetTickCount64();
    if (g_decodePinRead == 0) g_decodePinRead = now;
    for (size_t i = 0; i < sizeof kDecodeTop / sizeof *kDecodeTop; i++)
    {
        uint8_t bytes[16];
        if (!SafeRead(base + kDecodeTop[i].rva, bytes, sizeof bytes) && memcmp(bytes, kDecodeTop[i].bytes, sizeof bytes) == 0) continue;
        if (now - g_decodePinRead < DECODE_TOP_WAIT) return 0;
        Note("game.dll: 0x%x still reads as packed %llu ms after the pin: counted as decoded", kDecodeTop[i].rva, now - g_decodePinRead);
        return 1;
    }
    if (now > g_decodePinRead) Note("game.dll: unpacked to the top %llu ms after the pin", now - g_decodePinRead);
    return 1;
}
