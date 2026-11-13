// SPDX-FileCopyrightText: 2022 Google LLC
// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: Apache-2.0
// Derived from Material Color Utilities' cpp/quantize/wu.cc, wsmeans.cc,
// lab.cc, celebi.cc and cpp/score/score.cc (commit 194144d); see
// common/color/LICENSE.
/*
Copyright 2022 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Changed for Vela, 2026: k-means' initial assignment from a generator of its
own (std::minstd_rand, upstream's seed) instead of the C library's global
rand(), which a library must not reseed and whose sequence differs between
C libraries; without k-means' random starting clusters, which Celebi's
method never asks for (it starts from Wu's); a std::unordered_map instead
of Abseil's; a stable sort of the scores, as the TypeScript implementation
(Array.prototype.sort) has; names and namespace of this library.
*/

#include "quantize.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>

namespace vela::color {

namespace {

// --- wu.cc ---

struct Box {
    int r0 = 0;
    int r1 = 0;
    int g0 = 0;
    int g1 = 0;
    int b0 = 0;
    int b1 = 0;
    int vol = 0;
};

enum class Direction { Red, Green, Blue };

constexpr int kIndexBits = 5;
constexpr int kIndexCount = ((1 << kIndexBits) + 1);
constexpr int kTotalSize = (kIndexCount * kIndexCount * kIndexCount);
constexpr int kMaxColors = 256;

using IntArray = std::vector<std::int64_t>;
using DoubleArray = std::vector<double>;

int redOf(Argb argb)
{
    return int((argb & 0x00ff0000) >> 16);
}

int greenOf(Argb argb)
{
    return int((argb & 0x0000ff00) >> 8);
}

int blueOf(Argb argb)
{
    return int(argb & 0x000000ff);
}

Argb argbFromRgb(int red, int green, int blue)
{
    return 0xFF000000u | (Argb(red & 0xff) << 16) | (Argb(green & 0xff) << 8) | Argb(blue & 0xff);
}

int getIndex(int r, int g, int b)
{
    return (r << (kIndexBits * 2)) + (r << (kIndexBits + 1)) + (g << kIndexBits) + r + g + b;
}

void constructHistogram(const std::vector<Argb>& pixels, IntArray& weights, IntArray& mR, IntArray& mG,
    IntArray& mB, DoubleArray& moments)
{
    for (const Argb pixel : pixels) {
        const int red = redOf(pixel);
        const int green = greenOf(pixel);
        const int blue = blueOf(pixel);
        const int bitsToRemove = 8 - kIndexBits;
        const int index = getIndex((red >> bitsToRemove) + 1, (green >> bitsToRemove) + 1, (blue >> bitsToRemove) + 1);
        weights[index]++;
        mR[index] += red;
        mG[index] += green;
        mB[index] += blue;
        moments[index] += (red * red) + (green * green) + (blue * blue);
    }
}

void computeMoments(IntArray& weights, IntArray& mR, IntArray& mG, IntArray& mB, DoubleArray& moments)
{
    for (int r = 1; r < kIndexCount; r++) {
        std::int64_t area[kIndexCount] = {};
        std::int64_t areaR[kIndexCount] = {};
        std::int64_t areaG[kIndexCount] = {};
        std::int64_t areaB[kIndexCount] = {};
        double area2[kIndexCount] = {};
        for (int g = 1; g < kIndexCount; g++) {
            std::int64_t line = 0;
            std::int64_t lineR = 0;
            std::int64_t lineG = 0;
            std::int64_t lineB = 0;
            double line2 = 0.0;
            for (int b = 1; b < kIndexCount; b++) {
                const int index = getIndex(r, g, b);
                line += weights[index];
                lineR += mR[index];
                lineG += mG[index];
                lineB += mB[index];
                line2 += moments[index];

                area[b] += line;
                areaR[b] += lineR;
                areaG[b] += lineG;
                areaB[b] += lineB;
                area2[b] += line2;

                const int previousIndex = getIndex(r - 1, g, b);
                weights[index] = weights[previousIndex] + area[b];
                mR[index] = mR[previousIndex] + areaR[b];
                mG[index] = mG[previousIndex] + areaG[b];
                mB[index] = mB[previousIndex] + areaB[b];
                moments[index] = moments[previousIndex] + area2[b];
            }
        }
    }
}

std::int64_t top(const Box& cube, Direction direction, int position, const IntArray& moment)
{
    switch (direction) {
    case Direction::Red:
        return moment[getIndex(position, cube.g1, cube.b1)] - moment[getIndex(position, cube.g1, cube.b0)]
            - moment[getIndex(position, cube.g0, cube.b1)] + moment[getIndex(position, cube.g0, cube.b0)];
    case Direction::Green:
        return moment[getIndex(cube.r1, position, cube.b1)] - moment[getIndex(cube.r1, position, cube.b0)]
            - moment[getIndex(cube.r0, position, cube.b1)] + moment[getIndex(cube.r0, position, cube.b0)];
    case Direction::Blue: break;
    }
    return moment[getIndex(cube.r1, cube.g1, position)] - moment[getIndex(cube.r1, cube.g0, position)]
        - moment[getIndex(cube.r0, cube.g1, position)] + moment[getIndex(cube.r0, cube.g0, position)];
}

std::int64_t bottom(const Box& cube, Direction direction, const IntArray& moment)
{
    switch (direction) {
    case Direction::Red:
        return -moment[getIndex(cube.r0, cube.g1, cube.b1)] + moment[getIndex(cube.r0, cube.g1, cube.b0)]
            + moment[getIndex(cube.r0, cube.g0, cube.b1)] - moment[getIndex(cube.r0, cube.g0, cube.b0)];
    case Direction::Green:
        return -moment[getIndex(cube.r1, cube.g0, cube.b1)] + moment[getIndex(cube.r1, cube.g0, cube.b0)]
            + moment[getIndex(cube.r0, cube.g0, cube.b1)] - moment[getIndex(cube.r0, cube.g0, cube.b0)];
    case Direction::Blue: break;
    }
    return -moment[getIndex(cube.r1, cube.g1, cube.b0)] + moment[getIndex(cube.r1, cube.g0, cube.b0)]
        + moment[getIndex(cube.r0, cube.g1, cube.b0)] - moment[getIndex(cube.r0, cube.g0, cube.b0)];
}

template <typename Moments> auto vol(const Box& cube, const Moments& moment)
{
    return moment[getIndex(cube.r1, cube.g1, cube.b1)] - moment[getIndex(cube.r1, cube.g1, cube.b0)]
        - moment[getIndex(cube.r1, cube.g0, cube.b1)] + moment[getIndex(cube.r1, cube.g0, cube.b0)]
        - moment[getIndex(cube.r0, cube.g1, cube.b1)] + moment[getIndex(cube.r0, cube.g1, cube.b0)]
        + moment[getIndex(cube.r0, cube.g0, cube.b1)] - moment[getIndex(cube.r0, cube.g0, cube.b0)];
}

double variance(const Box& cube, const IntArray& weights, const IntArray& mR, const IntArray& mG,
    const IntArray& mB, const DoubleArray& moments)
{
    const double dr = double(vol(cube, mR));
    const double dg = double(vol(cube, mG));
    const double db = double(vol(cube, mB));
    const double xx = vol(cube, moments);
    const double hypotenuse = dr * dr + dg * dg + db * db;
    const double volume = double(vol(cube, weights));
    return xx - hypotenuse / volume;
}

double maximize(const Box& cube, Direction direction, int first, int last, int* cut, std::int64_t wholeW,
    std::int64_t wholeR, std::int64_t wholeG, std::int64_t wholeB, const IntArray& weights, const IntArray& mR,
    const IntArray& mG, const IntArray& mB)
{
    const std::int64_t bottomR = bottom(cube, direction, mR);
    const std::int64_t bottomG = bottom(cube, direction, mG);
    const std::int64_t bottomB = bottom(cube, direction, mB);
    const std::int64_t bottomW = bottom(cube, direction, weights);

    double max = 0.0;
    *cut = -1;
    for (int i = first; i < last; i++) {
        std::int64_t halfR = bottomR + top(cube, direction, i, mR);
        std::int64_t halfG = bottomG + top(cube, direction, i, mG);
        std::int64_t halfB = bottomB + top(cube, direction, i, mB);
        std::int64_t halfW = bottomW + top(cube, direction, i, weights);
        if (halfW == 0) {
            continue;
        }
        double temp = (double(halfR) * halfR + double(halfG) * halfG + double(halfB) * halfB) / double(halfW);
        halfR = wholeR - halfR;
        halfG = wholeG - halfG;
        halfB = wholeB - halfB;
        halfW = wholeW - halfW;
        if (halfW == 0) {
            continue;
        }
        temp += (double(halfR) * halfR + double(halfG) * halfG + double(halfB) * halfB) / double(halfW);
        if (temp > max) {
            max = temp;
            *cut = i;
        }
    }
    return max;
}

bool cut(Box& box1, Box& box2, const IntArray& weights, const IntArray& mR, const IntArray& mG, const IntArray& mB)
{
    const std::int64_t wholeR = vol(box1, mR);
    const std::int64_t wholeG = vol(box1, mG);
    const std::int64_t wholeB = vol(box1, mB);
    const std::int64_t wholeW = vol(box1, weights);

    int cutR, cutG, cutB;
    const double maxR = maximize(box1, Direction::Red, box1.r0 + 1, box1.r1, &cutR, wholeW, wholeR, wholeG,
        wholeB, weights, mR, mG, mB);
    const double maxG = maximize(box1, Direction::Green, box1.g0 + 1, box1.g1, &cutG, wholeW, wholeR, wholeG,
        wholeB, weights, mR, mG, mB);
    const double maxB = maximize(box1, Direction::Blue, box1.b0 + 1, box1.b1, &cutB, wholeW, wholeR, wholeG,
        wholeB, weights, mR, mG, mB);

    Direction direction;
    if (maxR >= maxG && maxR >= maxB) {
        direction = Direction::Red;
        if (cutR < 0) {
            return false;
        }
    } else if (maxG >= maxR && maxG >= maxB) {
        direction = Direction::Green;
    } else {
        direction = Direction::Blue;
    }

    box2.r1 = box1.r1;
    box2.g1 = box1.g1;
    box2.b1 = box1.b1;
    if (direction == Direction::Red) {
        box2.r0 = box1.r1 = cutR;
        box2.g0 = box1.g0;
        box2.b0 = box1.b0;
    } else if (direction == Direction::Green) {
        box2.r0 = box1.r0;
        box2.g0 = box1.g1 = cutG;
        box2.b0 = box1.b0;
    } else {
        box2.r0 = box1.r0;
        box2.g0 = box1.g0;
        box2.b0 = box1.b1 = cutB;
    }
    box1.vol = (box1.r1 - box1.r0) * (box1.g1 - box1.g0) * (box1.b1 - box1.b0);
    box2.vol = (box2.r1 - box2.r0) * (box2.g1 - box2.g0) * (box2.b1 - box2.b0);
    return true;
}

std::vector<Argb> quantizeWu(const std::vector<Argb>& pixels, int maxColors)
{
    if (maxColors <= 0 || maxColors > 256 || pixels.empty()) {
        return {};
    }
    IntArray weights(kTotalSize, 0);
    IntArray momentsRed(kTotalSize, 0);
    IntArray momentsGreen(kTotalSize, 0);
    IntArray momentsBlue(kTotalSize, 0);
    DoubleArray moments(kTotalSize, 0.0);
    constructHistogram(pixels, weights, momentsRed, momentsGreen, momentsBlue, moments);
    computeMoments(weights, momentsRed, momentsGreen, momentsBlue, moments);

    std::vector<Box> cubes(kMaxColors);
    cubes[0].r0 = cubes[0].g0 = cubes[0].b0 = 0;
    cubes[0].r1 = cubes[0].g1 = cubes[0].b1 = kIndexCount - 1;

    std::vector<double> volumeVariance(kMaxColors);
    int next = 0;
    for (int i = 1; i < maxColors; ++i) {
        if (cut(cubes[next], cubes[i], weights, momentsRed, momentsGreen, momentsBlue)) {
            volumeVariance[next] = cubes[next].vol > 1
                ? variance(cubes[next], weights, momentsRed, momentsGreen, momentsBlue, moments)
                : 0.0;
            volumeVariance[i]
                = cubes[i].vol > 1 ? variance(cubes[i], weights, momentsRed, momentsGreen, momentsBlue, moments) : 0.0;
        } else {
            volumeVariance[next] = 0.0;
            i--;
        }
        next = 0;
        double temp = volumeVariance[0];
        for (int j = 1; j <= i; j++) {
            if (volumeVariance[j] > temp) {
                temp = volumeVariance[j];
                next = j;
            }
        }
        if (temp <= 0.0) {
            maxColors = i + 1;
            break;
        }
    }

    std::vector<Argb> outColors;
    for (int i = 0; i < maxColors; ++i) {
        const std::int64_t weight = vol(cubes[i], weights);
        if (weight > 0) {
            const auto red = std::int32_t(vol(cubes[i], momentsRed) / weight);
            const auto green = std::int32_t(vol(cubes[i], momentsGreen) / weight);
            const auto blue = std::int32_t(vol(cubes[i], momentsBlue) / weight);
            outColors.push_back(argbFromRgb(red, green, blue));
        }
    }
    return outColors;
}

// --- lab.cc ---

constexpr double kWhitePointD65[] = { 95.047, 100.0, 108.883 };

struct Lab {
    double l = 0.0;
    double a = 0.0;
    double b = 0.0;

    // The squared distance: enough to compare.
    double deltaE(const Lab& lab) const
    {
        const double dL = l - lab.l;
        const double dA = a - lab.a;
        const double dB = b - lab.b;
        return (dL * dL) + (dA * dA) + (dB * dB);
    }
};

Argb argbFromLab(const Lab lab)
{
    const double e = 216.0 / 24389.0;
    const double kappa = 24389.0 / 27.0;
    const double ke = 8.0;

    const double fy = (lab.l + 16.0) / 116.0;
    const double fx = (lab.a / 500.0) + fy;
    const double fz = fy - (lab.b / 200.0);
    const double fx3 = fx * fx * fx;
    const double xNormalized = (fx3 > e) ? fx3 : (116.0 * fx - 16.0) / kappa;
    const double yNormalized = (lab.l > ke) ? fy * fy * fy : (lab.l / kappa);
    const double fz3 = fz * fz * fz;
    const double zNormalized = (fz3 > e) ? fz3 : (116.0 * fz - 16.0) / kappa;
    const double x = xNormalized * kWhitePointD65[0];
    const double y = yNormalized * kWhitePointD65[1];
    const double z = zNormalized * kWhitePointD65[2];

    // intFromXyz
    const double rL = 3.2406 * x - 1.5372 * y - 0.4986 * z;
    const double gL = -0.9689 * x + 1.8758 * y + 0.0415 * z;
    const double bL = 0.0557 * x - 0.2040 * y + 1.0570 * z;
    return argbFromRgb(detail::delinearized(rL), detail::delinearized(gL), detail::delinearized(bL));
}

Lab labFromArgb(const Argb argb)
{
    const double redL = detail::linearized(redOf(argb));
    const double greenL = detail::linearized(greenOf(argb));
    const double blueL = detail::linearized(blueOf(argb));
    const double x = 0.41233895 * redL + 0.35762064 * greenL + 0.18051042 * blueL;
    const double y = 0.2126 * redL + 0.7152 * greenL + 0.0722 * blueL;
    const double z = 0.01932141 * redL + 0.11916382 * greenL + 0.95034478 * blueL;
    const double e = 216.0 / 24389.0;
    const double kappa = 24389.0 / 27.0;
    const auto f = [&](double normalized) {
        return normalized > e ? std::pow(normalized, 1.0 / 3.0) : (kappa * normalized + 16) / 116;
    };
    const double fy = f(y / kWhitePointD65[1]);
    const double fx = f(x / kWhitePointD65[0]);
    const double fz = f(z / kWhitePointD65[2]);
    return { 116.0 * fy - 16, 500.0 * (fx - fy), 200.0 * (fy - fz) };
}

// --- wsmeans.cc ---

constexpr int kMaxIterations = 100;
constexpr double kMinDeltaE = 3.0;

struct DistanceToIndex {
    double distance = 0.0;
    int index = 0;

    bool operator<(const DistanceToIndex& a) const { return distance < a.distance; }
};

std::map<Argb, std::uint32_t> quantizeWsmeans(
    const std::vector<Argb>& inputPixels, const std::vector<Argb>& startingClusters, int maxColors)
{
    if (maxColors <= 0 || inputPixels.empty() || startingClusters.empty()) {
        return {};
    }
    maxColors = std::min(maxColors, 256);

    std::unordered_map<Argb, int> pixelToCount;
    std::vector<Argb> pixels;
    std::vector<Lab> points;
    for (const Argb pixel : inputPixels) {
        const auto it = pixelToCount.find(pixel);
        if (it != pixelToCount.end()) {
            it->second++;
        } else {
            pixels.push_back(pixel);
            points.push_back(labFromArgb(pixel));
            pixelToCount[pixel] = 1;
        }
    }

    const int clusterCount = std::min({ maxColors, int(points.size()), int(startingClusters.size()) });

    int pixelCountSums[256] = {};
    std::vector<Lab> clusters;
    clusters.reserve(startingClusters.size());
    for (const Argb argb : startingClusters) {
        clusters.push_back(labFromArgb(argb));
    }

    // Not a secret: a fixed sequence that only spreads the pixels over the
    // starting clusters, the same in every run so a wallpaper always gives
    // the same color.
    std::minstd_rand random(42688); // NOSONAR(cpp:S2245)
    std::vector<int> clusterIndices;
    clusterIndices.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); i++) {
        clusterIndices.push_back(int(random() % unsigned(clusterCount)));
    }

    std::vector<std::vector<int>> indexMatrix(clusterCount, std::vector<int>(clusterCount, 0));
    std::vector<std::vector<DistanceToIndex>> distanceToIndexMatrix(
        clusterCount, std::vector<DistanceToIndex>(clusterCount));

    for (int iteration = 0; iteration < kMaxIterations; iteration++) {
        // Calculate cluster distances
        for (int i = 0; i < clusterCount; i++) {
            distanceToIndexMatrix[i][i].distance = 0;
            distanceToIndexMatrix[i][i].index = i;
            for (int j = i + 1; j < clusterCount; j++) {
                const double distance = clusters[i].deltaE(clusters[j]);
                distanceToIndexMatrix[j][i].distance = distance;
                distanceToIndexMatrix[j][i].index = i;
                distanceToIndexMatrix[i][j].distance = distance;
                distanceToIndexMatrix[i][j].index = j;
            }
            std::vector<DistanceToIndex> row = distanceToIndexMatrix[i];
            std::sort(row.begin(), row.end());
            for (int j = 0; j < clusterCount; j++) {
                indexMatrix[i][j] = row[j].index;
            }
        }

        // Reassign points
        bool colorMoved = false;
        for (std::size_t i = 0; i < points.size(); i++) {
            const Lab point = points[i];
            const int previousClusterIndex = clusterIndices[i];
            const double previousDistance = point.deltaE(clusters[previousClusterIndex]);
            double minimumDistance = previousDistance;
            int newClusterIndex = -1;
            for (int j = 0; j < clusterCount; j++) {
                if (distanceToIndexMatrix[previousClusterIndex][j].distance >= 4 * previousDistance) {
                    continue;
                }
                const double distance = point.deltaE(clusters[j]);
                if (distance < minimumDistance) {
                    minimumDistance = distance;
                    newClusterIndex = j;
                }
            }
            if (newClusterIndex != -1) {
                const double distanceChange = std::fabs(std::sqrt(minimumDistance) - std::sqrt(previousDistance));
                if (distanceChange > kMinDeltaE) {
                    colorMoved = true;
                    clusterIndices[i] = newClusterIndex;
                }
            }
        }
        if (!colorMoved && iteration != 0) {
            break;
        }

        // Recalculate cluster centers
        double componentASums[256] = {};
        double componentBSums[256] = {};
        double componentCSums[256] = {};
        for (int i = 0; i < clusterCount; i++) {
            pixelCountSums[i] = 0;
        }
        for (std::size_t i = 0; i < points.size(); i++) {
            const int clusterIndex = clusterIndices[i];
            const Lab point = points[i];
            const int count = pixelToCount[pixels[i]];
            pixelCountSums[clusterIndex] += count;
            componentASums[clusterIndex] += (point.l * count);
            componentBSums[clusterIndex] += (point.a * count);
            componentCSums[clusterIndex] += (point.b * count);
        }
        for (int i = 0; i < clusterCount; i++) {
            const int count = pixelCountSums[i];
            if (count == 0) {
                clusters[i] = { 0, 0, 0 };
                continue;
            }
            clusters[i] = { componentASums[i] / count, componentBSums[i] / count, componentCSums[i] / count };
        }
    }

    // Clusters that became the same color count once, with both
    // populations.
    std::map<Argb, std::uint32_t> colorToCount;
    for (int i = 0; i < clusterCount; i++) {
        const int count = pixelCountSums[i];
        if (count == 0) {
            continue;
        }
        colorToCount[argbFromLab(clusters[i])] += std::uint32_t(count);
    }
    return colorToCount;
}

// --- score.cc ---

constexpr double kTargetChroma = 48.0; // A1 Chroma
constexpr double kWeightProportion = 0.7;
constexpr double kWeightChromaAbove = 0.3;
constexpr double kWeightChromaBelow = 0.1;
constexpr double kCutoffChroma = 5.0;
constexpr double kCutoffExcitedProportion = 0.01;

} // namespace

std::map<Argb, std::uint32_t> quantizeCelebi(std::span<const Argb> pixels, int maxColors)
{
    if (maxColors <= 0 || pixels.empty()) {
        return {};
    }
    maxColors = std::min(maxColors, 256);
    std::vector<Argb> opaquePixels;
    opaquePixels.reserve(pixels.size());
    for (const Argb pixel : pixels) {
        if ((pixel >> 24) == 255) {
            opaquePixels.push_back(pixel);
        }
    }
    const std::vector<Argb> wuResult = quantizeWu(opaquePixels, maxColors);
    return quantizeWsmeans(opaquePixels, wuResult, maxColors);
}

std::vector<Argb> rankedSuggestions(const std::map<Argb, std::uint32_t>& population, Argb fallback,
    std::size_t desired)
{
    // The HCT of each color, while finding the per hue count and total
    // count.
    std::vector<Hct> colorsHct;
    std::vector<Argb> colorsArgb;
    std::vector<std::uint32_t> huePopulation(360, 0);
    double populationSum = 0;
    for (const auto& [argb, count] : population) {
        const Hct hct = hctFromArgb(argb);
        colorsHct.push_back(hct);
        colorsArgb.push_back(argb);
        huePopulation[std::clamp(int(std::floor(hct.hue)), 0, 359)] += count;
        populationSum += count;
    }

    // Hues with more usage in neighboring 30 degree slice get a larger
    // number.
    std::vector<double> hueExcitedProportions(360, 0.0);
    for (int hue = 0; hue < 360; hue++) {
        const double proportion = huePopulation[hue] / populationSum;
        for (int i = hue - 14; i < hue + 16; i++) {
            hueExcitedProportions[detail::sanitizeDegrees(i)] += proportion;
        }
    }

    // Scores each color on usage and chroma, leaving out those without
    // enough of either.
    struct Scored {
        Hct hct;
        Argb argb;
        double score;
    };
    std::vector<Scored> scored;
    for (std::size_t i = 0; i < colorsHct.size(); ++i) {
        const Hct& hct = colorsHct[i];
        const int hue = detail::sanitizeDegrees(int(std::round(hct.hue)));
        const double proportion = hueExcitedProportions[hue];
        if (hct.chroma < kCutoffChroma || proportion <= kCutoffExcitedProportion) {
            continue;
        }
        const double proportionScore = proportion * 100.0 * kWeightProportion;
        const double chromaWeight = hct.chroma < kTargetChroma ? kWeightChromaBelow : kWeightChromaAbove;
        const double chromaScore = (hct.chroma - kTargetChroma) * chromaWeight;
        scored.push_back({ hct, colorsArgb[i], proportionScore + chromaScore });
    }
    // Higher scores first.
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) { return a.score > b.score; });

    // The colors with the largest distribution of hues possible: hues at
    // least 90 degrees apart (the most for 4 colors), then less, down to 15.
    std::vector<const Scored*> chosen;
    for (int differenceDegrees = 90; differenceDegrees >= 15; differenceDegrees--) {
        chosen.clear();
        for (const Scored& entry : scored) {
            const bool duplicateHue = std::any_of(chosen.begin(), chosen.end(), [&](const Scored* c) {
                return detail::differenceDegrees(entry.hct.hue, c->hct.hue) < differenceDegrees;
            });
            if (!duplicateHue) {
                chosen.push_back(&entry);
                if (chosen.size() >= desired) {
                    break;
                }
            }
        }
        if (chosen.size() >= desired) {
            break;
        }
    }
    std::vector<Argb> colors;
    if (chosen.empty()) {
        colors.push_back(fallback);
    }
    for (const Scored* c : chosen) {
        colors.push_back(c->argb);
    }
    return colors;
}

Argb sourceFromPixels(std::span<const Argb> pixels, Argb fallback)
{
    return rankedSuggestions(quantizeCelebi(pixels, 128), fallback).front();
}

} // namespace vela::color
