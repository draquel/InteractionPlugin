// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "InteractionPromptWidget.generated.h"

class UBorder;
class UHorizontalBox;
class UImage;
class UTextBlock;
class USpacer;
class UOverlay;

/**
 * Interaction prompt widget ("Press E to Pick Up").
 *
 * Shows/hides based on whether the player's InteractionComponent
 * has detected a nearby interactable. Reads the first FInteractionOption's
 * DisplayText for the action label.
 */
UCLASS(BlueprintType, Blueprintable)
class INTERACTIONPLUGIN_API UInteractionPromptWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// --- Style ---

	/** Background brush for the prompt container. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InteractionPrompt|Style")
	FSlateBrush PromptBackgroundBrush;

	/** Brush for the key icon frame (the button shape behind "E"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InteractionPrompt|Style")
	FSlateBrush KeyIconBrush;

	/** Background tint color. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InteractionPrompt|Style")
	FLinearColor PromptBackgroundTint = FLinearColor(0.02f, 0.02f, 0.05f, 0.8f);

	/** Color for the action text (e.g., "Pick Up"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InteractionPrompt|Style")
	FLinearColor ActionTextColor = FLinearColor::White;

	/** Color for the key text (e.g., "E"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InteractionPrompt|Style")
	FLinearColor KeyTextColor = FLinearColor::White;

	/** Font for the action text. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InteractionPrompt|Style")
	FSlateFontInfo ActionTextFont;

	/** Font for the key label. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InteractionPrompt|Style")
	FSlateFontInfo KeyTextFont;

	/** Default key label text. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InteractionPrompt|Style")
	FText DefaultKeyText = NSLOCTEXT("InteractionPrompt", "DefaultKey", "E");

	// --- API ---

	/** Show the prompt for the given interactable actor. Reads interaction options for text. */
	UFUNCTION(BlueprintCallable, Category = "InteractionPrompt")
	void ShowPromptForActor(AActor* InteractableActor);

	/** Hide the prompt. */
	UFUNCTION(BlueprintCallable, Category = "InteractionPrompt")
	void HidePrompt();

	/** Returns true if the prompt is currently visible. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "InteractionPrompt")
	bool IsPromptVisible() const;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;

private:
	void BuildWidgetTree();

	UPROPERTY()
	TObjectPtr<UBorder> RootBorder;

	UPROPERTY()
	TObjectPtr<UTextBlock> KeyLabel;

	UPROPERTY()
	TObjectPtr<UTextBlock> ActionLabel;
};
