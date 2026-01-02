// pch.h: This is a precompiled header file.
// Files listed below are compiled only once, improving build performance for future builds.
// This also affects IntelliSense performance, including code completion and many code browsing features.
// However, files listed here are ALL re-compiled if any one of them is updated between builds.
// Do not add files here that you will be updating frequently as this negates the performance advantage.

#ifndef PCH_H
#define PCH_H
#include <cstdint>
#include <array>
#include <string>

#define TRUE 1
#define FALSE 0

constexpr const char editor_header_prefix[] = "Reloaded Chaos Theory Editor";
constexpr const char verbose_save_message[] = "\n\n(current version of the map will be rebuilt before saving)\n\n\nWarning: Quitting without saving might result in incoherent data.";
constexpr const char editor_header[] = "v%.1f] - [%s";
constexpr const float editor_version = 1.1f;

constexpr uint32_t LIGHTMAP_MAX_RES = 512; // default 256

constexpr uint32_t LIGHTMAP_TEXTURE_PBYTES = 4;
constexpr uint32_t LIGHTMAP_TEXTURE_RES = 512;
constexpr uint32_t LIGHTMAP_TEXTURE_PIXEL_COUNT = LIGHTMAP_TEXTURE_RES * LIGHTMAP_TEXTURE_RES;
constexpr uint32_t LIGHTMAP_TEXTURE_BUFFER_SIZE = LIGHTMAP_TEXTURE_PIXEL_COUNT * LIGHTMAP_TEXTURE_PBYTES;


/*	===============
	Feature Toggles
	=============== */

#define LIGHTMAP_OVERRIDE_RESOLUTION
#define LIGHTMAP_DISABLE_DOWNSAMPLING

#if defined(_DEBUG)
	#define debug_cout std::cout << "DEBUG: "
	#define debug_wcout std::wcout << L"DEBUG: "
	#define debug_cerr std::cerr << "DEBUG: "
	#define debug_wcerr std::wcerr << L"DEBUG: "

#else
	#define debug_cout 0 && std::cout
	#define debug_wcout 0 && std::wcout
	#define debug_cerr 0 && std::cerr
	#define debug_wcerr 0 && std::wcerr
#endif

// add headers that you want to pre-compile here
#include "framework.h"

#endif //PCH_H



#ifndef DLL_API
#define DLL_API __declspec(dllexport)
extern "C" DLL_API void NULL_Function() {
}
#endif
