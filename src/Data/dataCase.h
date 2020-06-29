#pragma once

#include <mutex>
#include <chrono>

#include "ProducerLootables.h"

class DataCase
{
public:
	static DataCase* GetInstance(void)
	{
		if (s_pInstance == nullptr)
			s_pInstance = new DataCase();
		return s_pInstance;
	}

	typedef std::unordered_set<const RE::TESForm*> FormCategory;

	void BlockFirehoseSource(const RE::TESObjectREFR* refr);
	void ForgetFirehoseSources();

	bool BlockReference(const RE::TESObjectREFR* refr);
	bool IsReferenceBlocked(const RE::TESObjectREFR* refr);
	void ClearBlockedReferences(const bool gameReload);

	// permanent REFR blacklist, reset on game reload
	bool BlacklistReference(const RE::TESObjectREFR* refr);
	bool IsReferenceOnBlacklist(const RE::TESObjectREFR* refr);
	void ClearReferenceBlacklist();

	bool IsReferenceLockedContainer(const RE::TESObjectREFR* refr);
	void ForgetLockedContainers();
	void UpdateLockedContainers();

	bool BlockForm(const RE::TESForm* form);
	bool UnblockForm(const RE::TESForm* form);
	bool IsFormBlocked(const RE::TESForm* form);
	void ResetBlockedForms();

	ObjectType GetFormObjectType(RE::FormID formID) const;
	bool SetObjectTypeForForm(RE::FormID formID, ObjectType objectType);
	ObjectType GetObjectTypeForFormType(RE::FormType formType) const;

	template <typename T>
	ObjectType GetObjectTypeForForm(T* form) const
	{
		ObjectType objectType(GetObjectTypeForFormType(form->formType));
		if (objectType == ObjectType::unknown)
		{
			objectType = GetFormObjectType(form->formID);
		}
		return objectType;
	}

	ResourceType DataCase::OreVeinResourceType(const RE::TESObjectACTI* mineable) const;

	const char* GetTranslation(const char* key) const;

	const RE::TESAmmo* ProjToAmmo(const RE::BGSProjectile* proj);
	const RE::TESForm* ConvertIfLeveledItem(const RE::TESForm* form) const;

	void CategorizeLootables(void);
	void ListsClear(const bool gameReload);
	bool SkipAmmoLooting(RE::TESObjectREFR* refr);

	inline bool IsBookGlowableKeyword(RE::BGSKeyword* keyword) const
	{
		return keyword && m_glowableBookKeywords.find(keyword->GetFormID()) != m_glowableBookKeywords.cend();
	}

	bool PerksAddLeveledItemsOnDeath(const RE::Actor* actor) const;
	inline const std::unordered_set<const RE::TESForm*>& OffLimitsLocations()
	{
		return m_offLimitsLocations;
	}

private:
	std::unordered_map<std::string, std::string> m_translations;

	std::unordered_map<const RE::TESObjectREFR*, RE::NiPoint3> m_arrowCheck;
	std::unordered_map<const RE::BGSProjectile*, RE::TESAmmo*> m_ammoList;

	std::unordered_set<const RE::TESForm*> m_offLimitsLocations;
	std::unordered_set<RE::TESObjectREFR*> m_offLimitsContainers;
	std::unordered_set<RE::TESForm*> m_offLimitsForms;
	std::unordered_set<RE::FormID> m_userBlockedForm;
	std::unordered_set<const RE::TESForm*> m_blockForm;
	std::unordered_set<RE::FormID> m_firehoseSources;
	std::unordered_set<RE::FormID> m_blockRefr;
	std::unordered_set<RE::FormID> m_blacklistRefr;
	std::unordered_map<RE::FormID, std::chrono::time_point<std::chrono::high_resolution_clock>> m_lockedContainers;

	std::unordered_map<RE::FormType, ObjectType> m_objectTypeByFormType;
	std::unordered_map<RE::FormID, ObjectType> m_objectTypeByForm;
	std::unordered_map<const RE::TESProduceForm*, const RE::TESForm*> m_produceFormContents;
	std::unordered_set<RE::FormID> m_glowableBookKeywords;
	std::unordered_set<const RE::BGSPerk*> m_leveledItemOnDeathPerks;

	mutable RecursiveLock m_blockListLock;

	bool GetTSV(std::unordered_set<RE::FormID> *tsv, const char* fileName);

	void RecordOffLimitsLocations(void);
	void BlockOffLimitsContainers(void);
	void GetAmmoData(void);

	template <typename T>
	ObjectType DefaultIngredientObjectType(const T* form)
	{
		return ObjectType::unknown;
	}

	template <>	ObjectType DefaultIngredientObjectType(const RE::TESFlora* form);
	template <>	ObjectType DefaultIngredientObjectType(const RE::TESObjectTREE* form);

	class LeveledItemCategorizer
	{
	public:
		LeveledItemCategorizer(const RE::TESLevItem* rootItem, const std::string& targetName);
		void CategorizeContents();

	private:
		void ProcessContentsAtLevel(const RE::TESLevItem* leveledItem);

	protected:
		virtual void ProcessContentLeaf(RE::TESForm* itemForm, ObjectType itemType) = 0;

		const RE::TESLevItem* m_rootItem;
		const std::string m_targetName;
	};

	class ProduceFormCategorizer : public LeveledItemCategorizer
	{
	public:
		ProduceFormCategorizer(RE::TESProduceForm* produceForm, const RE::TESLevItem* rootItem, const std::string& targetName);

	protected:
		virtual void ProcessContentLeaf(RE::TESForm* itemForm, ObjectType itemType) override;

	private:
		RE::TESProduceForm* m_produceForm;
		RE::TESForm* m_contents;
	};

	template <typename T>
	void CategorizeByIngredient()
	{
		RE::TESDataHandler* dhnd = RE::TESDataHandler::GetSingleton();
		if (!dhnd)
			return;

		for (T* target : dhnd->GetFormArray<T>())
		{
			if (!target->GetFullNameLength())
				continue;
			const char * targetName(target->GetFullName());
			DBG_VMESSAGE("Checking target %s/0x%08x", targetName, target->GetFormID());

			const RE::TESBoundObject* ingredient(target->produceItem);
			if (!ingredient)
			{
				REL_WARNING("No ingredient for %s/0x%08x", targetName, target->GetFormID());
				continue;
			}

			// Categorize ingredient
			ObjectType storedType(ObjectType::unknown);
			const RE::TESLevItem* leveledItem(ingredient->As<RE::TESLevItem>());
			if (leveledItem)
			{
				DBG_VMESSAGE("%s/0x%08x ingredient is Leveled Item", targetName, target->GetFormID());
				ProduceFormCategorizer(target, leveledItem, targetName).CategorizeContents();
			}
			else
			{
				// Try the ingredient form on this Produce holder
				storedType = GetObjectTypeForForm(ingredient);
				if (storedType != ObjectType::unknown)
				{
					DBG_VMESSAGE("Target %s/0x%08x has ingredient %s/0x%08x stored as type %s", targetName, target->GetFormID(),
						ingredient->GetName(), ingredient->GetFormID(), GetObjectTypeName(storedType).c_str());
					ProducerLootables::Instance().SetLootableForProducer(target, const_cast<RE::TESBoundObject*>(ingredient));
				}
				else
				{
					storedType = DefaultIngredientObjectType(target);
				}
				if (storedType != ObjectType::unknown)
				{ 
					// Store mapping of Produce holder to ingredient - this is the most correct type for this item producer
					if (SetObjectTypeForForm(target->GetFormID(), storedType))
					{
						DBG_VMESSAGE("Target %s/0x%08x stored as type %s", targetName, target->GetFormID(), GetObjectTypeName(storedType).c_str());
					}
					else
					{
						REL_WARNING("Target %s/0x%08x (%s) already stored, check data", targetName, target->GetFormID(), GetObjectTypeName(storedType).c_str());
					}
				}
				else
				{
					DBG_VMESSAGE("Target %s/0x%08x not stored", targetName, target->GetFormID());
				}
			}
		}
	}

	void HandleExceptions(void);
	ObjectType DecorateIfEnchanted(const RE::TESForm* form, const ObjectType rawType);
	void SetObjectTypeByKeywords();

	template <typename T>
	ObjectType ConsumableObjectType(T* consumable)
	{
		return ObjectType::unknown;
	}

	template <> ObjectType ConsumableObjectType<RE::AlchemyItem>(RE::AlchemyItem* consumable);
	template <> ObjectType ConsumableObjectType<RE::IngredientItem>(RE::IngredientItem* consumable);

	template <typename T>
	void CategorizeConsumables()
	{
		RE::TESDataHandler* dhnd = RE::TESDataHandler::GetSingleton();
		if (!dhnd)
			return;
		for (T* consumable : dhnd->GetFormArray<T>())
		{
			RE::TESFullName* pFullName = consumable->As<RE::TESFullName>();
			if (!pFullName || pFullName->GetFullNameLength() == 0)
			{
				DBG_VMESSAGE("Skipping unnamed form 0x%08x", consumable->GetFormID());
				continue;
			}

			std::string formName(pFullName->GetFullName());
			if (GetFormObjectType(consumable->GetFormID()) != ObjectType::unknown)
			{
				DBG_VMESSAGE("Skipping previously categorized form %s/0x%08x", formName.c_str(), consumable->GetFormID());
				continue;
			}

			ObjectType objectType(ConsumableObjectType<T>(consumable));
			DBG_MESSAGE("Consumable %s/0x%08x has type %s", formName.c_str(), consumable->GetFormID(), GetObjectTypeName(objectType).c_str());
			m_objectTypeByForm[consumable->GetFormID()] = objectType;
		}
	}

	template <typename T>
	ObjectType DefaultObjectType()
	{
		return ObjectType::clutter;
	}
	template <> ObjectType  DefaultObjectType<RE::TESObjectARMO>();

	template <typename T>
	ObjectType OverrideIfBadChoice(const RE::TESForm* form, const ObjectType objectType)
	{
		return objectType;
	}
	template <> ObjectType OverrideIfBadChoice<RE::TESObjectARMO>(const RE::TESForm* form, const ObjectType objectType);
	template <> ObjectType OverrideIfBadChoice<RE::TESObjectWEAP>(const RE::TESForm* form, const ObjectType objectType);

	std::unordered_map<std::string, ObjectType> m_objectTypeByActivationVerb;
	mutable std::unordered_set<std::string> m_unhandledActivationVerbs;
	std::unordered_map<const RE::TESObjectACTI*, ResourceType> m_resourceTypeByOreVein;

	ObjectType GetObjectTypeForActivationText(const RE::BSString& activationText) const;

	inline std::string GetVerbFromActivationText(const RE::BSString& activationText) const
	{
		std::string strActivation;
		const char* nextChar(activationText.c_str());
		size_t index(0);
		while (!isspace(*nextChar) && index < activationText.size())
		{
			strActivation.push_back(*nextChar);
			++nextChar;
			++index;
		}
		return strActivation;
	}

	template <typename T>
	void CategorizeByKeyword()
	{
		RE::TESDataHandler* dhnd = RE::TESDataHandler::GetSingleton();
		if (!dhnd)
			return;

		// use keywords from preference
		for (T* typedForm : dhnd->GetFormArray<T>())
		{
			if (!typedForm->GetFullNameLength())
				continue;
			const char* formName(typedForm->GetFullName());
			DBG_VMESSAGE("Categorizing %s/0x%08x", formName, typedForm->GetFormID());
			if ((typedForm->formFlags & T::RecordFlags::kNonPlayable) == T::RecordFlags::kNonPlayable)
			{
				DBG_VMESSAGE("%s/0x%08x is NonPlayable", formName, typedForm->GetFormID());
				continue;
			}
			RE::BGSKeywordForm* keywordForm(typedForm->As<RE::BGSKeywordForm>());
			if (!keywordForm)
			{
				DBG_WARNING("%s/0x%08x Not a Keyword", formName, typedForm->GetFormID());
				continue;
			}

			ObjectType correctType(ObjectType::unknown);
			bool hasDefault(false);
			for (UInt32 index = 0; index < keywordForm->GetNumKeywords(); ++index)
			{
				std::optional<RE::BGSKeyword*> keyword(keywordForm->GetKeywordAt(index));
				if (!keyword)
					continue;
				const auto matched(m_objectTypeByForm.find(keyword.value()->GetFormID()));
				if (matched != m_objectTypeByForm.cend())
				{
					// if default type, postpone storage in case there is a more specific match
					if (matched->second == DefaultObjectType<T>())
					{
						hasDefault = true;
					}
					else if (correctType != ObjectType::unknown)
					{
						REL_WARNING("%s/0x%08x mapped to %s already stored with keyword %s, check data", formName, typedForm->GetFormID(),
							GetObjectTypeName(matched->second).c_str(), GetObjectTypeName(correctType).c_str());
					}
					else
					{
						correctType = matched->second;
					}
				}
			}
			if (correctType == ObjectType::unknown && hasDefault)
			{
				correctType = DefaultObjectType<T>();
			}
			else
			{
				correctType = OverrideIfBadChoice<T>(typedForm, correctType);
			}
			if (correctType != ObjectType::unknown)
			{
				if (SetObjectTypeForForm(typedForm->GetFormID(), correctType))
				{
					DBG_VMESSAGE("%s/0x%08x stored as %s", formName, typedForm->GetFormID(), GetObjectTypeName(correctType).c_str());
				}
				else
				{
					REL_WARNING("%s/0x%08x (%s) already stored, check data", formName, typedForm->GetFormID(), GetObjectTypeName(correctType).c_str());
				}
				continue;
			}

			// fail-safe is to check if the form has value and store as clutter if so
			// Also, check model path for - you guessed it - clutter. Some base game MISC objects lack keywords.
			if (typedForm->value > 0 || CheckObjectModelPath(typedForm, "clutter"))
			{
				if (SetObjectTypeForForm(typedForm->GetFormID(), ObjectType::clutter))
				{
					DBG_VMESSAGE("%s/0x%08x with value %d stored as clutter", formName, typedForm->GetFormID(), std::max(typedForm->value, SInt32(0)));
				}
				else
				{
					REL_WARNING("%s/0x%08x (defaulting as clutter) already stored, check data", formName, typedForm->GetFormID());
				}
				continue;
			}
			DBG_VMESSAGE("%s/0x%08x not mappable", formName, typedForm->GetFormID());
		}
	}

	void GetTranslationData(void);
	void ActivationVerbsByType(const char* activationVerbKey, const ObjectType objectType);
	void StoreActivationVerbs(void);
	void CategorizeByActivationVerb(void);
	void AnalyzePerks(void);

	std::string GetModelPath(const RE::TESForm* thisForm) const;
	bool CheckObjectModelPath(const RE::TESForm* thisForm, const char* arg) const;

	static DataCase* s_pInstance;

	// special case statics
	static constexpr RE::FormID LockPick = 0x0A;
	static constexpr RE::FormID Gold = 0x0F;

	void CategorizeStatics();
	void ExcludeFactionContainers();
	void ExcludeVendorContainers();
	void ExcludeImmersiveArmorsGodChest();

	template <typename T>
	T* FindExactMatch(const std::string& defaultESP, const RE::FormID maskedFormID)
	{
		RE::TESDataHandler* dhnd = RE::TESDataHandler::GetSingleton();
		if (!dhnd)
			return nullptr;

		T* typedForm(RE::TESDataHandler::GetSingleton()->LookupForm<T>(maskedFormID, defaultESP));
		if (typedForm)
		{
			DBG_MESSAGE("Found exact match 0x%08x for %s:0x%06x", typedForm->GetFormID(), defaultESP.c_str(), maskedFormID);
		}
		else
		{
			DBG_MESSAGE("No exact match for %s:0x%06x", defaultESP.c_str(), maskedFormID);
		}
		return typedForm;
	}

	template <typename T>
	T* FindBestMatch(const std::string& defaultESP, const RE::FormID maskedFormID, const std::string& name)
	{
		T* match(FindExactMatch<T>(defaultESP, maskedFormID));
		// supplied EDID and Name not checked if we match plugin/formID
		if (match)
		{
			DBG_MESSAGE("Returning exact match 0x%08x/%s for %s:0x%06x", match->GetFormID(), match->GetName(),
				defaultESP.c_str(), maskedFormID);
			return match;
		}

		// look for merged form
		RE::TESDataHandler* dhnd = RE::TESDataHandler::GetSingleton();
		if (!dhnd)
			return nullptr;

		// Check for match on name. FormID can change if this is in a merge output. Cannot use EDID as it is not loaded.
		for (T* container : dhnd->GetFormArray<T>())
		{
			if (container->GetName() == name)
			{
				if (match)
				{
					REL_MESSAGE("Ambiguity in best match 0x%08x vs for 0x%08x for %s:0x%06x/%s",
						match->GetFormID(), container->GetFormID(), defaultESP.c_str(), maskedFormID, name);
					return nullptr;
				}
				else
				{
					REL_MESSAGE("Found best match 0x%08x for %s:0x%06x", container->GetFormID(),
						defaultESP.c_str(), maskedFormID, name);
					match = container;
				}
			}
		}
		return match;
	}

	void IncludeFossilMiningExcavation();
	DataCase(void);
};

