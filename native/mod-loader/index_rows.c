/* Component index rows read in one go, for the record lookups of hellforge_modloader.c and registry.c. Included by
 * hellforge_modloader.c. */

/* A component's index rows (16 bytes each) in one read, into this thread's buffer (kept for its next call); NULL when
 * they do not all read. One read a row cost the editor's live reads about a millisecond of every frame, on the thread
 * that renders (Health alone has 1002 rows, walked twice for each field). */
static const uint8_t* ReadIndexRows(uintptr_t address, uint32_t rows)
{
    static _Thread_local uint8_t* buffer;
    static _Thread_local size_t capacity;
    size_t size = (size_t)rows * 16;
    if (size == 0) return NULL;
    if (size > capacity)
    {
        uint8_t* grown = (uint8_t*)realloc(buffer, size);
        if (grown == NULL) return NULL;
        buffer = grown;
        capacity = size;
    }
    return SafeRead(address, buffer, size) ? NULL : buffer;
}

/* One index row (16 bytes: resource id, record, flags) of rows read by ReadIndexRows. */
static void IndexRowAt(const uint8_t* rows, uint32_t row, uint64_t* id, uint32_t* record, uint32_t* flags)
{
    const uint8_t* entry = rows + (size_t)row * 16;
    memcpy(id, entry, 8);
    memcpy(record, entry + 8, 4);
    memcpy(flags, entry + 12, 4);
}
