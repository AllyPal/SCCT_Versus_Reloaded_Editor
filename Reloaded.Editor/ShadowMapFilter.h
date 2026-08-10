#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <execution>
#include <numeric>
#include "pch.h"
#include <vector>

// Bounds of one lightmap in atlas texture space.
struct LightmapRect
{
    int x, y, w, h;

    int MaxX() const { return x + w - 1; }
    int MaxY() const { return y + h - 1; }

    bool Contains(int px, int py) const {
        return px >= x && px <= MaxX() && py >= y && py <= MaxY();
    }
};

class ShadowMapFilter
{
public:
    // Each packed lightmap is filtered on its own; a kernel reaching over a border
    // would bleed neighbouring lightmaps into each other.
    static void ProcessLightmapAtlas(void* srcBuffer, void* destBuffer, const std::vector<LightmapRect>& rects) {
        // The atlas is an uninitialised malloc and the gaps between lightmaps are
        // never written below.
        memset(destBuffer, 0, LIGHTMAP_TEXTURE_BUFFER_SIZE);

        if (rects.empty()) {
            ProcessLightmap(srcBuffer, destBuffer, LightmapRect{ 0, 0, atlasRes, atlasRes });
            return;
        }

        for (const LightmapRect& rect : rects) {
            ProcessLightmap(srcBuffer, destBuffer, rect);
        }
    }

    static void ProcessLightmap(void* srcBuffer, void* destBuffer, const LightmapRect& rect) {
        //NoBlur(srcBuffer, destBuffer, rect);
        //BoxBlur(srcBuffer, destBuffer, rect, 3);
        //RootMeanSquareBlur(srcBuffer, destBuffer, rect, 3);
        //GaussianBlur(srcBuffer, destBuffer, rect, 3);
        GaussianBlurWithBandDithering(srcBuffer, destBuffer, rect, 3, 10);
    }

private:
    static constexpr int atlasRes = static_cast<int>(LIGHTMAP_TEXTURE_RES);
    static constexpr int stride = atlasRes * 4;
    static inline float fixRoundingError = 0.5f;

    static constexpr int bandingGridStep = 4;

    // Below this a rect costs more to hand to the thread pool than to walk.
    static constexpr int parallelPixelThreshold = 4096;

    struct BandingGrid
    {
        std::vector<float> cells;
        int w = 0;
        int h = 0;
    };

    // Rows never overlap: each reads src only and writes only its own texels.
    template<typename Body>
    static void ForEachRow(int firstRow, int rowCount, const LightmapRect& rect, Body body) {
        if (rowCount <= 0) return;

        if (rect.w * rect.h < parallelPixelThreshold) {
            for (int i = 0; i < rowCount; ++i) {
                body(firstRow + i);
            }
            return;
        }

        std::vector<int> rows(rowCount);
        std::iota(rows.begin(), rows.end(), firstRow);
        std::for_each(std::execution::par, rows.begin(), rows.end(), body);
    }

    // simple copy - some lines will look jagged
    static void NoBlur(void* srcBuffer, void* destBuffer, const LightmapRect& rect) {
        auto* dest = static_cast<uint8_t*>(destBuffer);
        const auto* src = static_cast<const uint8_t*>(srcBuffer);

        for (int y = rect.y; y <= rect.MaxY(); ++y) {
            const int rowIndex = (y * stride) + (rect.x * 4);
            memcpy(dest + rowIndex, src + rowIndex, static_cast<size_t>(rect.w) * 4);
        }
    }

    static void BoxBlur(void* srcBuffer, void* destBuffer, const LightmapRect& rect, int kernelSize) {
        auto* dest = static_cast<uint8_t*>(destBuffer);
        const auto* src = static_cast<const uint8_t*>(srcBuffer);

        const int half = kernelSize / 2;
        const int divisor = kernelSize * kernelSize;

        auto getPixel = [&](int x, int y, int offset) -> int {
            x = std::clamp(x, rect.x, rect.MaxX());
            y = std::clamp(y, rect.y, rect.MaxY());
            return src[(y * stride) + (x * 4) + offset];
            };

        for (int y = rect.y; y <= rect.MaxY(); ++y) {
            for (int x = rect.x; x <= rect.MaxX(); ++x) {
                int a = 0, r = 0, g = 0, b = 0;

                for (int ky = -half; ky <= half; ++ky) {
                    for (int kx = -half; kx <= half; ++kx) {
                        b += getPixel(x + kx, y + ky, 0);
                        g += getPixel(x + kx, y + ky, 1);
                        r += getPixel(x + kx, y + ky, 2);
                        a += getPixel(x + kx, y + ky, 3);
                    }
                }

                int destIndex = (y * stride) + (x * 4);
                dest[destIndex + 0] = static_cast<uint8_t>((b / divisor) + fixRoundingError);
                dest[destIndex + 1] = static_cast<uint8_t>((g / divisor) + fixRoundingError);
                dest[destIndex + 2] = static_cast<uint8_t>((r / divisor) + fixRoundingError);
                dest[destIndex + 3] = static_cast<uint8_t>((a / divisor) + fixRoundingError);
            }
        }
    }

    static void RootMeanSquareBlur(void* srcBuffer, void* destBuffer, const LightmapRect& rect, int kernelSize) {
        auto* dest = static_cast<uint8_t*>(destBuffer);
        const auto* src = static_cast<const uint8_t*>(srcBuffer);

        const int half = kernelSize / 2;

        for (int y = rect.y; y <= rect.MaxY(); ++y) {
            for (int x = rect.x; x <= rect.MaxX(); ++x) {
                int a = 0, r = 0, g = 0, b = 0;
                int samples = 0;

                for (int ky = -half; ky <= half; ++ky) {
                    for (int kx = -half; kx <= half; ++kx) {
                        int sy = y + ky;
                        int sx = x + kx;

                        if (!rect.Contains(sx, sy)) continue;

                        int idx = (sy * stride) + (sx * 4);
                        if (src[idx + 3] == 0) continue;

                        b += src[idx + 0] * src[idx + 0];
                        g += src[idx + 1] * src[idx + 1];
                        r += src[idx + 2] * src[idx + 2];
                        a += src[idx + 3] * src[idx + 3];
                        samples++;
                    }
                }

                int destIndex = (y * stride) + (x * 4);
                if (samples > 0) {
                    dest[destIndex + 0] = static_cast<uint8_t>((std::sqrt(b / samples)) + fixRoundingError);
                    dest[destIndex + 1] = static_cast<uint8_t>((std::sqrt(g / samples)) + fixRoundingError);
                    dest[destIndex + 2] = static_cast<uint8_t>((std::sqrt(r / samples)) + fixRoundingError);
                    dest[destIndex + 3] = static_cast<uint8_t>((std::sqrt(a / samples)) + fixRoundingError);
                }
            }
        }
    }

    static void GaussianBlur(void* srcBuffer, void* destBuffer, const LightmapRect& rect, int kernelSize) {
        auto* dest = static_cast<uint8_t*>(destBuffer);
        const auto* src = static_cast<const uint8_t*>(srcBuffer);

        const int blurRadius = kernelSize / 2;
        const int blurRadiusSq = blurRadius * blurRadius;
        const float sigma = max(1.0f, kernelSize / 3.0f);
        const float twoSigmaSq = 2.0f * sigma * sigma;

        for (int y = rect.y; y <= rect.MaxY(); ++y) {
            for (int x = rect.x; x <= rect.MaxX(); ++x) {
                float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
                float totalWeight = 0.0f;

                for (int ky = -blurRadius; ky <= blurRadius; ++ky) {
                    for (int kx = -blurRadius; kx <= blurRadius; ++kx) {
                        int distSq = kx * kx + ky * ky;

                        if (distSq > blurRadiusSq) continue;

                        int sy = y + ky;
                        int sx = x + kx;

                        if (!rect.Contains(sx, sy)) continue;

                        int idx = (sy * stride) + (sx * 4);
                        if (src[idx + 3] == 0) continue;

                        float weight = std::exp(-static_cast<float>(distSq) / twoSigmaSq);

                        b += src[idx + 0] * weight;
                        g += src[idx + 1] * weight;
                        r += src[idx + 2] * weight;
                        a += src[idx + 3] * weight;
                        totalWeight += weight;
                    }
                }

                int destIndex = (y * stride) + (x * 4);

                if (totalWeight > 0.0f) {
                    float invWeight = 1.0f / totalWeight;

                    dest[destIndex + 0] = static_cast<uint8_t>(std::clamp((b * invWeight) + fixRoundingError, 0.0f, 255.0f));
                    dest[destIndex + 1] = static_cast<uint8_t>(std::clamp((g * invWeight) + fixRoundingError, 0.0f, 255.0f));
                    dest[destIndex + 2] = static_cast<uint8_t>(std::clamp((r * invWeight) + fixRoundingError, 0.0f, 255.0f));
                    dest[destIndex + 3] = static_cast<uint8_t>(std::clamp((a * invWeight) + fixRoundingError, 0.0f, 255.0f));
                }
                else {
                    *(int*)&dest[destIndex] = 0;
                }
            }
        }
    }

    static float GetWhiteNoise(int x, int y) {
        float dot = x * 12.9898f + y * 78.233f;
        float sinVal = std::sin(dot) * 43758.5453f;
        return sinVal - std::floor(sinVal);
    }

    static float CalculateBandingFactor(int uniformityRadius, int y, int x, const LightmapRect& rect, const uint8_t* src)
    {
        float sumVal = 0.0f;
        float sumSqVal = 0.0f;
        int count = 0;
        const int uniformityRadiusSq = uniformityRadius * uniformityRadius;

        for (int uy = -uniformityRadius; uy <= uniformityRadius; ++uy) {
            for (int ux = -uniformityRadius; ux <= uniformityRadius; ++ux) {
                if (ux * ux + uy * uy > uniformityRadiusSq) continue;

                int sy = y + uy;
                int sx = x + ux;

                if (!rect.Contains(sx, sy)) continue;

                int idx = (sy * stride) + (sx * 4);
                if (src[idx + 3] == 0) continue;

                float intensity = (src[idx + 0] + src[idx + 1] + src[idx + 2]) / 3.0f;
                sumVal += intensity;
                sumSqVal += intensity * intensity;
                count++;
            }
        }

        if (count <= 1) return 0.0f;

        float mean = sumVal / count;
        float variance = (sumSqVal / count) - (mean * mean);
        float stdDev = std::sqrt(max(0.0f, variance));

        const float minBandingDev = 0.1f;
        const float maxBandingDev = 10.0f;

        if (stdDev < minBandingDev) return 0.0f;
        if (stdDev > maxBandingDev) return 0.0f;

        float factor = 1.0f - ((stdDev - minBandingDev) / (maxBandingDev - minBandingDev));
        return std::clamp(factor, 0.0f, 1.0f);
    }

    // Texels a few apart share almost all of this statistic's samples, so sampling it on
    // a grid and interpolating is visually identical for bandingGridStep^2 less work.
    static BandingGrid BuildBandingGrid(int uniformityRadius, const LightmapRect& rect, const uint8_t* src) {
        BandingGrid grid;
        grid.w = ((rect.w - 1) / bandingGridStep) + 2;
        grid.h = ((rect.h - 1) / bandingGridStep) + 2;
        grid.cells.resize(static_cast<size_t>(grid.w) * grid.h);

        ForEachRow(0, grid.h, rect, [&](int gy) {
            const int y = min(rect.y + (gy * bandingGridStep), rect.MaxY());
            float* row = grid.cells.data() + (static_cast<size_t>(gy) * grid.w);

            for (int gx = 0; gx < grid.w; ++gx) {
                const int x = min(rect.x + (gx * bandingGridStep), rect.MaxX());
                row[gx] = CalculateBandingFactor(uniformityRadius, y, x, rect, src);
            }
            });

        return grid;
    }

    static float SampleBandingGrid(const BandingGrid& grid, const LightmapRect& rect, int x, int y) {
        const int dx = x - rect.x;
        const int dy = y - rect.y;
        const int gx = dx / bandingGridStep;
        const int gy = dy / bandingGridStep;
        const float tx = static_cast<float>(dx % bandingGridStep) / bandingGridStep;
        const float ty = static_cast<float>(dy % bandingGridStep) / bandingGridStep;

        const float* cells = grid.cells.data();
        const float* row0 = cells + (static_cast<size_t>(gy) * grid.w);
        const float* row1 = cells + (static_cast<size_t>(min(gy + 1, grid.h - 1)) * grid.w);
        const int gx1 = min(gx + 1, grid.w - 1);

        const float top = row0[gx] + ((row0[gx1] - row0[gx]) * tx);
        const float bottom = row1[gx] + ((row1[gx1] - row1[gx]) * tx);
        return top + ((bottom - top) * ty);
    }

    static void GaussianBlurWithBandDithering(void* srcBuffer, void* destBuffer, const LightmapRect& rect, int kernelSize, int uniformityRadius) {
        auto* dest = static_cast<uint8_t*>(destBuffer);
        const auto* src = static_cast<const uint8_t*>(srcBuffer);

        const int blurRadius = kernelSize / 2;
        const int blurRadiusSq = blurRadius * blurRadius;
        const float sigma = max(1.0f, kernelSize / 3.0f);
        const float twoSigmaSq = 2.0f * sigma * sigma;

        const BandingGrid banding = BuildBandingGrid(uniformityRadius, rect, src);

        ForEachRow(rect.y, rect.h, rect, [&](int y) {
            for (int x = rect.x; x <= rect.MaxX(); ++x) {
                float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
                float totalWeight = 0.0f;

                uint8_t minR = 255, minG = 255, minB = 255;
                uint8_t maxR = 0, maxG = 0, maxB = 0;

                for (int ky = -blurRadius; ky <= blurRadius; ++ky) {
                    for (int kx = -blurRadius; kx <= blurRadius; ++kx) {
                        int distSq = kx * kx + ky * ky;

                        if (distSq > blurRadiusSq) continue;

                        int sy = y + ky;
                        int sx = x + kx;

                        if (!rect.Contains(sx, sy)) continue;

                        int idx = (sy * stride) + (sx * 4);
                        if (src[idx + 3] == 0) continue;

                        uint8_t sr = src[idx + 2];
                        uint8_t sg = src[idx + 1];
                        uint8_t sb = src[idx + 0];

                        minR = min(minR, sr); maxR = max(maxR, sr);
                        minG = min(minG, sg); maxG = max(maxG, sg);
                        minB = min(minB, sb); maxB = max(maxB, sb);

                        float weight = std::exp(-static_cast<float>(distSq) / twoSigmaSq);

                        b += sb * weight;
                        g += sg * weight;
                        r += sr * weight;
                        a += src[idx + 3] * weight;
                        totalWeight += weight;
                    }
                }

                int destIndex = (y * stride) + (x * 4);

                if (totalWeight > 0.0f) {
                    float invWeight = 1.0f / totalWeight;
                    float finalB = b * invWeight;
                    float finalG = g * invWeight;
                    float finalR = r * invWeight;
                    float finalA = a * invWeight;

                    // Hoisting this out of the branch would pay for texels that discard it.
                    float bandingFactor = SampleBandingGrid(banding, rect, x, y);

                    if (bandingFactor > 0.0f) {
                        float noise = (GetWhiteNoise(x, y) - 0.5f);
                        float ditherVal = noise * 4.0f * bandingFactor;

                        finalB += ditherVal;
                        finalG += ditherVal;
                        finalR += ditherVal;
                    }

                    dest[destIndex + 0] = static_cast<uint8_t>(std::clamp(finalB + fixRoundingError, (float)minB, (float)maxB));
                    dest[destIndex + 1] = static_cast<uint8_t>(std::clamp(finalG + fixRoundingError, (float)minG, (float)maxG));
                    dest[destIndex + 2] = static_cast<uint8_t>(std::clamp(finalR + fixRoundingError, (float)minR, (float)maxR));
                    dest[destIndex + 3] = static_cast<uint8_t>(std::clamp(finalA + fixRoundingError, 0.0f, 255.0f));
                }
                else {
                    *(int*)&dest[destIndex] = 0;
                }
            }
            });
    }
};
