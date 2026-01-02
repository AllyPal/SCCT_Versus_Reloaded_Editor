#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>
#include "pch.h"
#include <vector>

class ShadowMapFilter
{
public:
    static void ProcessLightmap(void* srcBuffer, void* destBuffer) {
        //NoBlur(srcBuffer, destBuffer);
        //BoxBlur(srcBuffer, destBuffer, 3);
        //RootMeanSquareBlur(srcBuffer, destBuffer, 3);
        //GaussianBlur(srcBuffer, destBuffer, 3);
        GaussianBlurWithBandDithering(srcBuffer, destBuffer, 3, 10);
    }

private:
    static inline float fixRoundingError = 0.5f;
    // simple copy - some lines will look jagged
    static void NoBlur(void* srcBuffer, void* destBuffer) {
        memcpy_s(destBuffer, LIGHTMAP_TEXTURE_BUFFER_SIZE, srcBuffer, LIGHTMAP_TEXTURE_BUFFER_SIZE);
    }

    static void BoxBlur(void* srcBuffer, void* destBuffer, int kernelSize) {
        auto* dest = static_cast<uint8_t*>(destBuffer);
        const auto* src = static_cast<const uint8_t*>(srcBuffer);

        const int res = LIGHTMAP_TEXTURE_RES;
        const int stride = res * 4;
        const int half = kernelSize / 2;
        const int divisor = kernelSize * kernelSize;

        auto getPixel = [&](int x, int y, int offset) -> int {
            x = std::clamp(x, 0, res - 1);
            y = std::clamp(y, 0, res - 1);
            return src[(y * stride) + (x * 4) + offset];
            };

        for (int y = 0; y < res; ++y) {
            for (int x = 0; x < res; ++x) {
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

    static void RootMeanSquareBlur(void* srcBuffer, void* destBuffer, int kernelSize) {
        auto* dest = static_cast<uint8_t*>(destBuffer);
        const auto* src = static_cast<const uint8_t*>(srcBuffer);

        const int res = LIGHTMAP_TEXTURE_RES;
        const int stride = res * 4;
        const int half = kernelSize / 2;

        for (int y = 0; y < res; ++y) {
            for (int x = 0; x < res; ++x) {
                int a = 0, r = 0, g = 0, b = 0;
                int samples = 0;

                for (int ky = -half; ky <= half; ++ky) {
                    int sy = y + ky;
                    if (sy >= 0 && sy < res) {
                        for (int kx = -half; kx <= half; ++kx) {
                            int sx = x + kx;
                            if (sx >= 0 && sx < res) {
                                int idx = (sy * stride) + (sx * 4);

                                if (src[idx + 3] > 0) {
                                    b += src[idx + 0] * src[idx + 0];
                                    g += src[idx + 1] * src[idx + 1];
                                    r += src[idx + 2] * src[idx + 2];
                                    a += src[idx + 3] * src[idx + 3];
                                    samples++;
                                }
                            }
                        }
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

    static void GaussianBlur(void* srcBuffer, void* destBuffer, int kernelSize) {
        auto* dest = static_cast<uint8_t*>(destBuffer);
        const auto* src = static_cast<const uint8_t*>(srcBuffer);

        const int res = LIGHTMAP_TEXTURE_RES;
        const int stride = res * 4;

        const int blurRadius = kernelSize / 2;
        const int blurRadiusSq = blurRadius * blurRadius;
        const float sigma = max(1.0f, kernelSize / 3.0f);
        const float twoSigmaSq = 2.0f * sigma * sigma;

        for (int y = 0; y < res; ++y) {
            for (int x = 0; x < res; ++x) {
                float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
                float totalWeight = 0.0f;

                for (int ky = -blurRadius; ky <= blurRadius; ++ky) {
                    for (int kx = -blurRadius; kx <= blurRadius; ++kx) {
                        int distSq = kx * kx + ky * ky;

                        if (distSq > blurRadiusSq) continue;

                        int sy = y + ky;
                        int sx = x + kx;

                        if (sy >= 0 && sy < res && sx >= 0 && sx < res) {
                            int idx = (sy * stride) + (sx * 4);

                            if (src[idx + 3] > 0) {
                                float weight = std::exp(-static_cast<float>(distSq) / twoSigmaSq);

                                b += src[idx + 0] * weight;
                                g += src[idx + 1] * weight;
                                r += src[idx + 2] * weight;
                                a += src[idx + 3] * weight;
                                totalWeight += weight;
                            }
                        }
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

    static float CalculateBandingFactor(int uniformityRadius, int y, int x, const int res, const int stride, const uint8_t* src)
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

                if (sy >= 0 && sy < res && sx >= 0 && sx < res) {
                    int idx = (sy * stride) + (sx * 4);
                    if (src[idx + 3] > 0) {
                        float intensity = (src[idx + 0] + src[idx + 1] + src[idx + 2]) / 3.0f;
                        sumVal += intensity;
                        sumSqVal += intensity * intensity;
                        count++;
                    }
                }
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

    static void GaussianBlurWithBandDithering(void* srcBuffer, void* destBuffer, int kernelSize, int uniformityRadius) {
        auto* dest = static_cast<uint8_t*>(destBuffer);
        const auto* src = static_cast<const uint8_t*>(srcBuffer);

        const int res = LIGHTMAP_TEXTURE_RES;
        const int stride = res * 4;

        const int blurRadius = kernelSize / 2;
        const int blurRadiusSq = blurRadius * blurRadius;
        const float sigma = max(1.0f, kernelSize / 3.0f);
        const float twoSigmaSq = 2.0f * sigma * sigma;

        for (int y = 0; y < res; ++y) {
            for (int x = 0; x < res; ++x) {
                float bandingFactor = CalculateBandingFactor(uniformityRadius, y, x, res, stride, src);

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

                        if (sy >= 0 && sy < res && sx >= 0 && sx < res) {
                            int idx = (sy * stride) + (sx * 4);

                            if (src[idx + 3] > 0) {
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
                    }
                }

                int destIndex = (y * stride) + (x * 4);

                if (totalWeight > 0.0f) {
                    float invWeight = 1.0f / totalWeight;
                    float finalB = b * invWeight;
                    float finalG = g * invWeight;
                    float finalR = r * invWeight;
                    float finalA = a * invWeight;

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
        }
    }
};

