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
#ifdef EXPLICIT_LOD
    return Source.SampleLevel(Filter, position.xy / 4.0, 2.25);
#elif defined(EXPLICIT_LOD_REGISTER)
    return Source.SampleLevel(Filter, position.xy / 4.0, 2.0 + position.y * 0.0625);
#else
    return Source.Sample(Filter, position.xy / 4.0);
#endif
}
