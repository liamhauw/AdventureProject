// Fill out your copyright notice in the Description page of Project Settings.


#include "AdventureGameMode.h"
#include "Engine/Engine.h"

void AAdventureGameMode::StartPlay()
{
	Super::StartPlay();
  check(GEngine != nullptr);
  GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Green, TEXT("AdventureGameMode::StartPlay"));
}

