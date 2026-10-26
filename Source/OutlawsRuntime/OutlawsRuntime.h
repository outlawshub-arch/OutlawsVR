#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "OutlawsShipLog.h"            // (REQ-SHIPLOG: in a Shipping build the UE_LOG lines below go to the game's own log file)

DECLARE_LOG_CATEGORY_EXTERN(LogOutlaws, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LogOutlawsCoop, Log, All);      // co-op and multiplayer: the OutlawsCoop*, remote player, beacon, UPnP files
DECLARE_LOG_CATEGORY_EXTERN(LogOutlawsAI, Log, All);        // the bad guys (OutlawsEnemy*) and the bots (OutlawsCoopBots)

class FOutlawsRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
