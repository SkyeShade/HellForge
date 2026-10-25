/* How a bridged method's arguments and results are laid out in a pipe message (docs/steam-bridge.md, phase 3), for both
 * ends: values in their own size, little-endian; a string as a uint16 length (0xFFFF: NULL) and its bytes; a buffer or
 * an out pointer as a uint8 "present" flag, then in the result its bytes. Every read is bounded: a short message fails
 * the read, never reads past it. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BRIDGE_MARSHAL_CAP 8176  /* BRIDGE_MSG_BUF less the call's and the reply's headers */
#define BRIDGE_NULL_STRING 0xFFFFu

typedef struct BridgeWriter { uint32_t size; int failed; uint8_t data[BRIDGE_MARSHAL_CAP]; } BridgeWriter;
typedef struct BridgeReader { uint32_t size, at; const uint8_t* data; } BridgeReader;

static inline BridgeWriter BridgeWriterStart(void) { BridgeWriter w; w.size = 0; w.failed = 0; return w; }

static inline void BridgePut(BridgeWriter* w, const void* value, uint32_t size)
{
    if (w->failed || size > BRIDGE_MARSHAL_CAP - w->size) { w->failed = 1; return; }
    memcpy(w->data + w->size, value, size);
    w->size += size;
}

static inline void BridgePutFlag(BridgeWriter* w, bool present) { uint8_t b = present ? 1 : 0; BridgePut(w, &b, 1); }

/* A string of at most max bytes; returns 0 (and writes nothing) when it is longer. */
static inline int BridgePutString(BridgeWriter* w, const char* s, uint32_t max)
{
    uint16_t length = BRIDGE_NULL_STRING;
    if (s)
    {
        size_t n = strnlen(s, (size_t)max + 1);
        if (n > max) return 0;
        length = (uint16_t)n;
    }
    BridgePut(w, &length, 2);
    if (s && length) BridgePut(w, s, length);
    return !w->failed;
}

/* A string Steam returned: cut at max bytes. */
static inline void BridgePutStringTruncated(BridgeWriter* w, const char* s, uint32_t max)
{
    uint16_t length = s ? (uint16_t)strnlen(s, max) : BRIDGE_NULL_STRING;
    BridgePut(w, &length, 2);
    if (s && length) BridgePut(w, s, length);
}

static inline int BridgeGet(BridgeReader* r, void* value, uint32_t size)
{
    if (size > r->size - r->at) return 0;
    memcpy(value, r->data + r->at, size);
    r->at += size;
    return 1;
}

/* The broker's read of an input string: at most max bytes, NUL-terminated into buf (max + 1 bytes); *isNull for NULL.
 * 0 when it is longer than max, holds a NUL, or the message is short. */
static inline int BridgeGetBoundedString(BridgeReader* r, char* buf, uint32_t max, int* isNull)
{
    uint16_t length;
    *isNull = 0;
    buf[0] = 0;
    if (!BridgeGet(r, &length, 2)) return 0;
    if (length == BRIDGE_NULL_STRING) { *isNull = 1; return 1; }
    if (length > max || length > r->size - r->at || memchr(r->data + r->at, 0, length)) return 0;
    memcpy(buf, r->data + r->at, length);
    buf[length] = 0;
    r->at += length;
    return 1;
}

/* The game's read of a returned string into buf (capacity bytes, NUL-terminated, cut to fit). */
static inline int BridgeGetString(BridgeReader* r, char* buf, uint32_t capacity)
{
    uint16_t length;
    buf[0] = 0;
    if (!BridgeGet(r, &length, 2)) return 0;
    if (length == BRIDGE_NULL_STRING) return 1;
    if (length > r->size - r->at) return 0;
    uint32_t n = length < capacity - 1 ? length : capacity - 1;
    memcpy(buf, r->data + r->at, n);
    buf[n] = 0;
    r->at += length;
    return 1;
}

static inline bool BridgePrintable(const char* s)
{
    for (; *s; s++) if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) return false;
    return true;
}

/* The table's "refusedStrings": the same text, ASCII letters in either case. */
static inline bool BridgeSameText(const char* s, const char* refused)
{
    for (; *s && *refused; s++, refused++)
    {
        char a = *s >= 'A' && *s <= 'Z' ? (char)(*s + 32) : *s, b = *refused >= 'A' && *refused <= 'Z' ? (char)(*refused + 32) : *refused;
        if (a != b) return false;
    }
    return *s == 0 && *refused == 0;
}
