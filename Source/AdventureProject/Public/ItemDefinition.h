// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Data/ItemTypes.h"
#include "ItemDefinition.generated.h"

class UStaticMesh;

// clangd expands this inside the class body, where UItemDefinition is still incomplete.
#if defined(__clang__)
#undef DEFINE_VTABLE_PTR_HELPER_CTOR_CALLER
#define DEFINE_VTABLE_PTR_HELPER_CTOR_CALLER(TClass) DEFINE_VTABLE_PTR_HELPER_CTOR_CALLER_DUMMY()
#endif

// Defines an item definition built from data table data.
UCLASS(BlueprintType, Blueprintable)
class ADVENTUREPROJECT_API UItemDefinition : public UDataAsset
{
	GENERATED_BODY()

public:

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item Data")
	FName ID;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item Data")
	EItemType ItemType;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item Data")
	FItemText ItemText;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item Data")
	TSoftObjectPtr<UStaticMesh> WorldMesh;
};