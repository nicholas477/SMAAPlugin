// Copyright Epic Games, Inc. All Rights Reserved.

#include "SMAAPlugin.h"

#include "SMAASceneExtension.h"
#include "SMAADeveloperSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FSMAAPluginModule"

void FSMAAPluginModule::StartupModule()
{
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("SMAAPlugin"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/SMAAPlugin"), PluginShaderDir);

	// Don't create an extension/load textures if there's no rendering
	if (!FApp::CanEverRender())
	{
		return;
	}

	FCoreDelegates::OnPostEngineInit.AddLambda([this]()
	{
		LoadTextures();

		UpdateExtensions();
	});
}

void FSMAAPluginModule::ShutdownModule()
{
	SMAASceneExtension.Reset();
	SMAAAreaTexture.Reset();
	SMAASearchTexture.Reset();
}

void FSMAAPluginModule::UpdateExtensions()
{
	if (!SMAASceneExtension.IsValid())
	{
		FTexture2DResource* AreaTextureResource = nullptr;
		FTexture2DResource* SearchTextureResource = nullptr;

		if (ensure(SMAAAreaTexture))
		{
			const auto TextureResource = SMAAAreaTexture->CreateResource();

			if (TextureResource)
			{
				AreaTextureResource = TextureResource->GetTexture2DResource();
			}
		}

		if (ensure(SMAASearchTexture))
		{
			const auto TextureResource = SMAASearchTexture->CreateResource();

			if (TextureResource)
			{
				SearchTextureResource = TextureResource->GetTexture2DResource();
			}
		}

		SMAASceneExtension = FSceneViewExtensions::NewExtension<FSMAASceneExtension>(AreaTextureResource, SearchTextureResource);
	}
}

void FSMAAPluginModule::LoadTextures()
{
	USMAADeveloperSettings* Settings = USMAADeveloperSettings::Get();

	if (SMAAAreaTexture == nullptr && !Settings->SMAAAreaTextureName.IsNull())
	{
		SMAAAreaTexture.Reset(Settings->SMAAAreaTextureName.LoadSynchronous());
	}

	if (SMAASearchTexture == nullptr && !Settings->SMAASearchTextureName.IsNull())
	{
		SMAASearchTexture.Reset(Settings->SMAASearchTextureName.LoadSynchronous());
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FSMAAPluginModule, SMAAPlugin)