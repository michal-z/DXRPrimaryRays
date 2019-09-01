#include "../CPUAndGPUCommon.h"

GlobalRootSignature GlobalSignature =
{
	"DescriptorTable(UAV(u0)),"
	"SRV(t0),"
	"CBV(b0),"
	"DescriptorTable(SRV(t1, numDescriptors = 2)),"
};

TriangleHitGroup HitGroup =
{
	"", // AnyHit
	"MainCHS", // ClosestHit
};

RaytracingShaderConfig ShaderConfig =
{
	16, // max payload size
	8, // max attribute size
};

RaytracingPipelineConfig PipelineConfig =
{
	1, // max trace recursion depth
};

RaytracingAccelerationStructure GScene : register(t0);
RWStructuredBuffer<FFragment> GOutput : register(u0);
ConstantBuffer<FPerFrameConstantData> GPerFrameCB : register(b0);
StructuredBuffer<FVertex> GVertexBuffer : register(t1);
Buffer<uint3> GIndexBuffer : register(t2);

typedef BuiltInTriangleIntersectionAttributes FAttributes;
struct FPayload
{
	float4 Color;
};

void GenerateCameraRay(uint2 RayIndex, out float3 Origin, out float3 Direction)
{
	float2 XY = RayIndex + 0.5f;
	float2 ScreenPos = XY / DispatchRaysDimensions().xy * 2.0f - 1.0f;

	ScreenPos.y = -ScreenPos.y;

	float4 World = mul(float4(ScreenPos, 0.0f, 1.0f), GPerFrameCB.ProjectionToWorld);
	World.xyz /= World.w;

	Origin = GPerFrameCB.CameraPosition.xyz;
	Direction = normalize(World.xyz - Origin);
}

[shader("raygeneration")]
void MainRGS()
{
	float3 Origin, Direction;
	GenerateCameraRay(DispatchRaysIndex().xy, Origin, Direction);

	RayDesc Ray;
	Ray.Origin = Origin;
	Ray.Direction = Direction;
	Ray.TMin = 0.001f;
	Ray.TMax = 1000.0f;
	FPayload Payload = { float4(0.0f, 0.0f, 0.0f, 0.0f) };
	TraceRay(GScene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, ~0, 0, 1, 0, Ray, Payload);
}

[shader("miss")]
void MainMS(inout FPayload Payload)
{
	float2 XY = DispatchRaysIndex().xy + 0.5f;
	float2 ScreenPos = XY / DispatchRaysDimensions().xy * 2.0f - 1.0f;
	ScreenPos.y = -ScreenPos.y;

	uint FragmentIdx = GOutput.IncrementCounter();
	GOutput[FragmentIdx].Position = ScreenPos;
	GOutput[FragmentIdx].Color = 0.5f;

	Payload.Color = 0.0f;
}

[shader("closesthit")]
void MainCHS(inout FPayload Payload, in FAttributes Attribs)
{
	float2 XY = DispatchRaysIndex().xy + 0.5f;
	float2 ScreenPos = XY / DispatchRaysDimensions().xy * 2.0f - 1.0f;
	ScreenPos.y = -ScreenPos.y;

	uint FragmentIdx = GOutput.IncrementCounter();
	GOutput[FragmentIdx].Position = ScreenPos;


	float3 Position = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();

	uint3 Triangle = GIndexBuffer[PrimitiveIndex()];

	float3 Normals[3] = { GVertexBuffer[Triangle.x].Normal, GVertexBuffer[Triangle.y].Normal, GVertexBuffer[Triangle.z].Normal };

	float3 N = Normals[0] + (Normals[1] - Normals[0]) * Attribs.barycentrics.x + (Normals[2] - Normals[0]) * Attribs.barycentrics.y;


	float3 Color = abs(N);
	GOutput[FragmentIdx].Color = Color;

	Payload.Color = 1.0f;
}
