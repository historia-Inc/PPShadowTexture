// PPShadowTexture: publishes UE's own shadows as the UserSceneTexture "PPShadowTexture".
// The mask is built in the PostOpaque hook (where UE's shadow maps and VSM are complete) and
// registered at the start of each view's post process chain, before any material reads it.
#include "PPShadowTextureMask.h"

#include "Engine/Engine.h"
#include "EngineModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphBuilder.h"
#include "RendererInterface.h"
#include "RenderingThread.h"
#include "Runtime/Launch/Resources/Version.h"
#include "SceneRendering.h"
#include "SceneViewExtension.h"
#include "ShaderCore.h"

static_assert(ENGINE_MAJOR_VERSION == 5 && (ENGINE_MINOR_VERSION == 8 || ENGINE_MINOR_VERSION == 9),
	"PPShadowTexture reads renderer-private shadow structures and supports UE 5.8 and 5.9 only.");

namespace
{
struct FPPShadowTextureOutputs
{
	TMap<const FSceneView*, FRDGTextureRef> Masks;
};
}
RDG_REGISTER_BLACKBOARD_STRUCT(FPPShadowTextureOutputs);

namespace
{
class FPPShadowTextureViewExtension final : public FSceneViewExtensionBase
{
public:
	explicit FPPShadowTextureViewExtension(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister) {}

	virtual void SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View,
		FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override
	{
		// BeforeDOF view extension callbacks run before the earliest post process material location
		// (BL_SceneColorBeforeDOF), so every material location can read the texture.
		if (PassId == EPostProcessingPass::BeforeDOF && PPShadowTexture::ShouldProcessView(View))
		{
			InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FPPShadowTextureViewExtension::PublishMask_RenderThread));
		}
	}

	void PostOpaqueRender_RenderThread(FPostOpaqueRenderParameters& Parameters)
	{
		// The renderer delegate is global: it fires for every deferred view, including scene captures.
		if (!Parameters.GraphBuilder || !Parameters.View || !Parameters.DepthTexture || !Parameters.SceneTexturesUniformParams ||
			Parameters.DepthTexture->Desc.NumSamples != 1 || !PPShadowTexture::ShouldProcessView(*Parameters.View))
		{
			return;
		}
		FRDGBuilder& GraphBuilder = *Parameters.GraphBuilder;
		const PPShadowTexture::FMaskInputs Inputs{ Parameters.DepthTexture, Parameters.SceneTexturesUniformParams };
		if (FRDGTextureRef Mask = PPShadowTexture::AddMaskPasses(GraphBuilder, *Parameters.View, Inputs))
		{
			GraphBuilder.Blackboard.GetOrCreate<FPPShadowTextureOutputs>().Masks.Add(Parameters.View, Mask);
		}
	}

private:
	FScreenPassTexture PublishMask_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
	{
		// User scene textures are shared by the views of a family and the most recent entry wins, so the
		// mask is registered here, inside this view's own post process chain, not in PostOpaque.
		const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
		FRDGTextureRef Mask = nullptr;
		if (FPPShadowTextureOutputs* Outputs = GraphBuilder.Blackboard.GetMutable<FPPShadowTextureOutputs>())
		{
			Outputs->Masks.RemoveAndCopyValue(&View, Mask);
		}
		PPShadowTexture::RegisterUserSceneTexture(ViewInfo, Mask ? Mask : PPShadowTexture::CreateUnshadowedMask(GraphBuilder, ViewInfo));
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}
};

class FPPShadowTextureModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PPShadowTexture"));
		check(Plugin.IsValid());
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/PPShadowTexture"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));

		if (GEngine && GEngine->ViewExtensions.IsValid())
		{
			Register();
		}
		else
		{
			PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FPPShadowTextureModule::Register);
		}
	}

	virtual void ShutdownModule() override
	{
		if (PostEngineInitHandle.IsValid())
		{
			FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
			PostEngineInitHandle.Reset();
		}
		if (!ViewExtension.IsValid())
		{
			return;
		}
		// Stop new callbacks, then let queued render commands and RDG lambdas finish while this module is loaded.
		ViewExtension.Reset();
		FlushRenderingCommands();
		if (PostOpaqueHandle.IsValid())
		{
			GetRendererModule().RemovePostOpaqueRenderDelegate(PostOpaqueHandle);
			PostOpaqueHandle.Reset();
		}
	}

private:
	void Register()
	{
		if (PostEngineInitHandle.IsValid())
		{
			FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
			PostEngineInitHandle.Reset();
		}
		if (ViewExtension.IsValid() || !GEngine || !GEngine->ViewExtensions.IsValid())
		{
			return;
		}
		ViewExtension = FSceneViewExtensions::NewExtension<FPPShadowTextureViewExtension>();
		FlushRenderingCommands();
		PostOpaqueHandle = GetRendererModule().RegisterPostOpaqueRenderDelegate(
			FPostOpaqueRenderDelegate::CreateSP(ViewExtension.ToSharedRef(), &FPPShadowTextureViewExtension::PostOpaqueRender_RenderThread));
	}

	FDelegateHandle PostEngineInitHandle;
	FDelegateHandle PostOpaqueHandle;
	TSharedPtr<FPPShadowTextureViewExtension, ESPMode::ThreadSafe> ViewExtension;
};
}

IMPLEMENT_MODULE(FPPShadowTextureModule, PPShadowTexture)
