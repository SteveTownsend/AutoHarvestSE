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

#include <unordered_map>

namespace shse
{

	class InventoryEntry
	{
	public:
		InventoryEntry(RE::TESBoundObject* item, const int count);

		static constexpr int UnlimitedItems = 1000000;

		ExcessInventoryHandling HandlingType() const;
		void Populate();
		int Headroom(const int delta) const;
		void HandleExcess(const RE::TESBoundObject* item);

	private:
		RE::TESBoundObject* m_item;
		ExcessInventoryHandling m_excessHandling;
		ObjectType m_excessType;
		bool m_crafting;
		int m_count;
		mutable int m_totalDelta;		// number of items assumed added by loot requests since last reconciliation
		int m_maxCount;
		uint32_t m_value;
		double m_weight;
	};

	typedef std::unordered_map<const RE::TESBoundObject*, InventoryEntry> InventoryCache;
}
