#pragma once

namespace shse
{

class PlayerState
{
public:
	static PlayerState& Instance();
	PlayerState();

	void Refresh();
	void ResetCarryWeight(const bool reloaded);
	void CheckPerks(const bool force);
	bool PerksAddLeveledItemsOnDeath() const;
	bool CanLoot() const;
	bool IsSneaking() const;
	void ExcludeMountedIfForbidden(void);

private:
	void AdjustCarryWeight();
	bool IsMagicallyConcealed(RE::MagicTarget* target) const;

	static std::unique_ptr<PlayerState> m_instance;

	static constexpr int InfiniteWeight = 100000;
	static constexpr int PerkCheckIntervalSeconds = 15;
	std::chrono::time_point<std::chrono::high_resolution_clock> m_lastPerkCheck;
	bool m_perksAddLeveledItemsOnDeath;

	bool m_carryAdjustedForCombat;
	bool m_carryAdjustedForPlayerHome;
	bool m_carryAdjustedForDrawnWeapon;
	int m_currentCarryWeightChange;

	bool m_sneaking;
	bool m_disableWhileMounted;

	mutable RecursiveLock m_playerLock;
};

}