#pragma once

#include "Looting/ObjectType.h"

// object glow reasons, in descending order of precedence
enum class GlowReason {
	LockedContainer = 1,
	BossContainer,
	QuestObject,
	Collectible,
	Valuable,
	EnchantedItem,
	PlayerProperty,
	SimpleTarget,
	None
};

inline GlowReason CycleGlow(const GlowReason current)
{
	int next(int(current) + 1);
	if (next > int(GlowReason::SimpleTarget))
		return GlowReason::LockedContainer;
	return GlowReason(next);
}

inline std::string GlowName(const GlowReason glow)
{
	switch (glow) {
	case GlowReason::LockedContainer:
		return "Locked";
	case GlowReason::BossContainer:
		return "Boss";
	case GlowReason::QuestObject:
		return "Quest";
	case GlowReason::Collectible:
		return "Collectible";
	case GlowReason::EnchantedItem:
		return "Enchanted";
	case GlowReason::Valuable:
		return "Valuable";
	case GlowReason::PlayerProperty:
		return "PlayerOwned";
	case GlowReason::SimpleTarget:
		return "Looted";
	default:
		return "Unknown";
	}
}

enum class LootingType {
	LeaveBehind = 0,
	LootAlwaysSilent,
	LootAlwaysNotify,
	LootIfValuableEnoughSilent,
	LootIfValuableEnoughNotify,
	MAX
};

inline bool LootingRequiresNotification(const LootingType lootingType)
{
	return lootingType == LootingType::LootIfValuableEnoughNotify || lootingType == LootingType::LootAlwaysNotify;
}

inline LootingType LootingTypeFromIniSetting(const double iniSetting)
{
	UInt32 intSetting(static_cast<UInt32>(iniSetting));
	if (intSetting >= static_cast<SInt32>(LootingType::MAX))
	{
		return LootingType::LeaveBehind;
	}
	return static_cast<LootingType>(intSetting);
}

enum class SpecialObjectHandling {
	DoNotLoot = 0,
	DoLoot,
	GlowTarget,
	MAX
};

constexpr std::pair<bool, SpecialObjectHandling> NotCollectible = { false, SpecialObjectHandling::DoNotLoot };

inline SpecialObjectHandling UpdateSpecialObjectHandling(const SpecialObjectHandling initial, const SpecialObjectHandling next)
{
	// update if new is more permissive
	if (next == SpecialObjectHandling::DoLoot)
	{
		return next;
	}
	else if (next == SpecialObjectHandling::GlowTarget)
	{
		return initial == SpecialObjectHandling::DoLoot ? initial : next;
	}
	else
	{
		// this is the least permissive - initial cannot be any less so
		return initial;
	}
}

inline bool IsSpecialObjectLootable(const SpecialObjectHandling specialObjectHandling)
{
	return specialObjectHandling == SpecialObjectHandling::DoLoot;
}

inline std::string SpecialObjectHandlingJSON(const SpecialObjectHandling specialObjectHandling)
{
	switch (specialObjectHandling) {
	case SpecialObjectHandling::DoLoot:
		return "take";
	case SpecialObjectHandling::GlowTarget:
		return "glow";
	case SpecialObjectHandling::DoNotLoot:
	default:
		return "leave";
	}
}

inline SpecialObjectHandling ParseSpecialObjectHandling(const std::string& action)
{
	if (action == "take")
		return SpecialObjectHandling::DoLoot;
	if (action == "glow")
		return SpecialObjectHandling::GlowTarget;
	return SpecialObjectHandling::DoNotLoot;
}

inline SpecialObjectHandling SpecialObjectHandlingFromIniSetting(const double iniSetting)
{
	UInt32 intSetting(static_cast<UInt32>(iniSetting));
	if (intSetting >= static_cast<SInt32>(SpecialObjectHandling::MAX))
	{
		return SpecialObjectHandling::DoNotLoot;
	}
	return static_cast<SpecialObjectHandling>(intSetting);
}

inline bool LootingDependsOnValueWeight(const LootingType lootingType, ObjectType objectType)
{
	if (objectType == ObjectType::septims ||
		objectType == ObjectType::key ||
		objectType == ObjectType::oreVein ||
		objectType == ObjectType::ammo ||
		objectType == ObjectType::lockpick)
		return false;
	if (lootingType != LootingType::LootIfValuableEnoughNotify && lootingType != LootingType::LootIfValuableEnoughSilent)
		return false;
	return true;
}

enum class DeadBodyLooting {
	DoNotLoot = 0,
	LootExcludingArmor,
	LootAll,
	MAX
};

inline DeadBodyLooting DeadBodyLootingFromIniSetting(const double iniSetting)
{
	UInt32 intSetting(static_cast<UInt32>(iniSetting));
	if (intSetting >= static_cast<SInt32>(DeadBodyLooting::MAX))
	{
		return DeadBodyLooting::DoNotLoot;
	}
	return static_cast<DeadBodyLooting>(intSetting);
}
