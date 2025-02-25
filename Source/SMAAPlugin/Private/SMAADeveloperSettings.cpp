// Fill out your copyright notice in the Description page of Project Settings.


#include "SMAADeveloperSettings.h"

USMAADeveloperSettings::USMAADeveloperSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SMAAAreaTextureName = FSoftObjectPath("/SMAAPlugin/AreaTex.AreaTex");
	SMAASearchTextureName = FSoftObjectPath("/SMAAPlugin/SearchTex.SearchTex");
}

void USMAADeveloperSettings::LoadTextures()
{
	if (SMAAAreaTexture == nullptr && !SMAAAreaTextureName.IsNull())
	{
		SMAAAreaTexture = SMAAAreaTextureName.LoadSynchronous();
		//SMAAAreaTexture->AddToRoot();
	}

	if (SMAASearchTexture == nullptr && !SMAASearchTextureName.IsNull())
	{
		SMAASearchTexture = SMAASearchTextureName.LoadSynchronous();
		//SMAASearchTexture->AddToRoot();
	}
}
