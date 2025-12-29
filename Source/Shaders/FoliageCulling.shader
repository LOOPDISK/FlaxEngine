#include "./Flax/Common.hlsl"

// Input buffers
Buffer<float4> InstanceBuffer : register(t0);
StructuredBuffer<uint> TypeToMeshIndex : register(t1);

// Camera parameters
META_CB_BEGIN(0, CameraParams)
float4 FrustumPlanes[6];
float3 CameraPosition;
float BoundingRadius;
uint InstanceCount;
uint Padding0;
uint Padding1;
uint Padding2;
META_CB_END

// Output: Indirect draw arguments
struct DrawIndexedIndirectArgs
{
    uint IndicesCount;
    uint InstanceCount;
    uint StartIndex;
    uint StartVertex;
    uint StartInstance;
};

RWStructuredBuffer<DrawIndexedIndirectArgs> IndirectArgs : register(u0);
RWStructuredBuffer<uint> VisibleInstances : register(u1);

META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(64, 1, 1)]
void CS_CullInstances(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    uint instanceIndex = DispatchThreadID.x;
    if (instanceIndex >= InstanceCount)
        return;
    
    uint baseIndex = instanceIndex * 8;
    
    // Read transform (position in .w components)
    float3 worldPos = float3(
        InstanceBuffer[baseIndex + 0].w,  // M41
        InstanceBuffer[baseIndex + 1].w,  // M42
        InstanceBuffer[baseIndex + 2].w   // M43
    );
    
    // Read metadata from slot 7
    float4 metadata = InstanceBuffer[baseIndex + 7];
    float cullDistance = metadata.x;
    int type = (int)metadata.y;
    
    // Distance culling
    float distanceSq = dot(worldPos - CameraPosition, worldPos - CameraPosition);
    if (distanceSq > cullDistance * cullDistance)
        return;
    
    // Frustum culling
    [unroll]
    for (int i = 0; i < 6; i++)
    {
        float distance = dot(FrustumPlanes[i].xyz, worldPos) + FrustumPlanes[i].w;
        if (distance < -BoundingRadius)
            return;
    }
    
    // Visible - write to output
    uint meshIndex = TypeToMeshIndex[type];
    if (meshIndex == 0xFFFFFFFF)
        return;
    
    uint visibleIndex;
    InterlockedAdd(IndirectArgs[meshIndex].InstanceCount, 1, visibleIndex);
    VisibleInstances[visibleIndex] = instanceIndex;
}
