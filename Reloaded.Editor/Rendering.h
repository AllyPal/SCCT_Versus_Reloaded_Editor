#pragma once
#include <Windows.h>
#include <chrono>
#include "GameStructs.h"

struct IDirect3DDevice9;   // forward declaration (full type lives in d3d9.h)

class Rendering
{
public:
	static void Initialize();
    static bool IsWine();

    // The live D3D9 device behind d3d8to9. Null until the device has been
    // created. Exposed so other modules (NormalMaps) can build D3D9
    // resources such as shaders.
    static IDirect3DDevice9* GetDevice9();
};

#define D3DX_PI    (3.14159265358979323846)
const double degToRadConversionFactor = D3DX_PI / 180.0;
const double radToDegConversionFactor = 180.0 / D3DX_PI;
