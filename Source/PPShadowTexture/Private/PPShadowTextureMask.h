#pragma once

#include "RenderGraphFwd.h"
#include "UObject/NameTypes.h"

class FSceneTextureUniformParameters;
class FSceneView;
class FViewInfo;

namespace PPShadowTexture
{
/** Name of the UserSceneTexture the mask is published as: "PPShadowTexture". */
extern const FName TextureName;

/** What the mask passes read from the view, as PostOpaque hands it over. */
struct FMaskInputs
{
	FRDGTextureRef SceneDepth = nullptr;
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures = nullptr;
};

/** True for views this plugin handles: deferred SM5+ desktop views that run post processing. */
bool ShouldProcessView(const FSceneView& View);

/**
 * PostOpaque only (the deferred renderer is the active scene renderer there). Builds the R8G8 mask:
 * R = main directional light shadow, G = product of all shadowed local lights. 1 = lit, 0 = shadowed.
 * Returns nullptr when the view's scene renderer cannot be resolved.
 */
FRDGTextureRef AddMaskPasses(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FMaskInputs& Inputs);

/** A mask filled with 1 (no shadow), for views whose PostOpaque produced nothing. */
FRDGTextureRef CreateUnshadowedMask(FRDGBuilder& GraphBuilder, const FViewInfo& View);

/** Publishes Mask as the UserSceneTexture "PPShadowTexture". Call at the start of View's post process chain. */
void RegisterUserSceneTexture(const FViewInfo& View, FRDGTextureRef Mask);
}
