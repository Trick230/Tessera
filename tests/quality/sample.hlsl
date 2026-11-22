// SPDX-License-Identifier: GPL-3.0-or-later
Texture2D<float4> Source : register(t3);
SamplerState Filter : register(s1);
#ifdef EXISTING_UAV
RWByteAddressBuffer Existing : register(u2);
#endif

float4 main(float4 position : SV_Position) : SV_Target
{
#ifdef EXISTING_UAV
    Existing.Store(0, 1);
#endif
    return Source.Sample(Filter, position.xy / 4.0);
}
