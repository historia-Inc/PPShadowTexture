#include "MaterialExpressionPPShadowTexture.h"

#include "PPShadowTextureMask.h"

#if WITH_EDITOR
#include "MaterialCompiler.h"
#include "UObject/UnrealType.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(MaterialExpressionPPShadowTexture)

UMaterialExpressionPPShadowTexture::UMaterialExpressionPPShadowTexture(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	UserSceneTexture = PPShadowTexture::TextureName;
}

#if WITH_EDITOR
bool UMaterialExpressionPPShadowTexture::CanEditChange(const FProperty* InProperty) const
{
	return Super::CanEditChange(InProperty) &&
		!(InProperty && InProperty->GetFName() == GET_MEMBER_NAME_CHECKED(UMaterialExpressionUserSceneTexture, UserSceneTexture));
}

void UMaterialExpressionPPShadowTexture::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	// The name is also editable through the node's advanced pin, which does not ask CanEditChange.
	UserSceneTexture = PPShadowTexture::TextureName;
	Super::PostEditChangeProperty(PropertyChangedEvent);
}

void UMaterialExpressionPPShadowTexture::Build(MIR::FEmitter& Emitter)
{
	// The new material translator (r.Material.Translator.EnableNew) reads user scene textures through an extern
	// that is private to the Engine module. Fall back to the base class, which reports the node as unsupported.
	UMaterialExpression::Build(Emitter);
}

int32 UMaterialExpressionPPShadowTexture::Compile(FMaterialCompiler* Compiler, int32 OutputIndex)
{
	// The material registers the texture by name; the renderer binds whatever the plugin published under it.
	const int32 TextureIndex = Compiler->FindOrAddUserSceneTexture(UserSceneTexture);
	if (TextureIndex == INDEX_NONE)
	{
		return Compiler->Errorf(TEXT("PPShadowTexture: the material has no free UserSceneTexture slot left"));
	}

	// Outputs are those of the UserSceneTexture node: 0 Color, 1 Size, 2 InvSize. Index 3 has no pin; a Custom node
	// uses it for an input it declares but never reads, which needs the texture bound but no sample.
	switch (OutputIndex)
	{
	case 0:
	case 3:
	{
		const bool bHasCoordinates = Coordinates.GetTracedInput().Expression != nullptr;
		const int32 UV = bHasCoordinates ? Coordinates.Compile(Compiler) : INDEX_NONE;	// none: the pixel's own position
		const bool bSampleUnused = OutputIndex == 3;
		return Compiler->SceneTextureLookup(UV, TextureIndex, bFiltered, bClamped, bSampleUnused);
	}
	case 1:
		return Compiler->GetSceneTextureViewSize(TextureIndex, /*InvProperty=*/ false);
	case 2:
		return Compiler->GetSceneTextureViewSize(TextureIndex, /*InvProperty=*/ true);
	default:
		return Compiler->Errorf(TEXT("PPShadowTexture: unknown output"));
	}
}

void UMaterialExpressionPPShadowTexture::GetCaption(TArray<FString>& OutCaptions) const
{
	OutCaptions.Add(PPShadowTexture::TextureName.ToString());
}
#endif // WITH_EDITOR
