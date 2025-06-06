#pragma once

#include "RendererPass.h"
#include "Engine/Graphics/PixelFormat.h"
#include "Engine/Scripting/ScriptingObjectReference.h"
#include "Engine/Level/Actor.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "Engine/Graphics/Textures/TextureData.h"
#include "Engine/Core/Math/Matrix.h"
#include "Engine/Graphics/RenderTask.h"

#define HZB_FRAME_COUNT 4

class TextureData;
class RenderTask;
struct HZBShaderData;
struct HZBData;
struct UploadHZBTask;

/// <summary>
/// Hierarchial Z-Buffer rendering pass
/// </summary>
class HierarchialZBufferPass : public RendererPass<HierarchialZBufferPass>
{
public:
    // [RendererPass]
    String ToString() const override;
    bool Init() override;
    void Dispose() override;

    /// <summary>
    /// Manages creation and disposal of an HZBData, linked to a SceneRenderTask.
    /// </summary>
    HZBData* GetOrCreateInfo(RenderContext& renderContext);

    /// <summary>
    /// Attempts to draw a HZB frame, unless all frames are being downloaded.
    /// </summary>
    void Render(GPUContext* context, RenderContext& renderContext);

    /// <summary>
    /// Draw the HZB pyramid over the depth buffer.
    /// </summary>
    void RenderDebug(RenderContext& renderContext, GPUContext* context);

protected:
    // [RendererPass]
    bool setupResources() override;

private:
    bool _supported = false;
    AssetReference<Shader> _shader;
    GPUConstantBuffer* _cb = nullptr;
    GPUPipelineState* _psHZB = nullptr;
    GPUPipelineState* _psDebug = nullptr;
    Array<HZBData*> _info;
    Array<Actor*> _emptyArray; // empty array to pass as dummy argument

    void SetInputs(const RenderView& view, HZBShaderData& data, Float2 dimensions, int level, int offset);

#if COMPILE_WITH_DEV_ENV
    void OnShaderReloading(Asset* obj);
#endif
};

/// <summary>
/// A single frame of HZB data
/// </summary>
struct HZBFrame
{
    int Index = 0;
    bool IsDownloading = false;
    GPUTexture* StagingTexture = nullptr;
    TextureData TextureData;
    Viewport Viewport;
    Matrix VP;
    Float3 ViewPosition;
    Float3 ViewDirection;
    Float3 ViewDirectionPerpendicular;
};

/// <summary>
/// The data structure that contains multiple frames of HZB data. It is associated with a SceneRenderTask. Use this to make occlusion queries.
/// </summary>
class HZBData
{
    friend HierarchialZBufferPass;
    friend UploadHZBTask;

public:
    bool Init();
    void Dispose();
    bool CheckSkip();

private:
    bool _isReady = false;
    bool _isValid = true;
    Float2 _resolution;
    int _nextRenderFrameIndex = 0;
    int _mostRecentAvailableFrameIndex = -1;
    void CompleteDownload(int frameIndex);

    GPUTexture* _depthTexture = nullptr;
    GPUTexture* _hzbTexture = nullptr;
    HZBFrame _frames[HZB_FRAME_COUNT];

    //Array<Rectangle> _debugArray;

public:
    /// <summary>
    /// Returns true if the bounds are completely occluded from the current view.
    /// </summary>
    /// <param name="bounds">An actor's bounding sphere to test for occlusion.</param>
    /// <returns>True if the bounds are occluded.</returns>
    bool CheckOcclusion(const BoundingSphere& bounds);
};
