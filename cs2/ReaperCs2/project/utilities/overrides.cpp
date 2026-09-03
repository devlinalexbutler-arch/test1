#include <pch/pch.hpp>
#include <protection/game_addresses.hpp>
#include <utilities/memory/memory.hpp>

#if defined(_MSC_VER) && !defined(__clang__)
#define KRYPTIK_FORCEINLINE __forceinline
#else
#define KRYPTIK_FORCEINLINE inline __attribute__((always_inline))
#endif

KRYPTIK_FORCEINLINE void* game_alloc (std::size_t size) {
	const auto memalloc = *reinterpret_cast <void**>(MODULE_EXPORT ("tier0.dll:g_pMemAlloc"));
	const auto vtable = *reinterpret_cast<void***>(memalloc);
	return reinterpret_cast<void* (__thiscall*)(void*, std::size_t)>(vtable [1])(memalloc, size);
}

KRYPTIK_FORCEINLINE void game_free (void* ptr) {
	const auto memalloc = *reinterpret_cast <void**>(MODULE_EXPORT ("tier0.dll:g_pMemAlloc"));
	const auto vtable = *reinterpret_cast<void***>(memalloc);
	reinterpret_cast<void (__thiscall*)(void*, void*)>(vtable [3])(memalloc, ptr);
}

void* __cdecl operator new(std::size_t size) {
	return game_alloc (size);
}

void* __cdecl operator new [] (std::size_t size) {
	return game_alloc (size);
}

void __cdecl operator delete(void* ptr) noexcept {
	game_free (ptr);
}

void __cdecl operator delete [] (void* ptr) noexcept {
	game_free (ptr);
}

void __cdecl operator delete(void* ptr, std::size_t) noexcept {
	game_free (ptr);
}

void __cdecl operator delete [] (void* ptr, std::size_t) noexcept {
	game_free (ptr);
}
