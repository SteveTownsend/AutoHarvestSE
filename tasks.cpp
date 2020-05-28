#include "PrecompiledHeaders.h"

#include "tasks.h"
#include "objects.h"
#include "dataCase.h"
#include "iniSettings.h"
#include "basketfile.h"
#include "debugs.h"
#include "papyrus.h"
#include "PlayerCellHelper.h"
#include "LogStackWalker.h"

#include <chrono>
#include <thread>

INIFile* SearchTask::m_ini = nullptr;
RE::BGSRefAlias* SearchTask::m_eventTarget = nullptr;

RecursiveLock SearchTask::m_lock;
std::unordered_map<const RE::TESObjectREFR*, std::chrono::time_point<std::chrono::high_resolution_clock>> SearchTask::m_glowExpiration;

// special object glow - not too long, in case we loot or move away
const int SearchTask::ObjectGlowDurationLootedSeconds = 2;
const int SearchTask::ObjectGlowDurationSpecialSeconds = 10;

SKSE::RegistrationSet<RE::TESObjectREFR*> onGetCritterIngredient("OnGetCritterIngredient");
SKSE::RegistrationSet<int> onCarryWeightDelta("OnCarryWeightDelta");
SKSE::RegistrationSet<> onResetCarryWeight("OnResetCarryWeight");
SKSE::RegistrationSet<RE::TESObjectREFR*, int, int, bool, bool, bool> onAutoHarvest("OnAutoHarvest");
SKSE::RegistrationSet<RE::TESObjectREFR*, RE::TESForm*, int> onLootFromNPC("OnLootFromNPC");
SKSE::RegistrationSet<RE::TESObjectREFR*, int, int> onObjectGlow("OnObjectGlow");

RE::BGSRefAlias* GetScriptTarget(const char* espName, UInt32 questID)
{
	static RE::TESQuest* quest = nullptr;
	static RE::BGSRefAlias* alias = nullptr;
#if _DEBUG
	static bool listed(false);
#endif
	if (!quest)
	{
		UInt32 formID = 0;
		std::optional<UInt8> idx = RE::TESDataHandler::GetSingleton()->GetLoadedModIndex(espName);
		if (idx.has_value())
		{
			formID = (idx.value() << 24) | questID;
#if _DEBUG
			_DMESSAGE("Got formID for questID %08.2x", questID);
#endif
		}
#if _DEBUG
		else if (!listed)
		{
			for (const auto& nextFile : RE::TESDataHandler::GetSingleton()->compiledFileCollection.files)
			{
				_DMESSAGE("Mod loaded %s", &nextFile->fileName);
			}
			listed = true;
		}
#endif
		if (formID != 0)
		{
			RE::TESForm* questForm = RE::TESForm::LookupByID(formID);
#if _DEBUG
			_DMESSAGE("Got Base Form %s", questForm ? questForm->GetFormEditorID() : "nullptr");
#endif
			quest = questForm ? questForm->As<RE::TESQuest>() : nullptr;
#if _DEBUG
			_DMESSAGE("Got Quest Form %s", questForm->As<RE::TESQuest>() ? questForm->GetFormEditorID() : "nullptr");
#endif
		}
	}
	if (quest && quest->IsRunning())
	{
#if _DEBUG
		_DMESSAGE("Quest %s is running", quest->GetFormEditorID());
#endif
		RE::BGSBaseAlias* baseAlias(quest->aliases[0]);
		if (!baseAlias)
		{
#if _DEBUG
			_DMESSAGE("Quest has no alias at index 0");
#endif
			return nullptr;
		}

		alias = static_cast<RE::BGSRefAlias*>(baseAlias);
		if (!alias)
		{
#if _DEBUG
			_DMESSAGE("Quest is not type BGSRefAlias");
#endif
			return nullptr;
		}
#if _DEBUG
		_MESSAGE("Got BGSRefAlias for Mod's Quest");
#endif
	}
	return alias;
}

bool SearchTask::GoodToGo()
{
	if (!m_eventTarget)
	{
		m_eventTarget = GetScriptTarget(MODNAME, QUEST_ID);
		// register the events
		if (m_eventTarget)
		{
			onGetCritterIngredient.Register(m_eventTarget);
			onCarryWeightDelta.Register(m_eventTarget);
			onResetCarryWeight.Register(m_eventTarget);
			onObjectGlow.Register(m_eventTarget);
			onAutoHarvest.Register(m_eventTarget);
			onLootFromNPC.Register(m_eventTarget);
		}
	}
	return m_eventTarget != nullptr;
}

SearchTask::SearchTask(RE::TESObjectREFR* candidate, INIFile::SecondaryType targetType)
	: m_candidate(candidate), m_targetType(targetType)
{
}

RE::TESForm* GetCellOwner(RE::TESObjectCELL* cell)
{
	for (RE::BSExtraData& extraData : cell->extraList)
	{
		if (extraData.GetType() == RE::ExtraDataType::kOwnership)
		{
#if _DEBUG
			_MESSAGE("GetCellOwner Hit %08x", reinterpret_cast<RE::ExtraOwnership&>(extraData).owner->formID);
#endif
			return reinterpret_cast<RE::ExtraOwnership&>(extraData).owner;
		}
	}
	return nullptr;
}

bool IsCellPlayerOwned(RE::TESObjectCELL* cell)
{
	if (!cell)
		return false;
	RE::TESForm* owner = GetCellOwner(cell);
	if (!owner)
		return false;
	if (owner->formType == RE::FormType::NPC)
	{
		const RE::TESNPC* npc = owner->As<RE::TESNPC>();
		RE::TESNPC* playerBase = RE::PlayerCharacter::GetSingleton()->GetActorBase();
		return (npc && npc == playerBase);
	}
	else if (owner->formType == RE::FormType::Faction)
	{
		RE::TESFaction* faction = owner->As<RE::TESFaction>();
		if (faction)
		{
			if (RE::PlayerCharacter::GetSingleton()->IsInFaction(faction))
				return true;

			return false;
		}
	}
	return false;
}

const int AutoHarvestSpamLimit = 10;

bool SearchTask::IsLootingForbidden()
{
	bool isForbidden(false);
	// Perform crime checks - this is done after checks for quest object glowing, as many quest-related objects are owned.
	// Ownership expires with the target, e.g. Francis the Horse from Interesting NPCs was killed by a wolf in Solitude
	// and becomes lootable thereafter.
	// For non-dead targets, check law-abiding settings vs criminality of target and player-ownership settings vs ownership
	if (m_targetType != INIFile::SecondaryType::deadbodies)
	{
		// check up to three ownership conditions depending on config
		bool playerOwned(TESObjectREFRHelper(m_candidate).IsPlayerOwned());
		bool lootingIsCrime(m_candidate->IsOffLimits());
		if (m_playerCellSelfOwned || playerOwned)
		{
			// can configure to not loot my own belongings even though it's always legal
			if (!IsSpecialObjectLootable(m_belongingsCheck))
			{
#if _DEBUG
				_DMESSAGE("Player home or player-owned, looting belongings disallowed: %s/0x%08x",
					m_candidate->data.objectReference->GetName(), m_candidate->data.objectReference->formID);
#endif
				isForbidden = true;
				// Glow if configured
				if (m_belongingsCheck == SpecialObjectHandling::GlowTarget)
					UpdateGlowReason(GlowReason::PlayerProperty);
			}
		}
		// if restricted to law-abiding citizenship, check if OK to loot
		else if (m_crimeCheck > 0)
		{
			if (m_candidate->IsOffLimits())
			{
				// never commit a crime unless crimeCheck is 0
#if _DEBUG
				_DMESSAGE("Crime to loot REFR, cannot loot");
#endif
				isForbidden = true;
			}
			else if (m_crimeCheck == 2 && m_candidate->GetOwner() != nullptr)
			{
				// owner cannot be player by construction, disallow
#if _DEBUG
				_DMESSAGE("REFR is owned, cannot loot");
#endif
				isForbidden = true;
			}
		}

		if (isForbidden)
		{
#if _DEBUG
			_MESSAGE("Skip owned/illegal-to-loot REFR: %s/0x%08x", m_candidate->data.objectReference->GetName(), m_candidate->data.objectReference->formID);
#endif
		}
	}
	return isForbidden;
}

bool SearchTask::IsBookGlowable() const
{
	RE::BGSKeywordForm* keywordForm(m_candidate->data.objectReference->As<RE::BGSKeywordForm>());
	if (!keywordForm)
		return false;
	for (UInt32 index = 0; index < keywordForm->GetNumKeywords(); ++index)
	{
		std::optional<RE::BGSKeyword*> keyword(keywordForm->GetKeywordAt(index));
		if (!keyword || !keyword.has_value())
			continue;
		if (DataCase::GetInstance()->IsBookGlowableKeyword(keyword.value()))
			return true;
	}
	return false;
}

// Dynamic REFR looting is not delayed - the visuals may be less appealing, but delaying risks CTD as REFRs can
// be recycled very quickly.
bool SearchTask::HasDynamicData(RE::TESObjectREFR* refr)
{
	// do not reregister known REFR
	if (IsLootedDynamicContainer(refr))
		return true;

	// risk exists if REFR or its concrete object is dynamic
	if (refr->IsDynamicForm() || refr->GetBaseObject()->IsDynamicForm())
	{
		_DMESSAGE("dynamic REFR 0x%08x or base 0x%08x for %s", refr->GetFormID(),
			refr->GetBaseObject()->GetFormID(), refr->GetBaseObject()->GetName());
		// record looting so we don't rescan
		MarkDynamicContainerLooted(refr);
		return true;
	}
	return false;
}

std::unordered_map<RE::TESObjectREFR*, RE::FormID> SearchTask::m_lootedDynamicContainers;
void SearchTask::MarkDynamicContainerLooted(RE::TESObjectREFR* refr)
{
	RecursiveLockGuard guard(m_lock);
	// record looting so we don't rescan
	m_lootedDynamicContainers.insert(std::make_pair(refr, refr->GetFormID()));
}

RE::FormID SearchTask::IsLootedDynamicContainer(RE::TESObjectREFR* refr)
{
	if (!refr)
		return false;
	RecursiveLockGuard guard(m_lock);
	if (m_lootedDynamicContainers.count(refr) > 0)
		return m_lootedDynamicContainers[refr];
	return RE::FormID(0);
}

// forget about dynamic containers we looted when cell changes. This is more aggressive than static container looting
// as this list contains recycled FormIDs, and hypothetically may grow unbounded.
void SearchTask::ResetLootedDynamicContainers()
{
	RecursiveLockGuard guard(m_lock);
	m_lootedDynamicContainers.clear();
}

std::unordered_set<RE::TESObjectREFR*> SearchTask::m_lootedContainers;
void SearchTask::MarkContainerLooted(RE::TESObjectREFR* refr)
{
	RecursiveLockGuard guard(m_lock);
	// record looting so we don't rescan
	m_lootedContainers.insert(refr);
}

bool SearchTask::IsLootedContainer(RE::TESObjectREFR* refr)
{
	if (!refr)
		return false;
	RecursiveLockGuard guard(m_lock);
	return m_lootedContainers.count(refr) > 0;
}

// forget about containers we looted to allow rescan after game load or config settings update
void SearchTask::ResetLootedContainers()
{
	RecursiveLockGuard guard(m_lock);
	m_lootedContainers.clear();
	m_actorApparentTimeOfDeath.clear();
}

// looting during combat is unstable, so if that option is enabled, we store the combat victims and loot them once combat ends, no sooner 
// than N seconds after their death
void SearchTask::RegisterActorTimeOfDeath(RE::TESObjectREFR* refr)
{
	RecursiveLockGuard guard(m_lock);
	m_actorApparentTimeOfDeath.emplace_back(std::make_pair(refr, std::chrono::high_resolution_clock::now()));
	// record looting so we don't rescan
	MarkContainerLooted(refr);
#if _DEBUG
	_DMESSAGE("Enqueued dead body to loot later 0x%08x", refr->GetFormID());
#endif
}

std::deque<std::pair<RE::TESObjectREFR*, std::chrono::time_point<std::chrono::high_resolution_clock>>> SearchTask::m_actorApparentTimeOfDeath;
const int SearchTask::ActorReallyDeadWaitIntervalSeconds = 3;

void SearchTask::ReleaseReliablyDeadActors()
{
	RecursiveLockGuard guard(m_lock);
	const auto cutoffPoint(std::chrono::high_resolution_clock::now() - std::chrono::milliseconds(static_cast<long long>(ActorReallyDeadWaitIntervalSeconds * 1000.0)));
	while (!m_actorApparentTimeOfDeath.empty() && m_actorApparentTimeOfDeath.front().second <= cutoffPoint)
	{
		// this actor died long enough ago that we trust actor->GetContainer not to crash, provided the ID is still usable
		RE::TESObjectREFR* refr(m_actorApparentTimeOfDeath.front().first);
		if (!RE::TESForm::LookupByID<RE::TESObjectREFR>(refr->GetFormID()))
		{
#if _DEBUG
			_DMESSAGE("Process enqueued dead body 0x%08x", refr->GetFormID());
#endif
		}
		else
		{
#if _DEBUG
			_DMESSAGE("Suspect enqueued dead body ID 0x%08x", refr->GetFormID());
#endif
		}
		m_refs.push_back(refr);
		m_actorApparentTimeOfDeath.pop_front();
	}
}

void SearchTask::Run()
{
	DataCase* data = DataCase::GetInstance();
	TESObjectREFRHelper refrEx(m_candidate);

	if (m_targetType == INIFile::SecondaryType::itemObjects)
	{
		ObjectType objType = refrEx.GetObjectType();
		std::string typeName = refrEx.GetTypeName();
		// Various form types contain an ingredient that is the final lootable item - resolve here
		RE::TESForm* lootable(DataCase::GetInstance()->GetLootableForProducer(m_candidate->data.objectReference));
		if (lootable)
		{
#if _DEBUG
			_DMESSAGE("producer %s/0x%08x has lootable %s/0x%08x", m_candidate->data.objectReference->GetName(), m_candidate->data.objectReference->formID,
				lootable->GetName(), lootable->formID);
#endif
			refrEx.SetLootable(lootable);
		}
		else if (objType == ObjectType::critter)
		{
			// trigger critter -> ingredient resolution and skip until it's resolved - pending resolve recorded using nullptr,
			// only trigger if not already pending
#if _DEBUG
			_DMESSAGE("resolve critter %s/0x%08x to ingredient", m_candidate->data.objectReference->GetName(), m_candidate->data.objectReference->formID);
#endif
			if (DataCase::GetInstance()->SetLootableForProducer(m_candidate->data.objectReference, nullptr))
			{
				TriggerGetCritterIngredient();
			}
			return;
		}

		if (objType == ObjectType::unknown)
		{
#if _DEBUG
			_DMESSAGE("blacklist objType == ObjectType::unknown for 0x%08x", m_candidate->GetFormID());
#endif
			data->BlacklistReference(m_candidate);
			return;
		}

		bool manualLootNotify(m_ini->GetSetting(INIFile::autoharvest, INIFile::config, "ManualLootTargetNotify") != 0);
		if (objType == ObjectType::manualLoot && manualLootNotify)
		{
			// notify about these, just once
			std::string notificationText;
			static RE::BSFixedString manualLootText(papyrus::GetTranslation(nullptr, RE::BSFixedString("$AHSE_MANUAL_LOOT_MSG")));
			if (!manualLootText.empty())
			{
				notificationText = manualLootText;
				Replace(notificationText, "{ITEMNAME}", m_candidate->GetName());
				if (!notificationText.empty())
				{
					RE::DebugNotification(notificationText.c_str());
				}
			}
#if _DEBUG
			_DMESSAGE("notify, then block objType == ObjectType::manualLoot for 0x%08x", m_candidate->GetFormID());
#endif
			data->BlockReference(m_candidate);
			return;
		}

		if (BasketFile::GetSingleton()->IsinList(BasketFile::EXCLUDELIST, m_candidate->data.objectReference))
		{
#if _DEBUG
			_DMESSAGE("blacklist form in exclude list for 0x%08x", m_candidate->data.objectReference->GetFormID());
#endif
			data->BlockForm(m_candidate->data.objectReference);
			return;
		}

#if _DEBUG
		_MESSAGE("typeName  %s", typeName.c_str());
		DumpReference(refrEx, typeName.c_str());
#endif

		// initially no glow - use synthetic value with highest precedence
		m_glowReason = GlowReason::None;
		bool skipLooting(false);

		bool needsFullQuestFlags(m_ini->GetSetting(INIFile::autoharvest, INIFile::config, "questObjectScope") != 0);
		SpecialObjectHandling questObjectLoot =
			SpecialObjectHandlingFromIniSetting(m_ini->GetSetting(INIFile::autoharvest, INIFile::config, "questObjectLoot"));
		if (refrEx.IsQuestItem(needsFullQuestFlags))
		{
#if _DEBUG
			_DMESSAGE("Quest Item 0x%08x", m_candidate->data.objectReference->formID);
#endif
			if (questObjectLoot == SpecialObjectHandling::GlowTarget)
			{
#if _DEBUG
				_DMESSAGE("glow quest object %s/0x%08x", m_candidate->data.objectReference->GetName(), m_candidate->data.objectReference->formID);
#endif
				UpdateGlowReason(GlowReason::QuestObject);
			}

			skipLooting = skipLooting || !IsSpecialObjectLootable(questObjectLoot);
		}
		// glow unread notes as they are often quest-related
		else if (questObjectLoot == SpecialObjectHandling::GlowTarget && objType == ObjectType::books && IsBookGlowable())
		{
#if _DEBUG
			_DMESSAGE("Glowable book 0x%08x", m_candidate->data.objectReference->formID);
#endif
			UpdateGlowReason(GlowReason::SimpleTarget);
		}

		if (objType == ObjectType::ammo)
		{
			skipLooting = skipLooting || data->SkipAmmoLooting(m_candidate);
		}

		// order is important to ensure we glow correctly even if blocked
		skipLooting = IsLootingForbidden() || skipLooting;

		if (m_glowReason != GlowReason::None)
		{
			TriggerObjectGlow(m_candidate, ObjectGlowDurationSpecialSeconds);
		}

		if (IsLocationExcluded())
		{
#if _DEBUG
			_DMESSAGE("Player location is excluded");
#endif
			skipLooting = true;
		}

		if (IsPopulationCenterExcluded())
		{
#if _DEBUG
			_DMESSAGE("Player location is excluded as unpermitted population center");
#endif
			skipLooting = true;
		}

		LootingType lootingType(LootingTypeFromIniSetting(m_ini->GetSetting(INIFile::autoharvest, INIFile::itemObjects, typeName.c_str())));
		if (!skipLooting)
		{
			if (lootingType == LootingType::LeaveBehind)
			{
#if _DEBUG
				_MESSAGE("Block REFR : LeaveBehind for 0x%08x", m_candidate->data.objectReference->formID);
#endif
				data->BlockReference(m_candidate);
				skipLooting = true;
			}
			else if (LootingDependsOnValueWeight(lootingType, objType) && TESFormHelper(m_candidate->data.objectReference).ValueWeightTooLowToLoot(m_ini))
			{
#if _DEBUG
				_DMESSAGE("block - v/w excludes harvest for 0x%08x", m_candidate->data.objectReference->formID);
#endif
				data->BlockForm(m_candidate->data.objectReference);
				skipLooting = true;
			}
		}

		if (skipLooting)
			return;

		// don't try to re-harvest excluded, depleted or malformed ore vein again until we revisit the cell
		if (objType == ObjectType::oreVein)
		{
#if _DEBUG
			_DMESSAGE("do not process oreVein more than once per cell visit: 0x%08x", m_candidate->formID);
#endif
			data->BlockReference(m_candidate);
		}

		bool isSilent = !LootingRequiresNotification(lootingType);
#if _DEBUG
		_MESSAGE("Enqueue AutoHarvest event");
#endif
		// don't let the backlog of messages get too large, it's about 1 per second
		bool ignoreBlocking(m_ini->GetSetting(INIFile::common, INIFile::config, "LootBlockedActivators") != 0);
		TriggerAutoHarvest(objType, refrEx.GetItemCount(), isSilent || PendingAutoHarvest() > AutoHarvestSpamLimit, ignoreBlocking, manualLootNotify);
	}
	else if (m_targetType == INIFile::SecondaryType::containers || m_targetType == INIFile::SecondaryType::deadbodies)
	{
#if _DEBUG
		_DMESSAGE("scanning container/body %s/0x%08x", m_candidate->GetName(), m_candidate->formID);
		DumpContainer(refrEx);
#endif
		bool requireQuestItemAsTarget = m_ini->GetSetting(INIFile::autoharvest, INIFile::config, "questObjectScope") != 0;
		bool hasQuestObject(false);
		bool hasEnchantItem(false);
		bool skipLooting(false);
		LootableItems lootableItems(
			ContainerLister(m_targetType, m_candidate, requireQuestItemAsTarget).GetOrCheckContainerForms(hasQuestObject, hasEnchantItem));
		if (lootableItems.empty())
		{
			// Nothing lootable here
#if _DEBUG
			_DMESSAGE("container %s/0x%08x is empty", m_candidate->GetName(), m_candidate->formID);
#endif
			// record looting so we don't rescan
			MarkContainerLooted(m_candidate);
			return;
		}

		// initially no glow - flag using synthetic value with highest precedence
		m_glowReason = GlowReason::None;
		if (m_targetType == INIFile::SecondaryType::containers)
		{
			if (data->IsReferenceLockedContainer(m_candidate))
			{
				SpecialObjectHandling lockedChestLoot =
					SpecialObjectHandlingFromIniSetting(m_ini->GetSetting(INIFile::autoharvest, INIFile::config, "lockedChestLoot"));
				if (lockedChestLoot == SpecialObjectHandling::GlowTarget)
				{
#if _DEBUG
					_DMESSAGE("glow locked container %s/0x%08x", m_candidate->GetName(), m_candidate->formID);
#endif
					UpdateGlowReason(GlowReason::LockedContainer);
				}

				skipLooting = skipLooting || !IsSpecialObjectLootable(lockedChestLoot);
			}

			if (IsBossContainer(m_candidate))
			{
				SpecialObjectHandling bossChestLoot = 
					SpecialObjectHandlingFromIniSetting(m_ini->GetSetting(INIFile::autoharvest, INIFile::config, "bossChestLoot"));
				if (bossChestLoot == SpecialObjectHandling::GlowTarget)
				{
#if _DEBUG
					_DMESSAGE("glow boss container %s/0x%08x", m_candidate->GetName(), m_candidate->formID);
#endif
					UpdateGlowReason(GlowReason::BossContainer);
				}

				skipLooting = skipLooting || !IsSpecialObjectLootable(bossChestLoot);
			}
		}

		if (hasQuestObject)
		{
			SpecialObjectHandling questObjectLoot =
				SpecialObjectHandlingFromIniSetting(m_ini->GetSetting(INIFile::autoharvest, INIFile::config, "questObjectLoot"));
			if (questObjectLoot == SpecialObjectHandling::GlowTarget)
			{
#if _DEBUG
				_DMESSAGE("glow container with quest object %s/0x%08x", m_candidate->GetName(), m_candidate->formID);
#endif
				UpdateGlowReason(GlowReason::QuestObject);
			}

			skipLooting = skipLooting || !IsSpecialObjectLootable(questObjectLoot);
		}

		if (hasEnchantItem)
		{
			SInt32 enchantItemGlow = static_cast<int>(m_ini->GetSetting(INIFile::autoharvest, INIFile::config, "enchantItemGlow"));
			if (enchantItemGlow == 1)
			{
#if _DEBUG
				_DMESSAGE("glow container with enchanted object %s/0x%08x", m_candidate->GetName(), m_candidate->formID);
#endif
				UpdateGlowReason(GlowReason::EnchantedItem);
			}
		}
		// order is important to ensure we glow correctly even if blocked
		skipLooting = IsLootingForbidden() || skipLooting;

		if (IsLocationExcluded())
		{
#if _DEBUG
			_DMESSAGE("Player location is excluded");
#endif
			skipLooting = true;
		}

		if (IsPopulationCenterExcluded())
		{
#if _DEBUG
			_DMESSAGE("Player location is excluded as unpermitted population center");
#endif
			skipLooting = true;
		}

		if (m_glowReason != GlowReason::None)
		{
			TriggerObjectGlow(m_candidate, ObjectGlowDurationSpecialSeconds);
		}
		if (skipLooting)
			return;

		// when we get to the point where looting is confirmed, block the reference to
		// avoid re-looting without a player cell or config change
#if _DEBUG
		_DMESSAGE("block looted container %s/0x%08x", m_candidate->GetName(), m_candidate->formID);
#endif
		data->BlockReference(m_candidate);
		// Build list of lootable targets with count and notification flag for each
		std::vector<std::pair<InventoryItem, bool>> targets;
		targets.reserve(lootableItems.size());
		for (auto& targetItemInfo : lootableItems)
		{
			RE::TESBoundObject* target(targetItemInfo.BoundObject());
			if (!target)
				continue;

			TESFormHelper itemEx(target);

			if (BasketFile::GetSingleton()->IsinList(BasketFile::EXCLUDELIST, target))
			{
#if _DEBUG
				_DMESSAGE("block due to BasketFile exclude-list for 0x%08x", target->formID);
#endif
				data->BlockForm(target);
				continue;
			}

			ObjectType objType = ClassifyType(itemEx.m_form);
			std::string typeName = GetObjectTypeName(objType);

			LootingType lootingType = LootingTypeFromIniSetting(m_ini->GetSetting(INIFile::autoharvest, INIFile::itemObjects, typeName.c_str()));
			if (lootingType == LootingType::LeaveBehind)
			{
#if _DEBUG
				_DMESSAGE("block - typename %s excluded for 0x%08x", typeName.c_str(), target->formID);
#endif
				data->BlockForm(target);
				continue;
			}
			if (LootingDependsOnValueWeight(lootingType, objType) && itemEx.ValueWeightTooLowToLoot(m_ini))
			{
#if _DEBUG
				_DMESSAGE("block - v/w excludes for 0x%08x", target->formID);
#endif
				data->BlockForm(target);
				continue;
			}

			targets.push_back({targetItemInfo, LootingRequiresNotification(lootingType)});
#if _DEBUG
			_DMESSAGE("get %s (%d) from container %s/0x%08x", itemEx.m_form->GetName(), targetItemInfo.Count(),
				m_candidate->GetName(), m_candidate->formID);
#endif
		}

		if (!targets.empty())
		{
			// check highlighting for dead NPC or container
			int playContainerAnimation(static_cast<int>(m_ini->GetSetting(INIFile::autoharvest, INIFile::config, "PlayContainerAnimation")));
			if (playContainerAnimation > 0)
			{
				if (m_targetType == INIFile::SecondaryType::containers)
				{
					if (!refrEx.GetTimeController())
					{
						// no container animation feasible, highlight it instead
						playContainerAnimation = 2;
					}
				}
				else
				{
					// Dead NPCs cannot be animated, but highlighting requested
					playContainerAnimation = 2;
				}
			}

			TriggerContainerLootMany(targets, playContainerAnimation);
		}
	}
}

RecursiveLock SearchTask::m_searchLock;
bool SearchTask::m_threadStarted = false;
bool SearchTask::m_searchAllowed = false;
bool SearchTask::m_sneaking = false;
RE::TESObjectCELL* SearchTask::m_playerCell = nullptr;
bool SearchTask::m_playerCellSelfOwned = false;
RE::BGSLocation* SearchTask::m_playerLocation = nullptr;
RE::BGSKeyword* SearchTask::m_playerHouseKeyword(nullptr);
bool SearchTask::m_carryAdjustedForCombat = false;
bool SearchTask::m_carryAdjustedForPlayerHome = false;
bool SearchTask::m_carryAdjustedForDrawnWeapon = false;
int SearchTask::m_currentCarryWeightChange = 0;
bool SearchTask::m_menuOpen = false;

int SearchTask::m_crimeCheck = 0;
SpecialObjectHandling SearchTask::m_belongingsCheck = SpecialObjectHandling::GlowTarget;

void SearchTask::SetPlayerHouseKeyword(RE::BGSKeyword* keyword)
{
	m_playerHouseKeyword = keyword;
}

double MinDelay = 0.1;

void SearchTask::ScanThread()
{
#if _DEBUG
	_DMESSAGE("starting thread");
#endif
	m_ini = INIFile::GetInstance();
	while (true)
	{
		double delay(m_ini->GetSetting(INIFile::PrimaryType::autoharvest,
			INIFile::SecondaryType::config, "IntervalSeconds"));
		delay = std::max(MinDelay, delay);
		if (!IsAllowed())
		{
#if _DEBUG
			_DMESSAGE("search disallowed, game loading or menus open");
#endif
		}
		else
		{
			DoPeriodicSearch();
		}
#if _DEBUG
		_DMESSAGE("wait for %d milliseconds", static_cast<long long>(delay * 1000.0));
#endif
		auto nextRunTime = std::chrono::high_resolution_clock::now() + std::chrono::milliseconds(static_cast<long long>(delay * 1000.0));
		std::this_thread::sleep_until(nextRunTime);
	}
}

void SearchTask::Start()
{
	std::thread([]()
	{
		// use structured exception handling to get stack walk on windows exceptions
		__try
		{
			ScanThread();
		}
		__except (LogStackWalker::LogStack(GetExceptionInformation()))
		{
		}
	}).detach();
}

int InfiniteWeight = 100000;

void SearchTask::ResetRestrictions(const bool gameReload)
{
	DataCase::GetInstance()->ListsClear(gameReload);
#if _DEBUG
	_MESSAGE("Unlock task-pending REFRs");
#endif
	RecursiveLockGuard guard(m_lock);
	// unblock all blocked auto-harvest objects
	m_autoHarvestLock.clear();
	// Dynamic containers that we looted reset on cell change
	ResetLootedDynamicContainers();
	if (gameReload)
	{
		// unblock possible player house checks after game reload
		m_playerHouses.clear();
		// clear list of dead bodies pending looting - blocked reference cleanup allows redo if still viable
		ResetLootedContainers();
	}
	// clean up the list of glowing objects, don't futz with EffectShader since cannot run scripts at this time
	m_glowExpiration.clear();
}

std::vector<RE::TESObjectREFR*> SearchTask::m_refs;

// used for PlayerCharacter
bool SearchTask::IsConcealed(RE::MagicTarget* target)
{
	if (target->HasEffectWithArchetype(RE::EffectArchetypes::ArchetypeID::kInvisibility))
	{
#if _DEBUG
		_DMESSAGE("player invisible");
#endif
		return true;
	}
	if (target->HasEffectWithArchetype(RE::EffectArchetypes::ArchetypeID::kEtherealize))
	{
#if _DEBUG
		_DMESSAGE("player ethereal");
#endif
		return true;
	}
	return false;
}

void SearchTask::DoPeriodicSearch()
{
	DataCase* data = DataCase::GetInstance();
	if (!data)
		return;
	bool playerInCombat(false);
	bool sneaking(false);
	{
		WindowsUtils::ScopedTimer elapsed("Periodic Search pre-checks");
		if (!IsAllowed())
		{
#if _DEBUG
			_DMESSAGE("search disallowed");
#endif
			return;
		}

		if (!GoodToGo())
		{
#if _DEBUG
			_DMESSAGE("Prerequisites not in place yet");
#endif
			return;
		}

		// disable auto-looting if we are inside player house - player 'current location' may be validly empty
		RE::PlayerCharacter* player(RE::PlayerCharacter::GetSingleton());
		if (!player)
		{
#if _DEBUG
			_DMESSAGE("PlayerCharacter not available");
#endif
			return;
		}

		{
			RecursiveLockGuard guard(m_lock);
			if (!m_pluginSynced)
			{
	#if _DEBUG
				_DMESSAGE("Plugin sync still pending");
	#endif
				return;
			}
		}

		// handle player death. Obviously we are not looting on their behalf until a game reload or other resurrection event.
		// Assumes player non-essential: if player is in God mode a little extra carry weight or post-death looting is not
		// breaking immersion.
		RE::BGSLocation* playerLocation(player->currentLocation);
		const bool RIPPlayer(player->IsDead(true));
		if (RIPPlayer)
		{
			// Fire location change logic
			m_playerLocation = nullptr;
			m_playerCell = nullptr;
			m_playerCellSelfOwned = false;
		}

		if (playerLocation != m_playerLocation)
		{
#if _DEBUG
			_MESSAGE("Player left old location, now at %s", playerLocation ? playerLocation->GetName() : "unnamed");
#endif
			m_playerLocation = playerLocation;
			// Player changed location
			if (m_playerLocation)
			{
				// check if it is a player house, and if so whether it is new
				if (!IsPlayerHouse(m_playerLocation))
				{
					if (m_playerLocation->HasKeyword(m_playerHouseKeyword))
					{
						// record as a player house and notify as it is a new one in this game load
#if _DEBUG
						_MESSAGE("Player House %s detected", m_playerLocation->GetName());
#endif
						AddPlayerHouse(m_playerLocation);
					}
				}
				if (IsPlayerHouse(m_playerLocation))
				{
					static RE::BSFixedString playerHouseMsg(papyrus::GetTranslation(nullptr, RE::BSFixedString("$AHSE_HOUSE_CHECK")));
					if (!playerHouseMsg.empty())
					{
						std::string notificationText(playerHouseMsg);
						Replace(notificationText, "{HOUSENAME}", m_playerLocation->GetName());
						RE::DebugNotification(notificationText.c_str());
					}
				}
				// check if this is a population center excluded from looting and if so, notify we entered it
				if (IsPopulationCenterExcluded())
				{
					static RE::BSFixedString populationCenterMsg(papyrus::GetTranslation(nullptr, RE::BSFixedString("$AHSE_POPULATED_CHECK")));
					if (!populationCenterMsg.empty())
					{
						std::string notificationText(populationCenterMsg);
						Replace(notificationText, "{LOCATIONNAME}", m_playerLocation->GetName());
						RE::DebugNotification(notificationText.c_str());
					}
				}
			}
		}

		if (RIPPlayer)
		{
#if _DEBUG
			_DMESSAGE("Player is dead");
#endif
			return;
		}

		if (!RE::PlayerControls::GetSingleton() || !RE::PlayerControls::GetSingleton()->IsActivateControlsEnabled())
		{
#if _DEBUG
			_DMESSAGE("player controls disabled");
#endif
			return;
		}

		// By inspection, UI menu stack has steady state size of 1. Opening application and/or inventory adds 1 each,
		// opening console adds 2. So this appears to be a catch-all for those conditions.
		if (!RE::UI::GetSingleton())
		{
#if _DEBUG
			_DMESSAGE("UI inaccessible");
			return;
#endif
		}
		size_t count(RE::UI::GetSingleton()->menuStack.size());
		bool menuOpen(count > 1);
		if (menuOpen != m_menuOpen)
		{
			// record state change
			m_menuOpen = menuOpen;
			if (menuOpen)
			{
				// Menu just opened
#if _DEBUG
				_DMESSAGE("console and/or menu(s) opened, delta to menu-stack size = %d", count);
#endif
			}
			else
			{
#if _DEBUG
				_DMESSAGE("console and/or menu(s) closed");
#endif
				// Menu just closed
				// reset carry weight - will reinstate correct value if/when scan resumes
				// update Locked Container last-accessed time
				ResetCarryWeight();
				data->UpdateLockedContainers();
			}
			return;
		}
		if (m_menuOpen)
			return;

		// Respect encumbrance quality of life settings
		bool playerInOwnHouse(IsPlayerHouse(m_playerLocation));
		int carryWeightChange(m_currentCarryWeightChange);
		if (m_ini->GetSetting(INIFile::PrimaryType::common, INIFile::config, "UnencumberedInPlayerHome") != 0.0)
		{
			// when location changes to/from player house, adjust carry weight accordingly
			if (playerInOwnHouse != m_carryAdjustedForPlayerHome)
			{
				carryWeightChange += playerInOwnHouse ? InfiniteWeight : -InfiniteWeight;
				m_carryAdjustedForPlayerHome = playerInOwnHouse;
#if _DEBUG
				_MESSAGE("Carry weight delta after in-player-home adjustment %d", carryWeightChange);
#endif
			}
		}
		playerInCombat = player->IsInCombat() && !player->IsDead(true);
		if (m_ini->GetSetting(INIFile::PrimaryType::common, INIFile::config, "UnencumberedInCombat") != 0.0)
		{
			// when state changes in/out of combat, adjust carry weight accordingly
			if (playerInCombat != m_carryAdjustedForCombat)
			{
				carryWeightChange += playerInCombat ? InfiniteWeight : -InfiniteWeight;
				m_carryAdjustedForCombat = playerInCombat;
#if _DEBUG
				_MESSAGE("Carry weight delta after in-combat adjustment %d", carryWeightChange);
#endif
			}
		}
		bool isWeaponDrawn(player->IsWeaponDrawn());
		if (m_ini->GetSetting(INIFile::PrimaryType::common, INIFile::config, "UnencumberedIfWeaponDrawn") != 0.0)
		{
			// when state changes between drawn/sheathed, adjust carry weight accordingly
			if (isWeaponDrawn != m_carryAdjustedForDrawnWeapon)
			{
				carryWeightChange += isWeaponDrawn ? InfiniteWeight : -InfiniteWeight;
				m_carryAdjustedForDrawnWeapon = isWeaponDrawn;
#if _DEBUG
				_MESSAGE("Carry weight delta after drawn weapon adjustment %d", carryWeightChange);
#endif
			}
		}
		if (carryWeightChange != m_currentCarryWeightChange)
		{
			int requiredWeightDelta(carryWeightChange - m_currentCarryWeightChange);
			m_currentCarryWeightChange = carryWeightChange;
			// handle carry weight update via a script event
#if _DEBUG
			_MESSAGE("Adjust carry weight by delta %d", requiredWeightDelta);
#endif
			TriggerCarryWeightDelta(requiredWeightDelta);
		}

		if (playerInOwnHouse)
		{
#if _DEBUG
			_DMESSAGE("Player House, skip");
#endif
			return;
		}

		const int disableDuringCombat = static_cast<int>(m_ini->GetSetting(INIFile::autoharvest, INIFile::config, "disableDuringCombat"));
		if (disableDuringCombat != 0 && playerInCombat)
		{
#if _DEBUG
			_MESSAGE("disableDuringCombat %d", disableDuringCombat);
#endif
			return;
		}

		const int disableWhileWeaponIsDrawn = static_cast<int>(m_ini->GetSetting(INIFile::autoharvest, INIFile::config, "disableWhileWeaponIsDrawn"));
		if (disableWhileWeaponIsDrawn != 0 && player->IsWeaponDrawn())
		{
#if _DEBUG
			_MESSAGE("disableWhileWeaponIsDrawn %d", disableWhileWeaponIsDrawn);
#endif
			return;
		}

		const int disableWhileConcealed = static_cast<int>(m_ini->GetSetting(INIFile::autoharvest, INIFile::config, "DisableWhileConcealed"));
		if (disableWhileConcealed != 0 && IsConcealed(player))
		{
#if _DEBUG
			_MESSAGE("disableWhileConcealed %d", disableWhileConcealed);
#endif
			return;
		}

		sneaking = player->IsSneaking();
		bool unblockAll(false);
		// Reset blocked lists if sneak state or player cell has changed
		if (m_sneaking != sneaking)
		{
			m_sneaking = sneaking;
			unblockAll = true;
		}
		// Player cell should never be empty
		RE::TESObjectCELL* playerCell(player->parentCell);
		if (playerCell != m_playerCell)
		{
			unblockAll = true;
			m_playerCell = playerCell;
			m_playerCellSelfOwned = IsCellPlayerOwned(m_playerCell);
#if _DEBUG
			if (m_playerCell)
			{
				_MESSAGE("Player cell updated to 0x%08x", m_playerCell->GetFormID());
			}
			else
			{
				_MESSAGE("Player cell cleared");
			}
#endif
		}
		if (unblockAll)
		{
			static const bool gameReload(false);
			ResetRestrictions(gameReload);
		}
		if (!m_playerCell)
		{
#if _DEBUG
			_MESSAGE("Player cell not yet set up");
#endif
			return;
		}

	}

	// Retrieve these settings only once
	m_crimeCheck = static_cast<int>(m_ini->GetSetting(INIFile::autoharvest, INIFile::config, (sneaking) ? "crimeCheckSneaking" : "crimeCheckNotSneaking"));
	m_belongingsCheck = SpecialObjectHandlingFromIniSetting(m_ini->GetSetting(INIFile::autoharvest, INIFile::config, "playerBelongingsLoot"));
	m_refs = PlayerCellHelper::GetInstance().GetReferences(m_playerCell, m_ini->GetRadius(INIFile::autoharvest));
	// Process any queued dead body that is dead long enough to have played kill animation
	ReleaseReliablyDeadActors();

	for (RE::TESObjectREFR* refr : m_refs)
	{
		// Filter out borked REFRs. PROJ repro observed in logs as below:
		/*
			0x15f0 (2020-05-17 14:05:27.290) J:\GitHub\AutoHarvestSE.Port\utils.cpp(211): [MESSAGE] TIME(Filter loot candidates in/near cell)=54419 micros
			0x15f0 (2020-05-17 14:05:27.290) J:\GitHub\AutoHarvestSE.Port\tasks.cpp(1037): [MESSAGE] Process REFR 0x00000000 with base object Iron Arrow/0x0003be11
			0x15f0 (2020-05-17 14:05:27.290) J:\GitHub\AutoHarvestSE.Port\utils.cpp(211): [MESSAGE] TIME(Process Auto-loot Candidate Iron Arrow/0x0003be11)=35 micros

			0x15f0 (2020-05-17 14:05:31.950) J:\GitHub\AutoHarvestSE.Port\utils.cpp(211): [MESSAGE] TIME(Filter loot candidates in/near cell)=54195 micros
			0x15f0 (2020-05-17 14:05:31.950) J:\GitHub\AutoHarvestSE.Port\tasks.cpp(1029): [MESSAGE] REFR 0x00000000 has no Base Object
		*/

		if (!refr->data.objectReference)
		{
#if _DEBUG
			_MESSAGE("REFR 0x%08x has no Base Object", refr->GetFormID());
#endif
			data->BlacklistReference(refr);
			continue;
		}
		else
		{
#if _DEBUG
			_MESSAGE("Process REFR 0x%08x with base object %s/0x%08x", refr->GetFormID(),
				refr->data.objectReference->GetName(), refr->data.objectReference->GetFormID());
#endif
		}

		INIFile::SecondaryType lootTargetType = INIFile::SecondaryType::itemObjects;
		{
			WindowsUtils::ScopedTimer elapsed("Process Auto-loot Candidate", refr);
			if (!refr)
				continue;
			RE::Actor* actor(nullptr);
			if ((actor = refr->data.objectReference->As<RE::Actor>()) || refr->data.objectReference->As<RE::TESNPC>())
			{
				if (m_ini->GetSetting(INIFile::PrimaryType::common, INIFile::config, "enableLootDeadbody") == 0.0 || !refr->IsDead(true))
					continue;

				if (actor)
				{
					ActorHelper actorEx(actor);
					if (actorEx.IsPlayerAlly() || actorEx.IsEssential() || actorEx.IsSummoned())
					{
						data->BlockReference(refr);
						continue;
					}
				}

				lootTargetType = INIFile::SecondaryType::deadbodies;
				// Delay looting exactly once. We only return here after required time since death has expired.
				if (!HasDynamicData(refr) && !IsLootedContainer(refr))
				{
					// Use async looting to allow game to settle actor state and animate their untimely demise
					RegisterActorTimeOfDeath(refr);
					continue;
				}
			}
			else if (refr->data.objectReference->As<RE::TESContainer>())
			{
				if (m_ini->GetSetting(INIFile::PrimaryType::common, INIFile::config, "EnableLootContainer") == 0.0)
					continue;
				lootTargetType = INIFile::SecondaryType::containers;
			}
			else if (refr->data.objectReference->As<RE::TESObjectACTI>() && HasAshPile(refr))
			{
				if (m_ini->GetSetting(INIFile::PrimaryType::common, INIFile::config, "enableLootDeadbody") == 0.0)
					continue;
				lootTargetType = INIFile::SecondaryType::deadbodies;
				// Delay looting exactly once. We only return here after required time since death has expired.
				if (!HasDynamicData(refr) && !IsLootedContainer(refr))
				{
					// Use async looting to allow game to settle actor state and animate their untimely demise
					RegisterActorTimeOfDeath(refr);
					continue;
				}
				// deferred looting of dead bodies - introspect ExtraDataList to get the REFR
#if _DEBUG
				RE::TESObjectREFR* originalRefr(refr);
#endif
				refr = GetAshPile(refr);
#if _DEBUG
				_MESSAGE("Got ash-pile REFR 0x%08x from REFR 0x%08x", refr->GetFormID(), originalRefr->GetFormID());
#endif
			}
			else if (m_ini->GetSetting(INIFile::PrimaryType::common, INIFile::config, "enableAutoHarvest") == 0.0)
			{
				continue;
			}
		}
		SearchTask(refr, lootTargetType).Run();
	}
	m_refs.clear();
}

// reset carry weight adjustments - scripts will handle the Player Actor Value, scan will reinstate as needed when we resume
void SearchTask::ResetCarryWeight()
{
	if (m_currentCarryWeightChange != 0)
	{
#if _DEBUG
		_MESSAGE("Reset carry weight delta %d, in-player-home=%s, in-combat=%s, weapon-drawn=%s", m_currentCarryWeightChange,
			m_carryAdjustedForPlayerHome ? "true" : "false", m_carryAdjustedForCombat ? "true" : "false", m_carryAdjustedForDrawnWeapon ? "true" : "false");
#endif
		m_currentCarryWeightChange = 0;
		m_carryAdjustedForCombat = false;
		m_carryAdjustedForPlayerHome = false;
		m_carryAdjustedForDrawnWeapon = false;
		TriggerResetCarryWeight();
	}
}

void SearchTask::PrepareForReload()
{
	// stop scanning
	Disallow();

	// reset carry weight and menu-active state
	ResetCarryWeight();
	m_menuOpen = false;

	// reset player location - reload may bring us back in a different place and even if not, we should start from scratch
	m_playerCell = nullptr;
	m_playerCellSelfOwned = false;
	m_playerLocation = nullptr;

	// Do not scan again until we are in sync with the scripts
	m_pluginSynced = false;
}

void SearchTask::Allow()
{
	RecursiveLockGuard guard(m_searchLock);
	m_searchAllowed = true;
	if (!m_threadStarted)
	{
		// Start the thread when we are first allowed to search
		m_threadStarted = true;
		SearchTask::Start();
	}
}

void SearchTask::Disallow()
{
	RecursiveLockGuard guard(m_searchLock);
	m_searchAllowed = false;
}
bool SearchTask::IsAllowed()
{
	RecursiveLockGuard guard(m_searchLock);
	return m_searchAllowed;
}

void SearchTask::TriggerGetCritterIngredient()
{
	onGetCritterIngredient.SendEvent(m_candidate);
}

void SearchTask::TriggerCarryWeightDelta(const int delta)
{
	onCarryWeightDelta.SendEvent(delta);
}

void SearchTask::TriggerResetCarryWeight()
{
	onResetCarryWeight.SendEvent();
}

std::unordered_set<const RE::TESObjectREFR*> SearchTask::m_autoHarvestLock;

void SearchTask::TriggerAutoHarvest(const ObjectType objType, int itemCount, const bool isSilent, const bool ignoreBlocking, const bool manualLootNotify)
{
	// Event handler in Papyrus script unlocks the task - do not issue multiple concurrent events on the same REFR
	if (!LockAutoHarvest(m_candidate))
		return;
	onAutoHarvest.SendEvent(m_candidate, static_cast<int>(objType), itemCount, isSilent, ignoreBlocking, manualLootNotify);
}

bool SearchTask::LockAutoHarvest(const RE::TESObjectREFR* refr)
{
	RecursiveLockGuard guard(m_lock);
	if (!refr)
		return false;
	return (m_autoHarvestLock.insert(refr)).second;
}

bool SearchTask::UnlockAutoHarvest(const RE::TESObjectREFR* refr)
{
	RecursiveLockGuard guard(m_lock);
	if (!refr)
		return false;
	return m_autoHarvestLock.erase(refr) > 0;
}

bool SearchTask::IsLockedForAutoHarvest(const RE::TESObjectREFR* refr)
{
	RecursiveLockGuard guard(m_lock);
	return (refr && m_autoHarvestLock.count(refr));
}

size_t SearchTask::PendingAutoHarvest()
{
	RecursiveLockGuard guard(m_lock);
	return m_autoHarvestLock.size();
}

void SearchTask::TriggerLootFromNPC(RE::TESObjectREFR* npc, RE::TESForm* item, int itemCount)
{
	onLootFromNPC.SendEvent(npc, item, itemCount);
}

std::unordered_set<const RE::BGSLocation*> SearchTask::m_playerHouses;
bool SearchTask::AddPlayerHouse(const RE::BGSLocation* location)
{
	if (!location)
		return false;
	RecursiveLockGuard guard(m_lock);
	return (m_playerHouses.insert(location)).second;
}

bool SearchTask::RemovePlayerHouse(const RE::BGSLocation* location)
{
	if (!location)
		return false;
	RecursiveLockGuard guard(m_lock);
	return m_playerHouses.erase(location) > 0;
}

// Check indeterminate status of the location, because a requested UI check is pending
bool SearchTask::IsPlayerHouse(const RE::BGSLocation* location)
{
	RecursiveLockGuard guard(m_lock);
	return location && m_playerHouses.count(location);
}

std::unordered_map<const RE::BGSLocation*, PopulationCenterSize> SearchTask::m_populationCenters;
bool SearchTask::IsPopulationCenterExcluded()
{
	if (!m_playerLocation)
		return false;
	PopulationCenterSize excludedCenterSize(PopulationCenterSizeFromIniSetting(
		m_ini->GetSetting(INIFile::common, INIFile::config, "PreventPopulationCenterLooting")));
	if (excludedCenterSize == PopulationCenterSize::None)
		return false;

	RecursiveLockGuard guard(m_lock);
	const auto locationRecord(m_populationCenters.find(m_playerLocation));
	// if small locations are excluded we automatically exclude any larger, so use >= here, assuming this is
	// a population center
	return locationRecord != m_populationCenters.cend() && locationRecord->second >= excludedCenterSize;
}

std::unordered_set<const RE::TESForm*> SearchTask::m_excludeLocations;
bool SearchTask::m_pluginSynced(false);

// this is the last function called by the scripts when re-syncing state
void SearchTask::MergeExcludeList()
{
	RecursiveLockGuard guard(m_lock);
	// Add loaded locations to the list of exclusions
	BasketFile::GetSingleton()->SyncList(BasketFile::EXCLUDELIST);
	for (const auto exclusion : BasketFile::GetSingleton()->GetList(BasketFile::EXCLUDELIST))
	{
		SearchTask::AddLocationToExcludeList(exclusion);
	}
	// reset blocked lists to allow recheck vs current state
	static const bool gameReload(true);
	ResetRestrictions(gameReload);

	// need to wait for the scripts to sync up before performing player house checks
	m_pluginSynced = true;
}

void SearchTask::ResetExcludedLocations()
{
#if _DEBUG
	_DMESSAGE("Reset list of locations excluded from looting");
#endif
	RecursiveLockGuard guard(m_lock);
	m_excludeLocations.clear();
}

void SearchTask::AddLocationToExcludeList(const RE::TESForm* location)
{
#if _DEBUG
	_DMESSAGE("Location %s excluded from looting", location->GetName());
#endif
	RecursiveLockGuard guard(m_lock);
	m_excludeLocations.insert(location);
}

void SearchTask::DropLocationFromExcludeList(const RE::TESForm* location)
{
#if _DEBUG
	_DMESSAGE("Location %s no longer excluded from looting", location->GetName());
#endif
	RecursiveLockGuard guard(m_lock);
	m_excludeLocations.erase(location);
}

bool SearchTask::IsLocationExcluded()
{
	if (!m_playerLocation)
		return false;
	RecursiveLockGuard guard(m_lock);
	return m_excludeLocations.count(m_playerLocation) > 0;
}

void SearchTask::TriggerContainerLootMany(std::vector<std::pair<InventoryItem, bool>>& targets, const int animationType)
{
	if (!m_candidate)
		return;

	// visual notification, if requested
	if (animationType == 1)
	{
		m_candidate->PlayAnimation("Close", "Open");
	}
	else if (animationType == 2)
	{
		// glow looted object briefly after looting
		TriggerObjectGlow(m_candidate, ObjectGlowDurationLootedSeconds, GlowReason::SimpleTarget);
	}

	for (auto& target : targets)
	{
		// Play sound first as this uses InventoryItemData on the source container
		InventoryItem& itemInfo(target.first);
		bool notify(target.second);
		RE::PlayerCharacter::GetSingleton()->PlayPickUpSound(itemInfo.BoundObject(), true, false);
		std::string name(itemInfo.BoundObject()->GetName());
		int count(itemInfo.TakeAll(m_candidate, RE::PlayerCharacter::GetSingleton()));
		if (notify)
		{
			std::string notificationText;
			if (count > 1)
			{
				static RE::BSFixedString multiActivate(papyrus::GetTranslation(nullptr, RE::BSFixedString("$AHSE_ACTIVATE(COUNT)_MSG")));
				if (!multiActivate.empty())
				{
					notificationText = multiActivate;
					Replace(notificationText, "{ITEMNAME}", name.c_str());
					std::ostringstream intStr;
					intStr << count;
					Replace(notificationText, "{COUNT}", intStr.str());
				}
			}
			else
			{
				static RE::BSFixedString singleActivate(papyrus::GetTranslation(nullptr, RE::BSFixedString("$AHSE_ACTIVATE_MSG")));
				if (!singleActivate.empty())
				{
					notificationText = singleActivate;
					Replace(notificationText, "{ITEMNAME}", name.c_str());
				}
			}
			if (!notificationText.empty())
			{
				RE::DebugNotification(notificationText.c_str());
			}
		}
	}
}

void SearchTask::TriggerObjectGlow(RE::TESObjectREFR* refr, const int duration)
{
	TriggerObjectGlow(refr, duration, m_glowReason);
}

void SearchTask::TriggerObjectGlow(RE::TESObjectREFR* refr, const int duration, const GlowReason glowReason)
{

	// only send the glow event once per N seconds. This will retrigger on later passes, but once we are out of
	// range no more glowing will be triggered. The item remains in the list until we change cell but there should
	// never be so many in a cell that this is a problem.
	RecursiveLockGuard guard(m_lock);
	const auto existingGlow(m_glowExpiration.find(refr));
	auto currentTime(std::chrono::high_resolution_clock::now());
	if (existingGlow != m_glowExpiration.cend() && existingGlow->second > currentTime)
		return;
	auto expiry = currentTime + std::chrono::milliseconds(static_cast<long long>(duration * 1000.0));
	m_glowExpiration[refr] = expiry;
#if _DEBUG
	_DMESSAGE("Trigger glow for %s/0x%08x", refr->GetName(), refr->formID);
#endif
	onObjectGlow.SendEvent(refr, duration, static_cast<int>(glowReason));
}

bool SearchTask::firstTime = true;

void SearchTask::Init()
{
    if (firstTime)
	{
		WindowsUtils::ScopedTimer elapsed("Categorize Lootables");
		DataCase::GetInstance()->CategorizeLootables();
		CategorizePopulationCenters();
		firstTime = false;
	}
	static const bool gameReload(true);
	ResetRestrictions(gameReload);
}


// Classify items by their keywords
void SearchTask::CategorizePopulationCenters()
{
	RE::TESDataHandler* dhnd = RE::TESDataHandler::GetSingleton();
	if (!dhnd)
		return;

	std::unordered_map<std::string, PopulationCenterSize> sizeByKeyword =
	{
		// Skyrim core
		{"LocTypeSettlement", PopulationCenterSize::Settlements},
		{"LocTypeTown", PopulationCenterSize::Towns},
		{"LocTypeCity", PopulationCenterSize::Cities}
	};

	for (RE::TESForm* form : dhnd->GetFormArray(RE::BGSLocation::FORMTYPE))
	{
		RE::BGSLocation* location(form->As<RE::BGSLocation>());
		if (!location)
		{
#if _DEBUG
			_MESSAGE("Skipping non-location form 0x%08x", form->formID);
#endif
			continue;
		}
		// Scan location keywords to check if it's a settlement
		UInt32 numKeywords(location->GetNumKeywords());
		PopulationCenterSize size(PopulationCenterSize::None);
		std::string largestMatch;
		for (UInt32 next = 0; next < numKeywords; ++next)
		{
			std::optional<RE::BGSKeyword*> keyword(location->GetKeywordAt(next));
			if (!keyword.has_value() || !keyword.value())
				continue;

			std::string keywordName(keyword.value()->GetFormEditorID());
			const auto matched(sizeByKeyword.find(keywordName));
			if (matched == sizeByKeyword.cend())
				continue;
			if (matched->second > size)
			{
				size = matched->second;
				largestMatch = keywordName;
			}
		}
		// record population center size in case looting is selectively prevented
		if (size != PopulationCenterSize::None)
		{
#if _DEBUG
			_MESSAGE("%s/0x%08x is population center of type %s", location->GetName(), location->GetFormID(), largestMatch.c_str());
#endif
			m_populationCenters.insert(std::make_pair(location, size));
		}
		else
		{
#if _DEBUG
			_MESSAGE("%s/0x%08x is not a population center", location->GetName(), location->GetFormID());
#endif
		}
	}

	// We also categorize descendants of population centers. Not all will follow the same rule as the parent. For example,
	// preventing looting in Whiterun should also prevent looting in the Bannered Mare, but not in Whiterun Sewers. Use
	// child location keywords to control this.
	std::unordered_set<std::string> lootableChildLocations =
	{
		// not all Skyrim core, necessarily
		"LocTypeClearable",
		"LocTypeDungeon",
		"LocTypeDraugrCrypt",
		"LocTypeNordicRuin",
		"zzzBMLocVampireDungeon"
	};
#if _DEBUG
	std::unordered_set<std::string> childKeywords;
#endif
	for (RE::TESForm* form : dhnd->GetFormArray(RE::BGSLocation::FORMTYPE))
	{
		RE::BGSLocation* location(form->As<RE::BGSLocation>());
		if (!location)
		{
			continue;
		}
		// check if this is a descendant of a population center
		RE::BGSLocation* antecedent(location->parentLoc);
		PopulationCenterSize parentSize(PopulationCenterSize::None);
		while (antecedent != nullptr)
		{
			const auto matched(m_populationCenters.find(antecedent));
			if (matched != m_populationCenters.cend())
			{
				parentSize = matched->second;
#if _DEBUG
				_MESSAGE("%s/0x%08x is a descendant of population center %s/0x%08x with size %d", location->GetName(), location->GetFormID(),
					antecedent->GetName(), antecedent->GetFormID(), parentSize);
#endif
				break;
			}
			antecedent = antecedent->parentLoc;
		}

		if (!antecedent)
			continue;

		// Scan location keywords to determine if lootable, or bucketed with its population center antecedent
		UInt32 numKeywords(location->GetNumKeywords());
		bool allowLooting(false);
		for (UInt32 next = 0; !allowLooting && next < numKeywords; ++next)
		{
			std::optional<RE::BGSKeyword*> keyword(location->GetKeywordAt(next));
			if (!keyword.has_value() || !keyword.value())
				continue;

			std::string keywordName(keyword.value()->GetFormEditorID());
#if _DEBUG
			childKeywords.insert(keywordName);
#endif
			if (lootableChildLocations.find(keywordName) != lootableChildLocations.cend())
			{
				allowLooting = true;
#if _DEBUG
				_MESSAGE("%s/0x%08x is lootable child location due to keyword %s", location->GetName(), location->GetFormID(), keywordName.c_str());
#endif
				break;
			}
		}
		if (allowLooting)
			continue;

		// Store the child location with the same criterion as parent, unless it's inherently lootable
		// e.g. dungeon within the city limits like Whiterun Sewers, parts of the Ratway
#if _DEBUG
		_MESSAGE("%s/0x%08x stored with same rule as its parent population center", location->GetName(), location->GetFormID());
#endif
		m_populationCenters.insert(std::make_pair(location, parentSize));
	}
#if _DEBUG
	// this debug output from a given load order drives the list of 'really-lootable' child location types above
	for (const std::string& keyword : childKeywords)
	{
		_MESSAGE("Population center child keyword: %s", keyword.c_str());
	}
#endif
}
