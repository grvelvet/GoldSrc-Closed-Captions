#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace hook {

// Minimal x86-32 instruction length decoder for function prologues (covers common MSVC/Clang prologues)
inline size_t InsnLen32(const uint8_t* p) {
    uint8_t op = p[0];
    size_t len = 1;
    // Skip prefixes (66, 67, F2, F3, 2E, 3E, 64, 65)
    while (op == 0x66 || op == 0x67 || op == 0xF2 || op == 0xF3 ||
           op == 0x2E || op == 0x3E || op == 0x64 || op == 0x65) {
        p++; len++; op = p[0];
    }
    // Single-byte push/pop/inc/dec/nop/ret
    if ((op >= 0x50 && op <= 0x5F) || op == 0x90 || op == 0xC3 || op == 0xCC) return len;
    // push imm8 (6A xx)
    if (op == 0x6A) return len + 1;
    // push imm32 (68 xx xx xx xx)
    if (op == 0x68) return len + 4;
    // mov reg, imm32 (B8..BF)
    if (op >= 0xB8 && op <= 0xBF) return len + 4;
    // mov eax, [disp32] (A1) / mov [disp32], eax (A3)
    if (op == 0xA1 || op == 0xA3) return len + 4;
    // rel8 jmp/jcc
    if (op == 0xEB || (op >= 0x70 && op <= 0x7F)) return len + 1;
    // call/jmp rel32
    if (op == 0xE8 || op == 0xE9) return len + 4;

    // ModRM instructions: 8B, 89, 8A, 88, 8D, 83, 81, 33, 31, 3B, 39, 2B, 29, 03, 01, C7, FF
    auto modrm_len = [&](const uint8_t* m, int imm_bytes) -> size_t {
        uint8_t mod = (*m >> 6) & 3;
        uint8_t rm  = *m & 7;
        size_t extra = 1; // ModRM byte
        if (mod != 3 && rm == 4) { // SIB
            uint8_t sib = m[1];
            extra++;
            if (mod == 0 && (sib & 7) == 5) extra += 4;
        }
        if (mod == 0 && rm == 5) extra += 4;
        else if (mod == 1) extra += 1;
        else if (mod == 2) extra += 4;
        return len + extra + imm_bytes;
    };

    if (op == 0x8B || op == 0x89 || op == 0x8A || op == 0x88 || op == 0x8D ||
        op == 0x33 || op == 0x31 || op == 0x3B || op == 0x39 ||
        op == 0x2B || op == 0x29 || op == 0x03 || op == 0x01 || op == 0x85 || op == 0xFF) {
        return modrm_len(p + 1, 0);
    }
    if (op == 0x83 || op == 0xC1 || op == 0xC0) return modrm_len(p + 1, 1);
    if (op == 0x81 || op == 0xC7) return modrm_len(p + 1, 4);
    if (op == 0x0F) {
        uint8_t op2 = p[1];
        len++;
        if (op2 >= 0x80 && op2 <= 0x8F) return len + 4;
        return modrm_len(p + 2, 0);
    }
    return 0; // unknown
}

inline void* InstallInlineHook(void* target, void* detour) {
    if (!target || !detour) return nullptr;
    uint8_t* src = static_cast<uint8_t*>(target);

    size_t stolen = 0;
    while (stolen < 5) {
        size_t ilen = InsnLen32(src + stolen);
        if (ilen == 0) return nullptr;
        stolen += ilen;
    }

    uint8_t* tramp = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, stolen + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return nullptr;

    std::memcpy(tramp, src, stolen);
    // Fixup if first instruction was rel32 call/jmp
    if (stolen == 5 && (src[0] == 0xE8 || src[0] == 0xE9)) {
        int32_t orig_disp;
        std::memcpy(&orig_disp, src + 1, 4);
        uintptr_t orig_target = reinterpret_cast<uintptr_t>(src + 5) + orig_disp;
        int32_t new_disp = static_cast<int32_t>(orig_target - reinterpret_cast<uintptr_t>(tramp + 5));
        std::memcpy(tramp + 1, &new_disp, 4);
    }

    tramp[stolen] = 0xE9;
    int32_t back_disp = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(src + stolen) - reinterpret_cast<uintptr_t>(tramp + stolen + 5));
    std::memcpy(tramp + stolen + 1, &back_disp, 4);

    DWORD oldProt = 0;
    if (!VirtualProtect(src, stolen, PAGE_EXECUTE_READWRITE, &oldProt)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return nullptr;
    }
    src[0] = 0xE9;
    int32_t hook_disp = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(detour) - reinterpret_cast<uintptr_t>(src + 5));
    std::memcpy(src + 1, &hook_disp, 4);
    for (size_t i = 5; i < stolen; ++i) src[i] = 0x90;
    VirtualProtect(src, stolen, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), src, stolen);
    return tramp;
}

inline void* FindFunctionByStringXref(HMODULE mod, const char* needle) {
    if (!mod || !needle) return nullptr;
    auto base = reinterpret_cast<const uint8_t*>(mod);
    auto dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt   = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const IMAGE_SECTION_HEADER* secs = IMAGE_FIRST_SECTION(nt);
    const uint8_t* strAddr = nullptr;
    size_t nlen = std::strlen(needle);

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections && !strAddr; ++i) {
        const uint8_t* sstart = base + secs[i].VirtualAddress;
        size_t ssize = secs[i].Misc.VirtualSize;
        if (ssize < nlen) continue;
        for (size_t off = 0; off + nlen <= ssize; ++off) {
            if (std::memcmp(sstart + off, needle, nlen) == 0) {
                strAddr = sstart + off;
                break;
            }
        }
    }
    if (!strAddr) return nullptr;

    uint32_t targetVA = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(strAddr));
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(secs[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* cstart = base + secs[i].VirtualAddress;
        size_t csize = secs[i].Misc.VirtualSize;
        for (size_t off = 0; off + 4 <= csize; ++off) {
            uint32_t val;
            std::memcpy(&val, cstart + off, 4);
            if (val == targetVA) {
                // Walk backwards up to 0x400 bytes for standard prologue (55 8B EC) preceded by CC/90/C3
                size_t backLimit = (off > 0x400) ? (off - 0x400) : 0;
                for (size_t k = off; k > backLimit; --k) {
                    if (cstart[k] == 0x55 && cstart[k + 1] == 0x8B && cstart[k + 2] == 0xEC) {
                        if (k == 0 || cstart[k - 1] == 0xCC || cstart[k - 1] == 0x90 || cstart[k - 1] == 0xC3) {
                            return const_cast<uint8_t*>(cstart + k);
                        }
                    }
                }
            }
        }
    }
    return nullptr;
}

} // namespace hook
