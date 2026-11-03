/* What modloader.log says of each code mod as it loads (docs/modding/debugging.md): its version, from the plan's
 * mod_version line, and its DLL's SHA-256, read once, at the start of the session, from the file LoadLibrary is about
 * to map; and why a DLL that does not load failed (GetLastError). Included by hellforge_modloader.c before SetupMod. */

/* SHA-256 (FIPS 180-4): the loader links nothing beyond the C runtime and kernel32, so it carries its own. */
typedef struct Sha256 { uint32_t state[8]; uint64_t bytes; uint8_t block[64]; size_t used; } Sha256;

static const uint32_t kSha256Round[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t Rotr(uint32_t x, int n) { return x >> n | x << (32 - n); }

static void Sha256Block(Sha256* s, const uint8_t* p)
{
    uint32_t w[64], v[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++)
        w[i] = w[i - 16] + (Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] + (Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ w[i - 2] >> 10);
    memcpy(v, s->state, sizeof v);
    for (int i = 0; i < 64; i++)
    {
        uint32_t t1 = v[7] + (Rotr(v[4], 6) ^ Rotr(v[4], 11) ^ Rotr(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + kSha256Round[i] + w[i];
        uint32_t t2 = (Rotr(v[0], 2) ^ Rotr(v[0], 13) ^ Rotr(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, 7 * sizeof v[0]);
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->state[i] += v[i];
}

static void Sha256Update(Sha256* s, const uint8_t* data, size_t size)
{
    s->bytes += size;
    while (size > 0)
    {
        size_t take = 64 - s->used < size ? 64 - s->used : size;
        memcpy(s->block + s->used, data, take);
        s->used += take;
        data += take;
        size -= take;
        if (s->used == 64) { Sha256Block(s, s->block); s->used = 0; }
    }
}

/* The digest as 64 lowercase hex digits into hex (65 bytes). */
static void Sha256Hex(Sha256* s, char hex[65])
{
    uint64_t bits = s->bytes * 8;
    uint8_t pad[72] = {0x80};
    size_t padding = (s->used < 56 ? 56 : 120) - s->used;
    for (int i = 0; i < 8; i++) pad[padding + i] = (uint8_t)(bits >> (56 - 8 * i));
    Sha256Update(s, pad, padding + 8);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", (unsigned)(s->state[i / 4] >> (24 - 8 * (i % 4)) & 0xFF));
}

static void Sha256Start(Sha256* s)
{
    static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->state, initial, sizeof initial);
    s->bytes = 0;
    s->used = 0;
}

/* The SHA-256 of a file into hex (65 bytes): 0, or 1 with the error in *error when it cannot be read. */
static int FileSha256(const wchar_t* path, char hex[65], DWORD* error)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) { *error = GetLastError(); return 1; }
    Sha256 s;
    Sha256Start(&s);
    static uint8_t chunk[64 * 1024]; /* the mods load one after another, on the loader's thread */
    DWORD read = 0;
    BOOL ok;
    while ((ok = ReadFile(file, chunk, sizeof chunk, &read, NULL)) && read > 0) Sha256Update(&s, chunk, read);
    *error = ok ? 0 : GetLastError();
    CloseHandle(file);
    if (!ok) return 1;
    Sha256Hex(&s, hex);
    return 0;
}

/* modloader.log's line for a code mod about to load: "<id>: version <v>, <dll>, SHA-256 <hex>". */
static void NoteModIdentity(const HfMod* mod)
{
    const wchar_t* slash = wcsrchr(mod->dll, L'\\');
    const wchar_t* name = slash != NULL ? slash + 1 : mod->dll;
    const char* version = mod->version[0] ? mod->version : "not given";
    char hex[65];
    DWORD error = 0;
    if (FileSha256(mod->dll, hex, &error)) Note("%s: version %s, %ls, SHA-256 not read (error %lu)", mod->id, version, name, error);
    else Note("%s: version %s, %ls, SHA-256 %s", mod->id, version, name, hex);
}

/* Why LoadLibrary failed, from its GetLastError: "error 126: The specified module could not be found." into text. */
static void LoadError(DWORD error, char* text, size_t size)
{
    int length = snprintf(text, size, "error %lu", error);
    char message[256];
    DWORD got = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        message, sizeof message, NULL);
    if (got == 0) got = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0, message, sizeof message, NULL);
    while (got > 0 && (message[got - 1] == '\r' || message[got - 1] == '\n' || message[got - 1] == ' ')) message[--got] = 0;
    if (got > 0 && length > 0 && (size_t)length < size) snprintf(text + length, size - (size_t)length, ": %s", message);
}

/* The plan's mod_version line: the version of the mod of that id (the last listed), as its manifest gives it. */
static void PlanModVersion(const char* id, const char* version)
{
    for (int i = g_modCount - 1; i >= 0; i--)
        if (strcmp(g_mods[i].id, id) == 0) { snprintf(g_mods[i].version, sizeof g_mods[i].version, "%s", version); return; }
}
