#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/** The module's log category (InteractionPlugin rule: one per module, with the module prefix). */
DECLARE_LOG_CATEGORY_EXTERN(LogIAP, Log, All);

class FInteractionPluginModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
