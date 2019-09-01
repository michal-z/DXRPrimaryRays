#pragma once

#ifdef __cplusplus
#include "DirectXMath/DirectXMath.h"
typedef XMFLOAT4X4 float4x4;
typedef XMFLOAT4X3 float4x3;
typedef XMFLOAT2 float2;
typedef XMFLOAT3 float3;
typedef XMFLOAT4 float4;
#endif

#ifdef __cplusplus
#define SALIGN alignas(256)
#else
#define SALIGN
#endif

struct SALIGN FPerFrameConstantData
{
	float4x4 ProjectionToWorld;
	float4 CameraPosition;
};

struct FVertex
{
	float3 Position;
	float3 Normal;
};

struct FFragment
{
	float2 Position;
	float3 Color;
};

#ifdef __cplusplus
#undef SALIGN
#endif
