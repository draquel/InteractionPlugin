#include "InteractionPlugin.h"

DEFINE_LOG_CATEGORY(LogIAP);

#define LOCTEXT_NAMESPACE "FInteractionPluginModule"

void FInteractionPluginModule::StartupModule()
{
}

void FInteractionPluginModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FInteractionPluginModule, InteractionPlugin)
