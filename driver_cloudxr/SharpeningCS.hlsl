// MIT License
//
// Copyright(c) 2026 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

cbuffer config : register(b0)
{
    uint4 const0; // CAS
    uint4 const1; // CAS
    uint2 topLeft;
    uint2 extent;
    uint yflip;
};

Texture2D<float4> sourceTexture : register(t0);
RWTexture2D<float4> sharpenedTexture : register(u0);

#define A_GPU 1
#define A_HLSL 1

#include <ffx-cas/ffx_a.h>

AF3 CasLoad(ASU2 p)
{
    p += topLeft.xy;
    if (yflip)
    {
        p.y = extent.y - p.y;
    }
    return sourceTexture.Load(int3(p, 0)).rgb;
}

void CasInput(inout AF1 r, inout AF1 g, inout AF1 b)
{
}

#include <ffx-cas/ffx_cas.h>

void CasStore(AU2 p, AF3 c)
{
#ifdef SRGB
    // From https://github.com/Microsoft/DirectX-Graphics-Samples/blob/master/MiniEngine/Core/Shaders/ColorSpaceUtility.hlsli
    c = c < 0.0031308 ? 12.92 * c : 1.13005 * sqrt(c - 0.00228) - 0.13448 * c + 0.005719;
#endif

    // Discard alpha, which is OK since we only sharpen the bottom layer.
    sharpenedTexture[p] = AF4(c, 1);
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 wgid : SV_GroupID)
{
    // Do remapping of local xy in workgroup for a more PS-like swizzle pattern.
    AU2 gxy = ARmp8x8(tid.x) + AU2(wgid.x << 4u, wgid.y << 4u);

    AF3 c;

    CasFilter(c.r, c.g, c.b, gxy, const0, const1, true /* noScaling */);
    CasStore(gxy, c);
    gxy.x += 8u;

    CasFilter(c.r, c.g, c.b, gxy, const0, const1, true /* noScaling */);
    CasStore(gxy, c);
    gxy.y += 8u;

    CasFilter(c.r, c.g, c.b, gxy, const0, const1, true /* noScaling */);
    CasStore(gxy, c);
    gxy.x -= 8u;

    CasFilter(c.r, c.g, c.b, gxy, const0, const1, true /* noScaling */);
    CasStore(gxy, c);
}
