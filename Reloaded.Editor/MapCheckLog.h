#pragma once

class MapCheckLog
{
public:
    static void Initialize();

    // Set for the length of a bulk run: entries come here and the dialog stays shut.
    typedef void (__cdecl *Sink)(const char* line);
    static void SetSink(Sink sink);
};
