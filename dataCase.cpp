#include "PrecompiledHeaders.h"

#include "CommonLibSSE/include/RE/BGSProjectile.h"

#include "utils.h"
#include "dataCase.h"
#include "iniSettings.h"
#include "tasks.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
	std::string GetBaseName(RE::TESForm* thisForm)
	{
		std::string result;
		if (thisForm)
		{
			RE::TESFullName* pFullName = thisForm->As<RE::TESFullName>();
			if (pFullName)
				result = pFullName->GetFullName();
		}
		return result;
	}
	bool IsFoundFile(const char* fileName)
	{
		std::ifstream ifs(fileName);
		return (ifs.fail()) ? false : true;
	}
	std::vector<std::string> Split(const std::string &str, char sep)
	{
		std::vector<std::string> result;

		auto first = str.begin();
		while (first != str.end())
		{
			auto last = first;
			while (last != str.end() && *last != sep)
				++last;
			result.push_back(std::string(first, last));
			if (last != str.end())
				++last;
			first = last;
		}
		return result;
	}
};


DataCase* DataCase::s_pInstance = nullptr;

DataCase::DataCase()
{
}

#include "strConv.h"

void DataCase::GetTranslationData()
{
	RE::Setting	* setting = RE::GetINISetting("sLanguage:General");

	std::string path = "Interface\\Translations\\";
	path += std::string(AHSE_NAME);
	path += std::string("_");
	path += std::string((setting && setting->GetType() == RE::Setting::Type::kString) ? setting->data.s : "ENGLISH");
	path += std::string(".txt");

#if _DEBUG
	_MESSAGE("Reading translations from %s", path.c_str());
#endif

	RE::BSResourceNiBinaryStream fs(path.c_str());
	if (!fs.good())
		return;

	UInt16	bom = 0;
	bool	ret = fs.read(&bom, sizeof(UInt16) / sizeof(wchar_t));
	if (!ret)
	{
#if _DEBUG
		_MESSAGE("Empty translation file.");
#endif
		return;
	}

	if (bom != 0xFEFF)
	{
#if _DEBUG
		_MESSAGE("BOM Error, file must be encoded in UCS-2 LE.");
#endif
		return;
	}

	while (true)
	{
		std::wstring buf;

		bool readOK(std::getline(fs, buf, L'\n'));
		if (!readOK) // End of file
		{
			break;
		}
		size_t len(buf.length());
		// at least $ + wchar_t + \t + wchar_t
		if (len < 4 || buf[0] != '$')
			continue;

		wchar_t last = buf[len - 1];
		if (last == '\r')
			len--;

		// null terminate
		// buf[len] = 0;

		UInt32 delimIdx = 0;
		for (UInt32 i = 0; i < len; i++)
			if (buf[i] == '\t')
				delimIdx = i;

		// at least $ + wchar_t
		if (delimIdx < 2)
			continue;

		// replace \t by \0
		buf[delimIdx] = 0;

		std::wstring key(buf, 0, delimIdx);
		std::wstring translation(buf, delimIdx+1, len - delimIdx - 1);

		std::string keyS = wide_to_utf8(key);
		std::string translationS = wide_to_utf8(translation);

		lists.translations[keyS] = translationS;

	}
#if _DEBUG
	_MESSAGE("* TranslationData(%d)", lists.translations.size());
#endif

	return;
}

bool DataCase::GetTSV(std::unordered_set<const RE::TESForm*> *tsv, const char* fileName)
{
	std::string filepath(FileUtils::GetPluginPath() + std::string(AHSE_NAME) + std::string("\\") + std::string(fileName));

	if (!::IsFoundFile(filepath.c_str()))
	{
#if _DEBUG
		_MESSAGE("* override TSV:%s not found", filepath.c_str());
#endif
		filepath = FileUtils::GetPluginPath() + std::string(AHSE_NAME) + std::string("\\default\\") + std::string(fileName);
		if (!FileUtils::IsFoundFile(filepath.c_str()))
		{
#if _DEBUG
			_MESSAGE("* default TSV:%s not found", filepath.c_str());
#endif
			return false;
		}
	}
#if _DEBUG
	_MESSAGE("Using TSV file %s", filepath.c_str());
#endif

	std::ifstream ifs(filepath);
	if (ifs.fail())
	{
#if _DEBUG
		_MESSAGE("* TSV:%s file error", filepath.c_str());
#endif
		return false;
	}

	std::string str;
	while (getline(ifs, str))
	{
		if (str[0] == '#' || str[0] == ';' || (str[0] == '/' && str[1] == '/'))
			continue;

		if (str.find_first_not_of("\t") == std::string::npos)
			continue;

		auto vec = StringUtils::Split(str, '\t');
		std::string modName = vec[0];

		UInt8 modIndex = PluginUtils::GetLoadedModIndex(vec[0].c_str());
		if (modIndex == 0xFF)
			continue;

		UInt32 formID = std::stoul(vec[1], nullptr, 16);
		formID |= (modIndex << 24);

		RE::TESForm* pForm = RE::TESForm::LookupByID(formID);
		if (pForm)
			tsv->insert(pForm);
	}

#if _DEBUG
	_MESSAGE("* TSV:%s(%d)", fileName, tsv->size());
#endif
	return true;
}

enum FactionFlags
{
	kFlag_Vender = 1 << 14,		//  4000
};

void DataCase::GetBlockContainerData()
{
	RE::TESDataHandler* dhnd = RE::TESDataHandler::GetSingleton();
	if (dhnd)
	{
		for (RE::TESForm* form : dhnd->GetFormArray(RE::FormType::Faction))
		{
			if (!form)
				continue;

			RE::TESFaction* faction = skyrim_cast<RE::TESFaction*, RE::TESForm>(form);
			if (!faction)
				continue;

			RE::TESObjectREFR* containerRef = nullptr;
			if (faction->data.kVendor)
			{
				containerRef = (faction->vendorData).merchantContainer;
				if (containerRef)
				{
#if _DEBUG2
					_MESSAGE("[ADD:%d] vendor container : %s(%08X)", index, CALL_MEMBER_FN(containerRef, GetReferenceName)(), containerRef->formID);
#endif
					BlockReference(containerRef);
				}
			}

			containerRef = faction->crimeData.factionStolenContainer;
			if (containerRef)
			{
#if _DEBUG2
				_MESSAGE("[ADD:%d] stolenGoodsContainer : %s(%08X)", index, CALL_MEMBER_FN(containerRef, GetReferenceName)(), containerRef->formID);
#endif
				BlockReference(containerRef);
			}

			containerRef = faction->crimeData.factionPlayerInventoryContainer;
			if (containerRef)
			{
#if _DEBUG2
				_MESSAGE("[ADD:%d] playerInventoryContainer : %s(%08X)", index, CALL_MEMBER_FN(containerRef, GetReferenceName)(), containerRef->formID);
#endif
				BlockReference(containerRef);
			}
		}
	}
}

void DataCase::GetAmmoData()
{
	RE::TESDataHandler* dhnd = RE::TESDataHandler::GetSingleton();
	if (!dhnd)
		return;

#if _DEBUG
	_MESSAGE("Loading AmmoData");
#endif
	for (RE::TESForm* form : dhnd->GetFormArray(RE::FormType::Ammo))
	{
		RE::TESAmmo* ammo(form->As<RE::TESAmmo>());
		if (!ammo)
			continue;
#if _DEBUG
		_MESSAGE("Checking %s", ammo->GetFullName());
#endif

		if (!ammo->GetPlayable())
		{
#if _DEBUG
			_MESSAGE("Not playable");
#endif
			continue;
		}

		std::string name;
		name = ::GetBaseName(ammo);
		if (name.empty())
		{
#if _DEBUG
			_MESSAGE("base name empty");
#endif
			continue;
     	}
#if _DEBUG
		_MESSAGE("base name %s", name.c_str());
#endif

		RE::BGSProjectile* proj = ammo->data.projectile;
		if (!proj)
			continue;

#if _DEBUG
		_MESSAGE("Adding Projectile %s with ammo %s", proj->GetFullName(), ammo->GetFullName());
#endif
		lists.ammoList[proj] = ammo;
	}

#if _DEBUG
	_MESSAGE("* AmmoData(%d)", lists.ammoList.size());
#endif
}

bool DataCase::BlockReference(const RE::TESObjectREFR* refr)
{
	if (!refr)
		return false;
	return (lists.blockRefr.insert(refr)).second;
}

bool DataCase::UnblockReference(const RE::TESObjectREFR* refr)
{
	if (!refr)
		return false;
	return lists.blockRefr.erase(refr) > 0;
}

bool DataCase::IsReferenceBlocked(const RE::TESObjectREFR* refr)
{
	if (!refr)
		return false;
	return lists.blockRefr.count(refr) > 0;
}

ObjectType DataCase::GetFormObjectType(RE::FormID formID) const
{
	const auto entry(m_objectTypeByForm.find(formID));
	if (entry != m_objectTypeByForm.cend())
		return entry->second;
	return ObjectType::unknown;
}

bool DataCase::SetFormObjectType(RE::FormID formID, ObjectType objectType)
{
	return m_objectTypeByForm.insert(std::make_pair(formID, objectType)).second;
}

ObjectType DataCase::GetObjectTypeForFormType(RE::FormType formType) const
{
	const auto entry(m_objectTypeByFormType.find(formType));
	if (entry != m_objectTypeByFormType.cend())
		return entry->second;
	return ObjectType::unknown;
}

const RE::TESAmmo* DataCase::ProjToAmmo(const RE::BGSProjectile* proj)
{
	return (proj && lists.ammoList.find(proj) != lists.ammoList.end()) ? lists.ammoList[proj] : nullptr;
}

const RE::TESForm* DataCase::ConvertIfLeveledItem(const RE::TESForm* form) const
{
	const RE::TESProduceForm* produceForm(form->As<RE::TESProduceForm>());
	if (produceForm)
	{
		const auto matched(m_leveledItemContents.find(produceForm));
		if (matched != m_leveledItemContents.cend())
		{
			return matched->second;
		}
	}
	return form;
}

void DataCase::ListsClear()
{
	lists.blockRefr.clear();
	lists.arrowCheck.clear();
}

void DataCase::BuildList()
{
	if (!GetTSV(&lists.blockForm, "blocklist.tsv"))
		GetTSV(&lists.blockForm, "default\\blocklist.tsv");

	GetAmmoData();

	CategorizeStatics();

	SetObjectTypeByKeywords();

	// consumable item categorization is useful for Activator, Flora, Tree and direct access
	CategorizeConsumables();

	CategorizeByKeyword<RE::TESObjectACTI>();
	CategorizeByKeyword<RE::TESObjectMISC>();
	CategorizeByKeyword<RE::TESObjectARMO>();

	// classes inheriting from TESProduceForm may have an ingredient, categorized as the appropriate consumable
	CategorizeByIngredient<RE::TESFlora>();
	CategorizeByIngredient<RE::TESObjectTREE>();

	GetBlockContainerData();
	GetTranslationData();
}

void DataCase::CategorizeConsumables()
{
	RE::TESDataHandler* dhnd = RE::TESDataHandler::GetSingleton();
	if (!dhnd)
		return;
	for (RE::TESForm* form : dhnd->GetFormArray(RE::AlchemyItem::FORMTYPE))
	{
		RE::AlchemyItem* consumable(form->As<RE::AlchemyItem>());
		if (!consumable)
		{
#if _DEBUG
			_MESSAGE("Skipping non-consumable form 0x%08x", form->formID);
#endif
			continue;
		}

		RE::TESFullName* pFullName = form->As<RE::TESFullName>();
		if (!pFullName || pFullName->GetFullNameLength() == 0)
		{
#if _DEBUG
			_MESSAGE("Skipping unnamed form 0x%08x", form->formID);
#endif
			continue;
		}

		std::string formName(pFullName->GetFullName());
		if (GetFormObjectType(form->formID) != ObjectType::unknown)
		{
#if _DEBUG
			_MESSAGE("Skipping previously categorized form %s/0x%08x", formName.c_str(), form->formID);
#endif
			continue;
		}

		const static RE::FormID drinkSound = 0x0B6435;
		ObjectType objectType(ObjectType::unknown);
		if (consumable->IsFood())
			objectType = (consumable->data.consumptionSound && consumable->data.consumptionSound->formID == drinkSound) ? ObjectType::drink : ObjectType::food;
		else
			objectType = (consumable->IsPoison()) ? ObjectType::poison : ObjectType::potion;

#if _DEBUG
		_MESSAGE("Consumable %s/0x%08x has type %s", formName.c_str(), form->formID, GetObjectTypeName(objectType).c_str());
#endif
		m_objectTypeByForm[form->formID] = objectType;
	}
}

void DataCase::SetObjectTypeByKeywords()
{
	RE::TESDataHandler* dhnd = RE::TESDataHandler::GetSingleton();
	if (!dhnd)
		return;

	// Classify Vendor Items of uncertain taxonomy
	std::unordered_map<std::string, ObjectType> typeByVendorItem =
	{ 
		// all in Skyrim core
		{"VendorItemGem", ObjectType::gems},
		{"VendorItemOreIngot", ObjectType::oreIngot},
		{"VendorItemAnimalHide", ObjectType::animalHide},
		{"VendorItemAnimalPart", ObjectType::animalParts},
		{"VendorItemJewelry", ObjectType::jewelry},
		{"VendorItemArmor", ObjectType::armor},
		{"VendorItemClutter", ObjectType::clutter}
	};
	for (RE::TESForm* form : dhnd->GetFormArray(RE::BGSKeyword::FORMTYPE))
	{
		RE::BGSKeyword* keywordDef(form->As<RE::BGSKeyword>());
		if (!keywordDef)
		{
#if _DEBUG
			_MESSAGE("Skipping non-keyword form 0x%08x", form->formID);
#endif
    		continue;
		}

		std::string keywordName(keywordDef->GetFormEditorID());
		// Store player house keyword for SearchTask usage
		if (keywordName == "LocTypePlayerHouse")
		{
			SearchTask::SetPlayerHouseKeyword(keywordDef);
			continue;
		}

		ObjectType objectType(ObjectType::unknown);
		const auto matched(typeByVendorItem.find(keywordName));
		if (matched != typeByVendorItem.cend())
		{
			objectType = matched->second;
	    }
		else if (keywordName.starts_with("VendorItem"))
		{
#if _DEBUG
			_MESSAGE("%s/0x%08x treated as clutter", keywordName.c_str(), form->formID);
#endif
			objectType = ObjectType::clutter;
		}
		else
		{
#if _DEBUG
			_MESSAGE("%s/0x%08x skipped", keywordName.c_str(), form->formID);
#endif
			continue;
		}
		m_objectTypeByForm[form->formID] = objectType;
#if _DEBUG
		_MESSAGE("%s/0x%08x stored as %s", keywordName.c_str(), form->formID, GetObjectTypeName(objectType).c_str());
#endif
	}
}

template <> ObjectType DataCase::DefaultObjectType<RE::TESObjectARMO>()
{
	return ObjectType::armor;
}

// ingredient nullptr indicates this critter is pending resolution
bool DataCase::SetIngredientForCritter(RE::TESForm* critter, RE::TESForm* ingredient)
{
	concurrency::critical_section::scoped_lock guard(m_critterIngredientLock);
	if (!ingredient)
	{
#if _DEBUG
		_MESSAGE("Critter %s/0x%08x pending resolution to ingredient", critter->GetName(), critter->formID);
#endif
		// return value signals entry pending resolution found/not found
		return m_critterIngredient.insert(std::make_pair(critter->As<RE::TESObjectACTI>(), nullptr)).second;
	}
	else
	{
#if _DEBUG
		_MESSAGE("Critter %s/0x%08x has ingredient %s/0x%08x", critter->GetName(), critter->formID, ingredient->GetName(), ingredient->formID);
#endif
		m_critterIngredient[critter->As<RE::TESObjectACTI>()] = ingredient->As<RE::IngredientItem>();
		return true;
	}
}

const RE::IngredientItem* DataCase::GetIngredientForCritter(RE::TESObjectACTI* activator) const
{
	concurrency::critical_section::scoped_lock guard(m_critterIngredientLock);
	const auto matched(m_critterIngredient.find(activator));
	if (matched != m_critterIngredient.cend())
		return matched->second;
	return nullptr;
}

std::string DataCase::GetModelPath(const RE::TESForm* thisForm) const
{
	if (thisForm)
	{
		const RE::TESObjectMISC* miscObject(thisForm->As<RE::TESObjectMISC>());
		if (miscObject)
			return miscObject->GetModel();
		const RE::TESObjectCONT* container(thisForm->As<RE::TESObjectCONT>());
		if (container)
			return container->GetModel();
	}
	return std::string();
}

bool DataCase::CheckObjectModelPath(const RE::TESForm* thisForm, const char* arg) const
{
	if (!thisForm || strlen(arg) == 0)
		return false;
	std::string s = GetModelPath(thisForm);
	if (s.empty())
		return false;
	StringUtils::ToLower(s);
	return (s.find(arg, 0) != std::string::npos) ? true : false;
}

void DataCase::CategorizeStatics()
{
	// These form types always map to the same Object Type
	m_objectTypeByFormType[RE::FormType::Container] = ObjectType::container;
	m_objectTypeByFormType[RE::FormType::Ingredient] = ObjectType::ingredients;
	m_objectTypeByFormType[RE::FormType::SoulGem] = ObjectType::soulgem;
	m_objectTypeByFormType[RE::FormType::KeyMaster] = ObjectType::keys;
	m_objectTypeByFormType[RE::FormType::Scroll] = ObjectType::scrolls;
	m_objectTypeByFormType[RE::FormType::Ammo] = ObjectType::ammo;
	m_objectTypeByFormType[RE::FormType::Light] = ObjectType::light;

	// Map well-known forms to ObjectType
	m_objectTypeByForm[LockPick] = ObjectType::lockpick;
	m_objectTypeByForm[Gold] = ObjectType::septims;
}

template <>
ObjectType DataCase::IngredientObjectType(const RE::TESFlora* form)
{
	return ObjectType::flora;
}

template <>
ObjectType DataCase::IngredientObjectType(const RE::TESObjectTREE* form)
{
	return ObjectType::food;
}

DataCase::LeveledItemCategorizer::LeveledItemCategorizer(const RE::TESProduceForm* produceForm,
	const RE::TESLevItem* rootItem, const std::string& targetName) : 
	m_produceForm(produceForm), m_rootItem(rootItem), m_contents(nullptr), m_objectType(ObjectType::unknown), m_targetName(targetName)
{
}

std::pair<RE::TESForm*, ObjectType>  DataCase::LeveledItemCategorizer::FindContents()
{
	FindContentsAtLevel(m_rootItem);
	return std::make_pair(m_contents, m_objectType);
}

void DataCase::LeveledItemCategorizer::FindContentsAtLevel(const RE::TESLevItem* leveledItem)
{
	for (const RE::LEVELED_OBJECT& leveledObject : leveledItem->entries)
	{
		RE::TESForm* itemForm(leveledObject.form);
		if (!itemForm)
			continue;
		// Handle nesting of leveled items
		RE::TESLevItem* leveledItem(itemForm->As<RE::TESLevItem>());
		if (leveledItem)
		{
			FindContentsAtLevel(leveledItem);
			continue;
		}
		ObjectType itemType(DataCase::GetInstance()->GetObjectTypeForForm(itemForm));
		if (itemType != ObjectType::unknown)
		{
			if (!m_contents)
			{
#if _DEBUG
				_MESSAGE("Target %s/0x%08x has contents type %s in form %s/0x%08x", m_targetName.c_str(), m_rootItem->formID,
					GetObjectTypeName(itemType).c_str(), itemForm->GetName(), itemForm->formID);
#endif
				m_contents = itemForm;
				m_objectType = itemType;
			}
			else if (m_contents == itemForm)
			{
#if _DEBUG
				_MESSAGE("Target %s/0x%08x contents type %s already recorded", m_targetName.c_str(), m_rootItem->formID,
					GetObjectTypeName(itemType).c_str());
#endif
			}
			else
			{
#if _DEBUG
				_MESSAGE("Target %s/0x%08x contents type %s already stored under different form %s/0x%08x", m_targetName.c_str(), m_rootItem->formID,
					GetObjectTypeName(itemType).c_str(), m_contents->GetName(), m_contents->formID);
#endif
			}
		}
	}

}

