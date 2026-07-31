#pragma once

class StaticMeshCollisionFix
{
public:
    static void Initialize();

    // True if a static mesh built during this session was too complex for the
    // 16-bit collision node index and had its collision tree truncated.
    static bool WasCollisionTruncated();
};
