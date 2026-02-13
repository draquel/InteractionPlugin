// Copyright Daniel Raquel. All Rights Reserved.

#include "UI/InteractionPromptWidget.h"
#include "Components/InteractableComponent.h"
#include "Types/CGFInteractionTypes.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/Spacer.h"
#include "Blueprint/WidgetTree.h"

void UInteractionPromptWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildWidgetTree();
}

void UInteractionPromptWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// Start collapsed
	SetVisibility(ESlateVisibility::Collapsed);
}

void UInteractionPromptWidget::BuildWidgetTree()
{
	if (!WidgetTree)
	{
		return;
	}

	// Root: UBorder
	RootBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("PromptBorder"));
	RootBorder->SetBrush(PromptBackgroundBrush);
	RootBorder->SetBrushColor(PromptBackgroundTint);
	RootBorder->SetPadding(FMargin(12.f, 6.f));
	WidgetTree->RootWidget = RootBorder;

	// Horizontal layout: [key icon area] [spacer] [action text]
	UHorizontalBox* HBox = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("PromptHBox"));
	RootBorder->AddChild(HBox);

	// Key icon area: Overlay with icon brush + key text
	UOverlay* KeyOverlay = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("KeyOverlay"));

	UImage* KeyBg = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("KeyBgImage"));
	KeyBg->SetBrush(KeyIconBrush);
	UOverlaySlot* KeyBgSlot = KeyOverlay->AddChildToOverlay(KeyBg);
	if (KeyBgSlot)
	{
		KeyBgSlot->SetHorizontalAlignment(HAlign_Center);
		KeyBgSlot->SetVerticalAlignment(VAlign_Center);
	}

	KeyLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("KeyText"));
	KeyLabel->SetText(DefaultKeyText);
	KeyLabel->SetColorAndOpacity(FSlateColor(KeyTextColor));
	if (KeyTextFont.HasValidFont())
	{
		KeyLabel->SetFont(KeyTextFont);
	}
	UOverlaySlot* KeyTextSlot = KeyOverlay->AddChildToOverlay(KeyLabel);
	if (KeyTextSlot)
	{
		KeyTextSlot->SetHorizontalAlignment(HAlign_Center);
		KeyTextSlot->SetVerticalAlignment(VAlign_Center);
	}

	UHorizontalBoxSlot* KeyHBSlot = HBox->AddChildToHorizontalBox(KeyOverlay);
	if (KeyHBSlot)
	{
		KeyHBSlot->SetVerticalAlignment(VAlign_Center);
	}

	// Spacer
	USpacer* Spacer = WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass(), TEXT("PromptSpacer"));
	Spacer->SetSize(FVector2D(8.f, 0.f));
	HBox->AddChildToHorizontalBox(Spacer);

	// Action text
	ActionLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ActionText"));
	ActionLabel->SetText(NSLOCTEXT("InteractionPrompt", "DefaultAction", "Interact"));
	ActionLabel->SetColorAndOpacity(FSlateColor(ActionTextColor));
	if (ActionTextFont.HasValidFont())
	{
		ActionLabel->SetFont(ActionTextFont);
	}
	UHorizontalBoxSlot* ActionSlot = HBox->AddChildToHorizontalBox(ActionLabel);
	if (ActionSlot)
	{
		ActionSlot->SetVerticalAlignment(VAlign_Center);
	}
}

void UInteractionPromptWidget::ShowPromptForActor(AActor* InteractableActor)
{
	if (!InteractableActor)
	{
		HidePrompt();
		return;
	}

	// Read interaction options from the interactable component
	UInteractableComponent* Interactable = InteractableActor->FindComponentByClass<UInteractableComponent>();
	if (!Interactable)
	{
		HidePrompt();
		return;
	}

	const TArray<FInteractionOption>& Options = Interactable->InteractionOptions;
	if (Options.Num() > 0 && ActionLabel)
	{
		const FInteractionOption& FirstOption = Options[0];
		if (!FirstOption.DisplayText.IsEmpty())
		{
			ActionLabel->SetText(FirstOption.DisplayText);
		}
		else
		{
			ActionLabel->SetText(NSLOCTEXT("InteractionPrompt", "DefaultAction", "Interact"));
		}
	}

	SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UInteractionPromptWidget::HidePrompt()
{
	SetVisibility(ESlateVisibility::Collapsed);
}

bool UInteractionPromptWidget::IsPromptVisible() const
{
	return GetVisibility() != ESlateVisibility::Collapsed && GetVisibility() != ESlateVisibility::Hidden;
}
