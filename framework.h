#pragma once

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files
#include <windows.h>
#include "MemoryMgr.h"
using namespace Memory::VP;
//#include "fmod_studio.hpp"
#include "fmod.hpp"

// cdecl
template<typename Ret = void, typename... Args>
inline Ret cdecl_call(uintptr_t addr, Args... args) {
    return reinterpret_cast<Ret(__cdecl*)(Args...)>(addr)(args...);
}

// stdcall
template<typename Ret = void, typename... Args>
inline Ret stdcall_call(uintptr_t addr, Args... args) {
    return reinterpret_cast<Ret(__stdcall*)(Args...)>(addr)(args...);
}

// fastcall
template<typename Ret = void, typename... Args>
inline Ret fastcall_call(uintptr_t addr, Args... args) {
    return reinterpret_cast<Ret(__fastcall*)(Args...)>(addr)(args...);
}

// thiscall
template<typename Ret = void, typename... Args>
inline Ret thiscall_call(uintptr_t addr, Args... args) {
    return reinterpret_cast<Ret(__thiscall*)(Args...)>(addr)(args...);
}
