#pragma once
#include "../../pch.h"
#include "FortInventory.h"

enum class EFortWeaponUpgradeDirection : uint8
{
    NotSet = 0,
    Vertical = 1,
    Horizontal = 2,
};

// Row layout of /Game/Items/Datatables/AthenaWumbaData (FWeaponUpgradeItemRow)
struct FWeaponUpgradeItemRow
{
    uint8_t FTableRowBasePadding[0x8]; // inherited FTableRowBase vtable/padding

    UFortWeaponItemDefinition* CurrentWeaponDef;
    UFortWeaponItemDefinition* UpgradedWeaponDef;
    uint8_t WoodCost;
    uint8_t MetalCost;
    uint8_t BrickCost;
    EFortWeaponUpgradeDirection Direction;
};

class ABuildingItemWeaponUpgradeActor : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(ABuildingItemWeaponUpgradeActor);
};
