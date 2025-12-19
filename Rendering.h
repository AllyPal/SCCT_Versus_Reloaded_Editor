#pragma once
#include <Windows.h>
#include <chrono>
#include "GameStructs.h"

class Rendering
{
public:
	static void Initialize();
    static bool IsWine();
};

#define D3DX_PI    (3.14159265358979323846)
const double degToRadConversionFactor = D3DX_PI / 180.0;
const double radToDegConversionFactor = 180.0 / D3DX_PI;
