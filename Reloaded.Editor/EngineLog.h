#pragma once

class EngineLog
{
public:
    static void Initialize();

    // Second destination for engine errors and warnings, on top of the Reloaded log.
    typedef void (__cdecl *Sink)(const char* line);
    static void SetSink(Sink sink);
};
