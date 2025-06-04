#pragma once

#include "Engine/Graphics/PixelFormat.h"
#include "Engine/Scripting/ScriptingObjectReference.h"
#include "Engine/Level/Actor.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "Engine/Core/Math/Matrix.h"
#include "Engine/Graphics/RenderTask.h"

class RenderTask;

GPU_CB_STRUCT(HZBData{
    Float4 ViewInfo;
    Float3 ViewPos;
    float ViewFar;
    Matrix InvViewMatrix;
    Matrix InvProjectionMatrix;
    Float2 Dimensions;
    int Level;
    int Offset;
    //float Dummy0;
    });


/// <summary>
/// Hierarchial Z-Buffer rendering service
/// </summary>
class HZBRenderer
{
public:
    /// <summary>
    /// Checks if resources are ready to render HZB (shaders or textures may be during loading).
    /// </summary>
    /// <returns>True if is ready, otherwise false.</returns>
    static bool HasReadyResources();

    /// <summary>
    /// Init HZB content
    /// </summary>
    /// <returns>True if cannot init service</returns>
    static bool Init();

    /// <summary>
    /// Release HZB content
    /// </summary>
    static void Release();
    
    static void AddOccluder(Actor* actor);
    static void RemoveOccluder(Actor* actor);
    static void ClearOccluders();

    static bool CheckOcclusion();

    /// <summary>
    /// Render the base depth buffer if it's ready.
    /// </summary>
    static void TryRender(GPUContext* context, RenderContext& renderContext);

    static void CompleteDownload();

    static void RenderDebug(RenderContext &renderContext, GPUContext* context);
private:
    static void SetInputs(const RenderView& view, HZBData& data, Float2 dimensions, int level, int offset);
    static Array<Actor*> _actors;
};
