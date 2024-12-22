// Copyright Epic Games, Inc. All Rights Reserved.

#include "SMAAPlugin.h"

#include "SMAASceneExtension.h"
#include "SMAADeveloperSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FSMAAPluginModule"

PRAGMA_DISABLE_OPTIMIZATION
void FSMAAPluginModule::StartupModule()
{
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("SMAAPlugin"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/SMAAPlugin"), PluginShaderDir);

	FCoreDelegates::OnPostEngineInit.AddLambda([this]()
	{
		USMAADeveloperSettings::Get()->LoadTextures();

		UpdateExtensions();
	});
}

void FSMAAPluginModule::ShutdownModule()
{
	SMAASceneExtension.Reset();
}

void FSMAAPluginModule::UpdateExtensions()
{
	if (!SMAASceneExtension.IsValid())
	{
		UTexture2D* AreaTexture = USMAADeveloperSettings::Get()->SMAAAreaTexture;
		UTexture2D* SearchTexture = USMAADeveloperSettings::Get()->SMAASearchTexture;
		FTexture2DResource* AreaTextureResource = nullptr;
		FTexture2DResource* SearchTextureResource = nullptr;

		if (ensure(AreaTexture))
		{
			//AreaTexture->UpdateResource();
			const auto TextureResource = AreaTexture->CreateResource();

			if (ensure(TextureResource))
			{
				AreaTextureResource = TextureResource->GetTexture2DResource();
			}
		}

		if (ensure(SearchTexture))
		{
			//SearchTexture->UpdateResource();
			const auto TextureResource = SearchTexture->CreateResource();

			if (ensure(TextureResource))
			{
				SearchTextureResource = TextureResource->GetTexture2DResource();
			}
		}

		SMAASceneExtension = FSceneViewExtensions::NewExtension<FSMAASceneExtension>(AreaTextureResource, SearchTextureResource);
	}
}
PRAGMA_ENABLE_OPTIMIZATION

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FSMAAPluginModule, SMAAPlugin)