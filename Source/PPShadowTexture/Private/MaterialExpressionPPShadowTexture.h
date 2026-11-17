#pragma once

#include "Materials/MaterialExpressionUserSceneTexture.h"
#include "MaterialExpressionPPShadowTexture.generated.h"

/**
 * A UserSceneTexture node whose name is fixed to "PPShadowTexture" (R = main directional light shadow,
 * G = local light shadows, 1 = lit, 0 = shadowed). It derives from the engine node, so everything that
 * looks for UserSceneTexture nodes (material instance overrides, Custom node inputs, previews) sees it.
 */
UCLASS(MinimalAPI, collapsecategories, hidecategories = Object)
class UMaterialExpressionPPShadowTexture : public UMaterialExpressionUserSceneTexture
{
	GENERATED_BODY()

public:
	UMaterialExpressionPPShadowTexture(const FObjectInitializer& ObjectInitializer);

#if WITH_EDITOR
	//~ Begin UObject Interface
	virtual bool CanEditChange(const FProperty* InProperty) const override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	//~ End UObject Interface

	//~ Begin UMaterialExpression Interface
	// The engine class is MinimalAPI and does not export its overrides of these three, so a class in
	// another module has to override all of them to link.
	virtual void Build(MIR::FEmitter& Emitter) override;
	virtual int32 Compile(class FMaterialCompiler* Compiler, int32 OutputIndex) override;
	virtual void GetCaption(TArray<FString>& OutCaptions) const override;
	//~ End UMaterialExpression Interface
#endif
};
