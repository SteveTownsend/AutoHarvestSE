/*************************************************************************
SmartHarvest SE
Copyright (c) Steve Townsend 2020

>>> SOURCE LICENSE >>>
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation (www.fsf.org); either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

A copy of the GNU General Public License is available at
http://www.fsf.org/licensing/licenses
>>> END OF LICENSE >>>
*************************************************************************/
#pragma once

#include "Collections/Collection.h"

namespace shse {

class CollectionManager {
public:
	static CollectionManager& Instance();

	CollectionManager();
	void ProcessDefinitions(void);
	std::pair<bool, SpecialObjectHandling> TreatAsCollectible(const ConditionMatcher& matcher);
	void Refresh() const;
	void UpdateGameTime(const float gameTime);
	void CheckEnqueueAddedItem(const RE::FormID formID);
	void ProcessAddedItems();
	inline bool IsActive() const { return m_enabled && m_ready; }
	inline bool IsAvailable() const { return m_ready; }
	void OnGameReload(void);
	void PrintDefinitions(void) const;
	void PrintMembership(void) const;
	int NumberOfFiles(void) const;
	std::string GroupNameByIndex(const int fileIndex) const;
	std::string GroupFileByIndex(const int fileIndex) const;
	int NumberOfCollections(const std::string& groupName) const;
	std::string NameByGroupIndex(const std::string& groupName, const int collectionIndex) const;
	static std::string MakeLabel(const std::string& groupName, const std::string& collectionName);

	bool PolicyRepeat(const std::string& groupName, const std::string& collectionName) const;
	bool PolicyNotify(const std::string& groupName, const std::string& collectionName) const;
	SpecialObjectHandling PolicyAction(const std::string& groupName, const std::string& collectionName) const;
	void PolicySetRepeat(const std::string& groupName, const std::string& collectionName, const bool allowRepeats);
	void PolicySetNotify(const std::string& groupName, const std::string& collectionName, const bool notify);
	void PolicySetAction(const std::string& groupName, const std::string& collectionName, const SpecialObjectHandling action);

	bool GroupPolicyRepeat(const std::string& groupName) const;
	bool GroupPolicyNotify(const std::string& groupName) const;
	SpecialObjectHandling GroupPolicyAction(const std::string& groupName) const;
	void GroupPolicySetRepeat(const std::string& groupName, const bool allowRepeats);
	void GroupPolicySetNotify(const std::string& groupName, const bool notify);
	void GroupPolicySetAction(const std::string& groupName, const SpecialObjectHandling action);

	size_t TotalItems(const std::string& groupName, const std::string& collectionName) const;
	size_t ItemsObtained(const std::string& groupName, const std::string& collectionName) const;
	bool IsPlacedObject(const RE::TESForm* form) const;
	void RecordPlacedObjects(void);

private:
	bool LoadData(void);
	bool LoadCollectionGroup(
		const std::filesystem::path& defFile, const std::string& groupName, nlohmann::json_schema::json_validator& validator);
	void BuildDecisionTrees(const std::shared_ptr<CollectionGroup>& collectionGroup);
	void RecordPlacedItem(const RE::TESForm* item, const RE::TESObjectREFR* refr);
	void SaveREFRIfPlaced(const RE::TESObjectREFR* refr);
	bool IsCellLocatable(const RE::TESObjectCELL* cell);
	void RecordPlacedObjectsForCell(const RE::TESObjectCELL* cell);
	void ResolveMembership(void);
	void AddToRelevantCollections(RE::FormID itemID);
	std::vector<RE::FormID> ReconcileInventory();
	void EnqueueAddedItem(const RE::FormID formID);

	static std::unique_ptr<CollectionManager> m_instance;
	bool m_ready;
	bool m_enabled;
	float m_gameTime;

	mutable RecursiveLock m_collectionLock;
	std::unordered_map<std::string, std::shared_ptr<Collection>> m_allCollectionsByLabel;
	std::multimap<std::string, std::string> m_collectionsByGroupName;
	std::unordered_map<std::string, std::string> m_fileNamesByGroupName;
	std::unordered_map<std::string, std::shared_ptr<CollectionGroup>> m_allGroupsByName;
	// Link each Form to the Collections in which it belongs
	std::unordered_multimap<RE::FormID, std::shared_ptr<Collection>> m_collectionsByFormID;
	std::unordered_set<RE::FormID> m_nonCollectionForms;
	std::unordered_set<const RE::TESForm*> m_placedItems;
	std::unordered_multimap<const RE::TESForm*, const RE::TESObjectREFR*> m_placedObjects;
	std::unordered_set<const RE::TESObjectCELL*> m_checkedForPlacedObjects;

	std::vector<RE::FormID> m_addedItemQueue;
	std::unordered_set<RE::FormID> m_lastInventoryItems;
	std::chrono::time_point<std::chrono::high_resolution_clock> m_lastInventoryCheck;
};

}
