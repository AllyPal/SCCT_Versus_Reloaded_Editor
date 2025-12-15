#pragma once
#include <iostream>

class UnrealMemory
{
public:
	static void free(void* ptr);
	static void* malloc(size_t size);
	static wchar_t* mallocUString(std::wstring text);
};
