// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SMAADeveloperSettings.generated.h"

class UTexture2D;

/**
 * 
 */
UCLASS(config = Engine, defaultconfig)
class SMAAPLUGIN_API USMAADeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()
	
public:
	USMAADeveloperSettings(const FObjectInitializer& ObjectInitializer);

	///** Path of the Area Texture used by SMAA. */
	UPROPERTY(globalconfig, EditAnywhere, Category = "SMAA", meta = (ConfigRestartRequired = true))
	TSoftObjectPtr<UTexture2D> SMAAAreaTextureName;

	///** Path of the Search Texture used by SMAA. */
	UPROPERTY(globalconfig, EditAnywhere, Category = "SMAA", meta=(ConfigRestartRequired = true))
	TSoftObjectPtr<UTexture2D> SMAASearchTextureName;

	static USMAADeveloperSettings* Get() { return GetMutableDefault<USMAADeveloperSettings>(); }
};
