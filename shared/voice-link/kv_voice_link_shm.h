// SPDX-License-Identifier: MIT（このフォルダの LICENSE）
// VocalDress の声の受け渡しに使う共有メモリと時計（形は PROTOCOL.md）。
// Windows は名前付きの共有メモリ（Local\VocalDressVoiceLink.v1）と QueryPerformanceCounter、
// それ以外は POSIX の共有メモリ（/VocalDressVoiceLink.v1）と CLOCK_MONOTONIC。どちらも OBS の os_gettime_ns と同じ時計。
#pragma once

#include "kv_voice_link.h"

#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

namespace kv::voicelink {

inline std::uint64_t nowNs() {
#ifdef _WIN32
    static const std::uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return std::uint64_t(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const std::uint64_t t = std::uint64_t(c.QuadPart);
    // os_gettime_ns と同じく、桁あふれしないよう秒と端数に分けて ns にする
    return (t / freq) * 1000000000ull + (t % freq) * 1000000000ull / freq;
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return std::uint64_t(ts.tv_sec) * 1000000000ull + std::uint64_t(ts.tv_nsec);
#endif
}

// 共有メモリ。送り手は create、読み手は open。閉じるのはデストラクター
class SharedRegion {
public:
    SharedRegion() = default;
    SharedRegion(const SharedRegion&) = delete;
    SharedRegion& operator=(const SharedRegion&) = delete;
    ~SharedRegion() { close(); }

    // name は省くと本番の名前。試験は別の名前を使い、動いている送り手とぶつからないようにする
    bool create(const char* name = kDefaultName) { return map(true, name); }
    bool open(const char* name = kDefaultName) { return map(false, name); }
    static constexpr const char* kDefaultName = "VocalDressVoiceLink.v1";
    // 試験の後片付け（POSIX の共有メモリは名前が残るので消す。Windows は全員が閉じれば消える）
    static void remove(const char* name) {
#ifndef _WIN32
        shm_unlink((std::string("/") + name).c_str());
#else
        (void)name;
#endif
    }
    void* data() const { return data_; }
    std::size_t size() const { return data_ != nullptr ? kRegionBytes : 0; }

    void close() {
#ifdef _WIN32
        if (data_ != nullptr) UnmapViewOfFile(data_);
        if (handle_ != nullptr) CloseHandle(handle_);
        handle_ = nullptr;
#else
        if (data_ != nullptr) munmap(data_, kRegionBytes);
#endif
        data_ = nullptr;
    }

private:
    bool map(bool create, const char* shortName) {
        close();
#ifdef _WIN32
        std::wstring name = L"Local\\";
        for (const char* c = shortName; *c != 0; ++c) name += wchar_t(*c);  // 名前は ASCII だけ
        // 前の送り手の領域を読み手が開いたままなら、既にある領域が返る（中身はそのまま）。Writer が先頭を作り直す
        handle_ = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(kRegionBytes), name.c_str())
                         : OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        if (handle_ == nullptr) return false;
        data_ = MapViewOfFile(handle_, create ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ, 0, 0, kRegionBytes);
        if (data_ == nullptr) { close(); return false; }
        return true;
#else
        const std::string name = std::string("/") + shortName;
        const int fd = create ? shm_open(name.c_str(), O_CREAT | O_RDWR, 0600) : shm_open(name.c_str(), O_RDONLY, 0);
        if (fd < 0) return false;
        if (create && ftruncate(fd, off_t(kRegionBytes)) != 0) { ::close(fd); return false; }
        struct stat st {};
        if (fstat(fd, &st) != 0 || std::size_t(st.st_size) < kRegionBytes) { ::close(fd); return false; }
        void* p = mmap(nullptr, kRegionBytes, create ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) return false;
        data_ = p;
        return true;
#endif
    }

    void* data_ = nullptr;
#ifdef _WIN32
    HANDLE handle_ = nullptr;
#endif
};

}  // namespace kv::voicelink
