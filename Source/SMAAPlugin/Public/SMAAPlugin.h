// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FSMAASceneExtension;

class FSMAAPluginModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	void UpdateExtensions();
	void LoadTextures();

protected:
	TSharedPtr<FSMAASceneExtension> SMAASceneExtension;

	TStrongObjectPtr<class UTexture2D> SMAAAreaTexture;
	TStrongObjectPtr<class UTexture2D> SMAASearchTexture;
};
