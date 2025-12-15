#pragma once
#include <string>
#include <Windows.h>
#include "UnrealMemory.h"
#include <unordered_map>
#include <bit>

struct BoolProxy {
    uint32_t* value;
    uint32_t mask;

    operator bool() const {
        return (*value & mask) != 0;
    }

    BoolProxy& operator=(bool enabled) {
        if (enabled) {
            *value |= mask;
        }
        else {
            *value &= ~mask;
        }
        return *this;
    }
};

struct UObject {
    std::byte unspecified[0x1000];

    BoolProxy flag(uint32_t offset, uint32_t mask) {
        return BoolProxy{ reinterpret_cast<uint32_t*>(unspecified + offset), mask };
    }
};

struct UViewport : UObject {
    int32_t& HitX() {
        //IntProperty - 2 - 212957
        return *reinterpret_cast<int32_t*>(unspecified + (0x17C));
    }

    int32_t& HitY() {
        //IntProperty - 2 - 212957
        return *reinterpret_cast<int32_t*>(unspecified + (0x180));
    }

    int32_t& HitXL() {
        //IntProperty - 2 - 212957
        return *reinterpret_cast<int32_t*>(unspecified + (0x184));
    }

    int32_t& HitYL() {
        //IntProperty - 2 - 212957
        return *reinterpret_cast<int32_t*>(unspecified + (0x188));
    }
};


struct Actor : UObject {
    float& CullDistance() {
        //FloatProperty - 4 - 1290017
        return *reinterpret_cast<float*>(unspecified + (0x254));
    }
};
