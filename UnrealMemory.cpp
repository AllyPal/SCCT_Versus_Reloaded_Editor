#include "pch.h"
#include "UnrealMemory.h"
#include <iostream>

typedef void* (*malloc_t)(size_t);
typedef void (*free_t)(void*);

void UnrealMemory::free(void* ptr) {
    return (*reinterpret_cast<free_t*>(0x10BDF2C8))(ptr);
}

void* UnrealMemory::malloc(size_t size) {
    return (*reinterpret_cast<malloc_t*>(0x10BDF2C4))(size);
}

wchar_t* UnrealMemory::mallocUString(std::wstring text) {
    wchar_t* heapText = (wchar_t*)UnrealMemory::malloc((text.size() + 1) * sizeof(wchar_t));
    if (heapText == nullptr) {
        throw std::exception("Failed to allocate memory.");
    }
    std::copy(text.begin(), text.end(), heapText);
    heapText[text.size()] = L'\0';
    return heapText;
}
