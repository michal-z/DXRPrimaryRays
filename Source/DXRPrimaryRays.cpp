#include "Library.h"
#include "CPUAndGPUCommon.h"
#include "d3dx12.h"
#include "imgui/imgui.h"
#include "EAStdC/EAStdC.h"
#include "EAStdC/EASprintf.h"
#include "EAStdC/EABitTricks.h"
#include "stb_image.h"

enum
{
	RTPSO_Raytracing,
};

enum
{
	PSO_DrawFragments,
};

struct FPipeline
{
	ID3D12PipelineState* Pipeline;
	ID3D12RootSignature* Signature;
};

struct FRTPipeline
{
	ID3D12StateObject* RTPipeline;
	ID3D12RootSignature* RTGlobalSignature;
};

struct FDemoRoot
{
	FGraphicsContext Gfx;
	FUIContext UI;
	eastl::vector<FPipeline> Pipelines;
	eastl::vector<FRTPipeline> RTPipelines;
	ID3D12Resource* VertexBuffer;
	ID3D12Resource* IndexBuffer;
	D3D12_CPU_DESCRIPTOR_HANDLE VertexBufferSRV;
	D3D12_CPU_DESCRIPTOR_HANDLE IndexBufferSRV;
	ID3D12Resource* BLASResultBuffer;
	ID3D12Resource* TLASInstanceBuffer;
	ID3D12Resource* TLASResultBuffer;
	ID3D12Resource* ShaderTable;
	ID3D12Resource* RTOutput;
	D3D12_CPU_DESCRIPTOR_HANDLE RTOutputSRV;
	D3D12_CPU_DESCRIPTOR_HANDLE RTOutputUAV;
	XMFLOAT3 CameraPosition;
	XMFLOAT3 CameraFocusPosition;
	uint32_t NumFragmentsToDraw;
};

static void Update(FDemoRoot& Root)
{
	double Time;
	float DeltaTime;
	UpdateFrameStats(Root.Gfx.Window, "DXRTest", Time, DeltaTime);
	UpdateUI(DeltaTime);

	ImGui::ShowDemoWindow();
}

static void Draw(FDemoRoot& Root)
{
	FGraphicsContext& Gfx = Root.Gfx;
	ID3D12GraphicsCommandList5* CmdList = GetAndInitCommandList(Gfx);

	// Raytrace.
	if (Gfx.FrameCount == 1)
	{
		const XMMATRIX ViewTransform = XMMatrixLookAtLH(XMLoadFloat3(&Root.CameraPosition), XMLoadFloat3(&Root.CameraFocusPosition), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
		const XMMATRIX ProjectionTransform = XMMatrixPerspectiveFovLH(XM_PI / 3, 1.0f, 0.1f, 100.0f);
		const XMMATRIX ProjectionToWorld = XMMatrixTranspose(XMMatrixInverse(nullptr, ViewTransform * ProjectionTransform));

		D3D12_GPU_VIRTUAL_ADDRESS GPUAddress;
		auto* CPUAddress = (FPerFrameConstantData*)AllocateGPUMemory(Gfx, sizeof(FPerFrameConstantData), GPUAddress);
		XMStoreFloat4x4(&CPUAddress->ProjectionToWorld, ProjectionToWorld);
		{
			const XMFLOAT3 P = Root.CameraPosition;
			CPUAddress->CameraPosition = XMFLOAT4(P.x, P.y, P.z, 1.0f);
		}

		CmdList->SetPipelineState1(Root.RTPipelines[RTPSO_Raytracing].RTPipeline);
		CmdList->SetComputeRootSignature(Root.RTPipelines[RTPSO_Raytracing].RTGlobalSignature);
		CmdList->SetComputeRootDescriptorTable(0, CopyDescriptorsToGPUHeap(Gfx, 1, Root.RTOutputUAV));
		CmdList->SetComputeRootShaderResourceView(1, Root.TLASResultBuffer->GetGPUVirtualAddress());
		CmdList->SetComputeRootConstantBufferView(2, GPUAddress);
		{
			const D3D12_GPU_DESCRIPTOR_HANDLE TableBase = CopyDescriptorsToGPUHeap(Gfx, 1, Root.VertexBufferSRV);
			CopyDescriptorsToGPUHeap(Gfx, 1, Root.IndexBufferSRV);
			CmdList->SetComputeRootDescriptorTable(3, TableBase);
		}

		{
			const D3D12_GPU_VIRTUAL_ADDRESS Base = Root.ShaderTable->GetGPUVirtualAddress();
			D3D12_DISPATCH_RAYS_DESC DispatchDesc = {};
			DispatchDesc.RayGenerationShaderRecord = { Base, 32 };
			DispatchDesc.MissShaderTable = { Base + 64, 32, 32 };
			DispatchDesc.HitGroupTable = { Base + 128, 32, 32 };
			DispatchDesc.Width = Gfx.Resolution[0];
			DispatchDesc.Height = Gfx.Resolution[1];
			DispatchDesc.Depth = 1;
			CmdList->DispatchRays(&DispatchDesc);
		}

		CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(Root.RTOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	}

	ID3D12Resource* BackBuffer;
	D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRTV;
	GetBackBuffer(Gfx, BackBuffer, BackBufferRTV);

	CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(BackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

	CmdList->RSSetViewports(1, &CD3DX12_VIEWPORT(0.0f, 0.0f, (float)Gfx.Resolution[0], (float)Gfx.Resolution[1]));
	CmdList->RSSetScissorRects(1, &CD3DX12_RECT(0, 0, (LONG)Gfx.Resolution[0], (LONG)Gfx.Resolution[1]));

	CmdList->OMSetRenderTargets(1, &BackBufferRTV, TRUE, nullptr);
	CmdList->ClearRenderTargetView(BackBufferRTV, XMVECTORF32{ 0.0f }, 0, nullptr);

	// Draw fragments.
	CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
	CmdList->SetPipelineState(Root.Pipelines[PSO_DrawFragments].Pipeline);
	CmdList->SetGraphicsRootSignature(Root.Pipelines[PSO_DrawFragments].Signature);
	CmdList->SetGraphicsRootDescriptorTable(0, CopyDescriptorsToGPUHeap(Gfx, 1, Root.RTOutputSRV));
	CmdList->DrawInstanced(Root.NumFragmentsToDraw, 1, 0, 0);

	if ((Root.NumFragmentsToDraw += 256) >= (Gfx.Resolution[0] * Gfx.Resolution[1]))
	{
		Root.NumFragmentsToDraw = 0;
	}

	DrawUI(Gfx, Root.UI);

	CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(BackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

	CmdList->Close();

	Gfx.CmdQueue->ExecuteCommandLists(1, CommandListCast(&CmdList));
}

static void AddGraphicsPipeline(FGraphicsContext& Gfx, D3D12_GRAPHICS_PIPELINE_STATE_DESC& PSODesc, const char* VSName, const char* PSName, eastl::vector<FPipeline>& OutPipelines)
{
	char Path[MAX_PATH];

	EA::StdC::Snprintf(Path, sizeof(Path), "Data/Shaders/%s", VSName);
	eastl::vector<uint8_t> VSBytecode = LoadFile(Path);

	EA::StdC::Snprintf(Path, sizeof(Path), "Data/Shaders/%s", PSName);
	eastl::vector<uint8_t> PSBytecode = LoadFile(Path);

	ID3D12RootSignature* RootSignature;
	VHR(Gfx.Device->CreateRootSignature(0, VSBytecode.data(), VSBytecode.size(), IID_PPV_ARGS(&RootSignature)));

	PSODesc.pRootSignature = RootSignature;
	PSODesc.VS = { VSBytecode.data(), VSBytecode.size() };
	PSODesc.PS = { PSBytecode.data(), PSBytecode.size() };

	ID3D12PipelineState* Pipeline;
	VHR(Gfx.Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&Pipeline)));

	OutPipelines.push_back(FPipeline{ Pipeline, RootSignature });
}

static void CreatePipelines(FGraphicsContext& Gfx, eastl::vector<FPipeline>& OutPipelines)
{
	// DrawFragments pipeline.
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc = {};
		PSODesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		PSODesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
		PSODesc.NumRenderTargets = 1;
		PSODesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		PSODesc.SampleMask = UINT32_MAX;
		PSODesc.SampleDesc.Count = 1;
		EA_ASSERT(OutPipelines.size() == PSO_DrawFragments);
		AddGraphicsPipeline(Gfx, PSODesc, "DrawFragments.vs.cso", "DrawFragments.ps.cso", OutPipelines);
	}
}

static void CreateRTPipelines(FGraphicsContext& Gfx, eastl::vector<FRTPipeline>& OutRTPipelines)
{
	// RTPSO_Raytracing.
	{
		char Path[MAX_PATH];
		EA::StdC::Snprintf(Path, sizeof(Path), "Data/Shaders/%s", "Raytracing.lib.cso");
		eastl::vector<uint8_t> DXIL = LoadFile(Path);

		CD3DX12_STATE_OBJECT_DESC PipelineDesc{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };
		auto DXILLibrary = PipelineDesc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();

		DXILLibrary->SetDXILLibrary(&CD3DX12_SHADER_BYTECODE(DXIL.data(), DXIL.size()));

		FRTPipeline Pipeline = {};
		VHR(Gfx.Device->CreateStateObject(PipelineDesc, IID_PPV_ARGS(&Pipeline.RTPipeline)));
		VHR(Gfx.Device->CreateRootSignature(0, DXIL.data(), DXIL.size(), IID_PPV_ARGS(&Pipeline.RTGlobalSignature)));
		EA_ASSERT(OutRTPipelines.size() == RTPSO_Raytracing);
		OutRTPipelines.push_back(Pipeline);
	}
}

static void CreateStaticGeometry(FDemoRoot& Root, eastl::vector<ID3D12Resource*>& OutTempResources)
{
	FGraphicsContext& Gfx = Root.Gfx;

	eastl::vector<FVertex> VertexData;
	eastl::vector<uint32_t> Triangles;

	{
		eastl::vector<XMFLOAT3> Positions;
		eastl::vector<XMFLOAT3> Normals;
		eastl::vector<XMFLOAT2> Texcoords;

		const size_t PositionsSize = Positions.size();
		const size_t TrianglesSize = Triangles.size();
		LoadPLYFile("Data/Meshes/Monkey.ply", Positions, Normals, Texcoords, Triangles);

		VertexData.reserve(Positions.size());

		for (uint32_t Idx = 0; Idx < Positions.size(); ++Idx)
		{
			FVertex Vertex;
			Vertex.Position = Positions[Idx];
			Vertex.Normal = Normals[Idx];
			VertexData.push_back(Vertex);
		}
	}

	// Static geometry vertex buffer (single buffer for all static meshes).
	{
		const D3D12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(VertexData.size() * sizeof(FVertex));

		ID3D12Resource* StagingVB;
		VHR(Gfx.Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&StagingVB)));
		OutTempResources.push_back(StagingVB);

		void* Ptr;
		VHR(StagingVB->Map(0, &CD3DX12_RANGE(0, 0), &Ptr));
		memcpy(Ptr, VertexData.data(), VertexData.size() * sizeof(FVertex));
		StagingVB->Unmap(0, nullptr);

		VHR(Gfx.Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&Root.VertexBuffer)));

		Gfx.CmdList->CopyResource(Root.VertexBuffer, StagingVB);
		Gfx.CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(Root.VertexBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

		Root.VertexBufferSRV = AllocateDescriptors(Gfx, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);

		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SRVDesc.Buffer.NumElements = (uint32_t)VertexData.size();
		SRVDesc.Buffer.StructureByteStride = sizeof(FVertex);
		Gfx.Device->CreateShaderResourceView(Root.VertexBuffer, &SRVDesc, Root.VertexBufferSRV);
	}

	// Static geometry index buffer (single buffer for all static meshes).
	{
		const D3D12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(Triangles.size() * sizeof(uint32_t));

		ID3D12Resource* StagingIB;
		VHR(Gfx.Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&StagingIB)));
		OutTempResources.push_back(StagingIB);

		void* Ptr;
		VHR(StagingIB->Map(0, &CD3DX12_RANGE(0, 0), &Ptr));
		memcpy(Ptr, Triangles.data(), Triangles.size() * sizeof(uint32_t));
		StagingIB->Unmap(0, nullptr);

		VHR(Gfx.Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&Root.IndexBuffer)));

		Gfx.CmdList->CopyResource(Root.IndexBuffer, StagingIB);
		Gfx.CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(Root.IndexBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

		Root.IndexBufferSRV = AllocateDescriptors(Gfx, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);

		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.Format = DXGI_FORMAT_R32G32B32_UINT;
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SRVDesc.Buffer.NumElements = (uint32_t)Triangles.size() / 3;
		Gfx.Device->CreateShaderResourceView(Root.IndexBuffer, &SRVDesc, Root.IndexBufferSRV);
	}

	// Bottom Level Acceleration Structure (BLAS).
	{
		D3D12_RAYTRACING_GEOMETRY_DESC GeometryDesc = {};
		GeometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		GeometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		GeometryDesc.Triangles.VertexBuffer.StartAddress = Root.VertexBuffer->GetGPUVirtualAddress();
		GeometryDesc.Triangles.VertexBuffer.StrideInBytes = (UINT)sizeof(FVertex);
		GeometryDesc.Triangles.VertexCount = (UINT)VertexData.size();
		GeometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		GeometryDesc.Triangles.IndexBuffer = Root.IndexBuffer->GetGPUVirtualAddress();
		GeometryDesc.Triangles.IndexCount = (UINT)Triangles.size();
		GeometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS BLASInputs = {};
		BLASInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		BLASInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		BLASInputs.NumDescs = 1;
		BLASInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		BLASInputs.pGeometryDescs = &GeometryDesc;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO BLASBuildInfo = {};
		Gfx.Device->GetRaytracingAccelerationStructurePrebuildInfo(&BLASInputs, &BLASBuildInfo);

		ID3D12Resource* BLASScratchBuffer;
		{
			const CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(BLASBuildInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			VHR(Gfx.Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&BLASScratchBuffer)));
			OutTempResources.push_back(BLASScratchBuffer);
		}

		// Create BLASResultBuffer.
		{
			const CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(BLASBuildInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			VHR(Gfx.Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&Root.BLASResultBuffer)));
		}

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC BLASBuildDesc = {};
		BLASBuildDesc.Inputs = BLASInputs;
		BLASBuildDesc.ScratchAccelerationStructureData = BLASScratchBuffer->GetGPUVirtualAddress();
		BLASBuildDesc.DestAccelerationStructureData = Root.BLASResultBuffer->GetGPUVirtualAddress();

		Gfx.CmdList->BuildRaytracingAccelerationStructure(&BLASBuildDesc, 0, nullptr);
		Gfx.CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(Root.BLASResultBuffer));
	}

	// Top Level Acceleration Structure (TLAS).
	{
		const uint32_t NumInstances = 1;
		ID3D12Resource* UploadInstanceBuffer;
		{
			D3D12_RAYTRACING_INSTANCE_DESC InstanceDesc = {};
			InstanceDesc.InstanceID = 0;
			InstanceDesc.InstanceContributionToHitGroupIndex = 0;
			InstanceDesc.InstanceMask = 1;
			{
				const float Identity[3][4] = { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
				memcpy(InstanceDesc.Transform, Identity, sizeof(Identity));
			}
			InstanceDesc.AccelerationStructure = Root.BLASResultBuffer->GetGPUVirtualAddress();

			// Create UploadInstanceBuffer.
			{
				CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(NumInstances * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
				VHR(Gfx.Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&UploadInstanceBuffer)));
				OutTempResources.push_back(UploadInstanceBuffer);
			}

			void* Ptr;
			VHR(UploadInstanceBuffer->Map(0, &CD3DX12_RANGE(0, 0), &Ptr));
			memcpy(Ptr, &InstanceDesc, NumInstances * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
			UploadInstanceBuffer->Unmap(0, nullptr);
		}

		// Create TLASInstanceBuffer.
		{
			CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(NumInstances * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
			VHR(Gfx.Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&Root.TLASInstanceBuffer)));
		}

		Gfx.CmdList->CopyResource(Root.TLASInstanceBuffer, UploadInstanceBuffer);
		Gfx.CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(Root.TLASInstanceBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS TLASInputs = {};
		TLASInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		TLASInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		TLASInputs.NumDescs = 1;
		TLASInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		TLASInputs.InstanceDescs = Root.TLASInstanceBuffer->GetGPUVirtualAddress();

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO TLASBuildInfo = {};
		Gfx.Device->GetRaytracingAccelerationStructurePrebuildInfo(&TLASInputs, &TLASBuildInfo);

		ID3D12Resource* TLASScratchBuffer;
		{
			const CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(TLASBuildInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			VHR(Gfx.Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&TLASScratchBuffer)));
			OutTempResources.push_back(TLASScratchBuffer);
		}

		// Create TLASResultBuffer.
		{
			const CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(TLASBuildInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			VHR(Gfx.Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&Root.TLASResultBuffer)));
		}

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC TLASBuildDesc = {};
		TLASBuildDesc.Inputs = TLASInputs;
		TLASBuildDesc.ScratchAccelerationStructureData = TLASScratchBuffer->GetGPUVirtualAddress();
		TLASBuildDesc.DestAccelerationStructureData = Root.TLASResultBuffer->GetGPUVirtualAddress();

		Gfx.CmdList->BuildRaytracingAccelerationStructure(&TLASBuildDesc, 0, nullptr);
		Gfx.CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(Root.TLASResultBuffer));
	}
}

static bool Initialize(FDemoRoot& Root)
{
	FGraphicsContext& Gfx = Root.Gfx;

	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS5 Options5 = {};
		Gfx.Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &Options5, sizeof(Options5));
		if (Options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
		{
			MessageBox(Gfx.Window, "This application requires GPU with raytracing support.", "Raytracing is not supported", MB_OK | MB_ICONERROR);
			return false;
		}
	}

	eastl::vector<ID3D12Resource*> TempResources;

	CreateUIContext(Gfx, 1, Root.UI, TempResources);
	CreateStaticGeometry(Root, TempResources);
	CreatePipelines(Gfx, Root.Pipelines);
	CreateRTPipelines(Gfx, Root.RTPipelines);

	// Create Shader Table.
	{
		ID3D12Resource* TempShaderTable;
		{
			const auto Desc = CD3DX12_RESOURCE_DESC::Buffer(1024);
			VHR(Gfx.Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&TempShaderTable)));
			TempResources.push_back(TempShaderTable);

			VHR(Gfx.Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&Root.ShaderTable)));
		}

		uint8_t* ShaderTableAddr;
		VHR(TempShaderTable->Map(0, &CD3DX12_RANGE(0, 0), (void**)& ShaderTableAddr));

		ID3D12StateObject* Pipeline = Root.RTPipelines[RTPSO_Raytracing].RTPipeline;
		ID3D12StateObjectProperties* Props;
		VHR(Pipeline->QueryInterface(IID_PPV_ARGS(&Props)));

		memcpy(ShaderTableAddr + 0, Props->GetShaderIdentifier(L"MainRGS"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
		memcpy(ShaderTableAddr + 64, Props->GetShaderIdentifier(L"MainMS"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
		memcpy(ShaderTableAddr + 128, Props->GetShaderIdentifier(L"HitGroup"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

		SAFE_RELEASE(Props);
		TempShaderTable->Unmap(0, nullptr);

		Gfx.CmdList->CopyResource(Root.ShaderTable, TempShaderTable);
		Gfx.CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(Root.ShaderTable, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	}

	// Create output texture for raytracing stage.
	{
		auto Desc = CD3DX12_RESOURCE_DESC::Buffer((Gfx.Resolution[0] * Gfx.Resolution[1] + 1) * sizeof(FFragment));
		Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		VHR(Gfx.Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&Root.RTOutput)));

		Root.RTOutputSRV = AllocateDescriptors(Gfx, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);
		Root.RTOutputUAV = AllocateDescriptors(Gfx, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);

		D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
		UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		UAVDesc.Buffer.FirstElement = 1;
		UAVDesc.Buffer.NumElements = Gfx.Resolution[0] * Gfx.Resolution[1];
		UAVDesc.Buffer.StructureByteStride = sizeof(FFragment);
		UAVDesc.Buffer.CounterOffsetInBytes = 0;
		Gfx.Device->CreateUnorderedAccessView(Root.RTOutput, Root.RTOutput, &UAVDesc, Root.RTOutputUAV);

		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SRVDesc.Buffer.FirstElement = 1;
		SRVDesc.Buffer.NumElements = Gfx.Resolution[0] * Gfx.Resolution[1];
		SRVDesc.Buffer.StructureByteStride = sizeof(FFragment);
		Gfx.Device->CreateShaderResourceView(Root.RTOutput, &SRVDesc, Root.RTOutputSRV);
	}

	// Execute "data upload" and "data generation" GPU commands, create mipmaps etc. Destroy temp resources when GPU is done.
	{
		Root.Gfx.CmdList->Close();
		Root.Gfx.CmdQueue->ExecuteCommandLists(1, CommandListCast(&Root.Gfx.CmdList));
		WaitForGPU(Root.Gfx);

		for (ID3D12Resource* Resource : TempResources)
		{
			SAFE_RELEASE(Resource);
		}
	}

	Root.CameraPosition = XMFLOAT3(0.0f, 0.0f, 2.5f);
	Root.CameraFocusPosition = XMFLOAT3(0.0f, 0.0f, 0.0f);

	return true;
}

static void Shutdown(FDemoRoot& Root)
{
	for (FRTPipeline& Pipeline : Root.RTPipelines)
	{
		SAFE_RELEASE(Pipeline.RTPipeline);
		SAFE_RELEASE(Pipeline.RTGlobalSignature);
	}
	SAFE_RELEASE(Root.VertexBuffer);
	SAFE_RELEASE(Root.IndexBuffer);
	SAFE_RELEASE(Root.BLASResultBuffer);
	SAFE_RELEASE(Root.TLASInstanceBuffer);
	SAFE_RELEASE(Root.TLASResultBuffer);
	SAFE_RELEASE(Root.ShaderTable);
	SAFE_RELEASE(Root.RTOutput);
	DestroyUIContext(Root.UI);
}

static int32_t Run(FDemoRoot& Root)
{
	EA::StdC::Init();
	ImGui::CreateContext();

	HWND Window = CreateSimpleWindow("DXRTest", 1024, 1024);
	CreateGraphicsContext(Window, /*bShouldCreateDepthBuffer*/false, Root.Gfx);

	if (Initialize(Root))
	{
		for (;;)
		{
			MSG Message = {};
			if (PeekMessage(&Message, 0, 0, 0, PM_REMOVE))
			{
				DispatchMessage(&Message);
				if (Message.message == WM_QUIT)
				{
					break;
				}
			}
			else
			{
				Update(Root);
				Draw(Root);
				PresentFrame(Root.Gfx, 1);
			}
		}
	}

	WaitForGPU(Root.Gfx);
	Shutdown(Root);
	DestroyGraphicsContext(Root.Gfx);
	ImGui::DestroyContext();
	EA::StdC::Shutdown();

	return 0;
}

int32_t CALLBACK WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int32_t)
{
	SetProcessDPIAware();
	FDemoRoot Root = {};
	return Run(Root);
}
