#include "../CPUAndGPUCommon.h"

#define GRootSignature "RootFlags(0), " \
    "DescriptorTable(SRV(t0), visibility = SHADER_VISIBILITY_VERTEX)"

StructuredBuffer<FFragment> GFragments : register(t0);

[RootSignature(GRootSignature)]
void MainVS(
    in uint VertexIdx : SV_VertexID,
    out float4 OutPosition : SV_Position,
	out float3 OutColor : _Color)
{
	FFragment Fragment = GFragments[VertexIdx];
	OutPosition = float4(Fragment.Position, 0.0f, 1.0f);
	OutColor = Fragment.Color;
}

[RootSignature(GRootSignature)]
void MainPS(
    in float4 InPosition : SV_Position,
	in float3 InColor : _Color,
	out float4 OutColor : SV_Target0)
{
    OutColor = float4(InColor, 1.0f);
}
