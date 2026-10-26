#include "link.h"
#include <stdlib.h>
#include <string.h>

namespace hellforge {
namespace {

constexpr uint32_t MaxFrameBytes = 1u << 20;
constexpr uint32_t MaxFields = 4096;

void PutU32(std::string& out, uint32_t value)
{
    for (int i = 0; i < 4; i++) out.push_back((char)((value >> (8 * i)) & 0xff));
}

uint32_t GetU32(const unsigned char* data) { return data[0] | (data[1] << 8) | (data[2] << 16) | ((uint32_t)data[3] << 24); }

// Overlapped I/O, so the reader thread's pending read never blocks a write from the game's thread.
bool Transfer(HANDLE pipe, HANDLE event, void* buffer, DWORD size, bool write)
{
    DWORD done = 0;
    while (size > 0)
    {
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        BOOL started = write ? WriteFile(pipe, buffer, size, nullptr, &overlapped) : ReadFile(pipe, buffer, size, nullptr, &overlapped);
        if (!started && GetLastError() != ERROR_IO_PENDING) return false;
        if (!GetOverlappedResult(pipe, &overlapped, &done, TRUE) || done == 0) return false;
        buffer = (char*)buffer + done;
        size -= done;
    }
    return true;
}

// A frame's body (its length bytes after the length): the field count, then each field, which must fill it exactly.
// False when it is malformed.
bool Decode(const std::vector<unsigned char>& buffer, uint32_t length, Frame& frame)
{
    uint32_t count = buffer[0] | (buffer[1] << 8);
    size_t at = 2;
    if (count == 0) return false;
    for (uint32_t i = 0; i < count; i++)
    {
        if (at + 4 > length) return false;
        uint32_t size = GetU32(&buffer[at]);
        if (size > length - at - 4) return false;
        std::string field((const char*)&buffer[at + 4], size);
        if (i == 0) frame.type = field;
        else frame.fields.push_back(field);
        at += 4 + size;
    }
    return at == length;
}

}

Frame& Frame::AddProperties(const std::map<std::string, std::string>& properties)
{
    Add((long long)properties.size());
    for (const auto& property : properties) Add(property.first).Add(property.second);
    return *this;
}

std::string Frame::Encode() const
{
    std::string body;
    uint16_t count = (uint16_t)(fields.size() + 1);
    body.push_back((char)(count & 0xff));
    body.push_back((char)(count >> 8));
    PutU32(body, (uint32_t)type.size());
    body += type;
    for (const std::string& field : fields)
    {
        PutU32(body, (uint32_t)field.size());
        body += field;
    }
    std::string frame;
    PutU32(frame, (uint32_t)body.size());
    return frame + body;
}

std::string FrameReader::Text()
{
    if (index_ >= frame_.fields.size()) { ok = false; return ""; }
    return frame_.fields[index_++];
}

long long FrameReader::Int()
{
    std::string text = Text();
    char* end = nullptr;
    long long value = strtoll(text.c_str(), &end, 10);
    if (text.empty() || end == nullptr || *end != '\0') ok = false;
    return value;
}

std::map<std::string, std::string> FrameReader::Properties()
{
    std::map<std::string, std::string> properties;
    long long count = Int();
    if (count < 0 || count > MaxFields) { ok = false; return properties; }
    for (long long i = 0; i < count && ok; i++)
    {
        std::string key = Text();
        properties[key] = Text();
    }
    return properties;
}

std::vector<FrameReader::Change> FrameReader::Changes()
{
    std::vector<Change> changes;
    long long count = Int();
    if (count < 0 || count > MaxFields) { ok = false; return changes; }
    for (long long i = 0; i < count && ok; i++)
    {
        Change change;
        change.key = Text();
        change.remove = Text() == "remove";
        change.value = Text();
        changes.push_back(change);
    }
    return changes;
}

bool Link::Connect(const char* component)
{
    wchar_t name[128];
    DWORD length = GetEnvironmentVariableW(L"HELLFORGE_PIPE", name, 100);
    if (length == 0 || length >= 100) return false;
    wchar_t path[160];
    if (swprintf(path, 160, L"\\\\.\\pipe\\%ls", name) < 0) return false;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 10 && pipe == INVALID_HANDLE_VALUE; attempt++)
    {
        pipe = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe == INVALID_HANDLE_VALUE && !WaitNamedPipeW(path, 500)) Sleep(100);
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;
    pipe_ = pipe;
    writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!Send(Frame("hello").Add(component).Add((long long)GetCurrentProcessId()))) return false;
    HANDLE thread = CreateThread(nullptr, 0, ReaderThread, this, 0, nullptr);
    if (thread == nullptr) { broken_ = true; return false; }
    CloseHandle(thread);
    return true;
}

bool Link::Send(const Frame& frame)
{
    if (pipe_ == nullptr || broken_) return false;
    std::string bytes = frame.Encode();
    if (bytes.size() > MaxFrameBytes) return false;
    AcquireSRWLockExclusive(&writeLock_);
    bool sent = Transfer(pipe_, writeEvent_, bytes.data(), (DWORD)bytes.size(), true);
    ReleaseSRWLockExclusive(&writeLock_);
    if (!sent) broken_ = true;
    return sent;
}

std::vector<Frame> Link::Drain()
{
    std::vector<Frame> frames;
    AcquireSRWLockExclusive(&inboxLock_);
    frames.swap(inbox_);
    ReleaseSRWLockExclusive(&inboxLock_);
    return frames;
}

DWORD WINAPI Link::ReaderThread(void* self)
{
    static_cast<Link*>(self)->Read();
    return 0;
}

void Link::Read()
{
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<unsigned char> buffer;
    for (;;)
    {
        unsigned char header[4];
        if (!Transfer(pipe_, event, header, 4, false)) break;
        uint32_t length = GetU32(header);
        if (length < 2 || length + 4 > MaxFrameBytes) break;
        buffer.resize(length);
        if (!Transfer(pipe_, event, buffer.data(), length, false)) break;
        Frame frame;
        if (!Decode(buffer, length, frame)) break;
        AcquireSRWLockExclusive(&inboxLock_);
        inbox_.push_back(std::move(frame));
        ReleaseSRWLockExclusive(&inboxLock_);
    }
    broken_ = true;
    CloseHandle(event);
}

}
