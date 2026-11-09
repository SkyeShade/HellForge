/* A code mod's identity in modloader.log (mod_identity.c): SHA-256 against FIPS 180-4's examples (empty, one block,
 * two blocks, a million bytes in pieces), a file's hash, a file that cannot be read, and a load error with Windows'
 * message. Included by stats_test.c. */

static void Sha256Of(const void* data, size_t size, char hex[65])
{
    Sha256 s;
    Sha256Start(&s);
    Sha256Update(&s, data, size);
    Sha256Hex(&s, hex);
}

static void ModIdentityFixture(void)
{
    char hex[65];
    Sha256Of("", 0, hex);
    assert(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    Sha256Of("abc", 3, hex);
    assert(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char* twoBlocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    Sha256Of(twoBlocks, strlen(twoBlocks), hex);
    assert(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    Sha256 s;
    Sha256Start(&s);
    uint8_t piece[1001];
    memset(piece, 'a', sizeof piece);
    for (int i = 0; i < 1000; i++) Sha256Update(&s, piece, i % 2 ? 999 : 1001); /* pieces across block boundaries */
    Sha256Hex(&s, hex);
    assert(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

    wchar_t folder[MAX_PATH], path[MAX_PATH];
    assert(GetTempPathW(MAX_PATH, folder));
    swprintf(path, MAX_PATH, L"%lshellforge-mod-identity-%lu.dll", folder, GetCurrentProcessId());
    FILE* file = _wfopen(path, L"wb");
    assert(file != NULL && fwrite("abc", 1, 3, file) == 3);
    fclose(file);
    DWORD error = 1;
    assert(!FileSha256(path, hex, &error) && error == 0 && !strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    DeleteFileW(path);
    assert(FileSha256(path, hex, &error) == 1 && error == ERROR_FILE_NOT_FOUND);

    char text[320];
    assert(LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) == NULL);
    LoadError(GetLastError(), text, sizeof text);
    assert(!strncmp(text, "error 126: ", 11) && strlen(text) > 12 && text[strlen(text) - 1] != '\n'); /* with Windows' message */
    LoadError(0xDEADBEEF, text, sizeof text);
    assert(!strcmp(text, "error 3735928559")); /* no message: the number alone */
    puts("PASS mod identity: SHA-256 (FIPS 180-4 examples, pieces across blocks), a file's hash, an unreadable file, load errors");
}
