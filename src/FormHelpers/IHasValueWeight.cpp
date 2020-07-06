#include "PrecompiledHeaders.h"

#include "Data/iniSettings.h"
#include "FormHelpers/IHasValueWeight.h"

ObjectType IHasValueWeight::GetObjectType() const
{
	return m_objectType;
}

std::string IHasValueWeight::GetTypeName() const
{
	return m_typeName;
}

constexpr const char* VW_Default = "valueWeightDefault";

double IHasValueWeight::GetWorth(void) const
{
	if (!m_worthSetup)
	{
		m_worth = CalculateWorth();
		m_worthSetup = true;
	}
	return m_worth;
}

bool IHasValueWeight::ValueWeightTooLowToLoot(const SInt32 itemValue) const
{
	double worth(itemValue > 0 ? itemValue : GetWorth());
	DBG_VMESSAGE("Checking value: %.2f", worth);

	// valuable objects overrides V/W checks
	if (IsValuable(static_cast<SInt32>(worth)))
		return false;

	INIFile* settings(INIFile::GetInstance());
	// A specified default for value-weight supersedes a missing type-specific value-weight
	double valueWeight(settings->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::valueWeight, m_typeName.c_str()));
	if (valueWeight <= 0.)
	{
		valueWeight = settings->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::config, VW_Default);
	}

	if (valueWeight > 0.)
	{
		double weight = std::max(GetWeight(), 0.);
		if (itemValue > 0. && weight <= 0.)
		{
			DBG_VMESSAGE("%s(%08x) has value %0.2f, weightless", GetName(), GetFormID(), worth);
			return false;
		}

		if (worth <= 0.)
		{
			if (weight <= 0.)
			{
				// this may be a scripted activator without special-case handling - one example is Poison Bloom (xx007cda).
				// Harvest if non v/w criteria say we should do so.
				DBG_VMESSAGE("%s(%08x) - cannot calculate v/w from weight %0.2f and value %0.2f", GetName(), GetFormID(), weight, worth);
				return false;
			}
			else
			{
				// zero value object with strictly positive weight - do not auto-harvest
				DBG_VMESSAGE("%s(%08x) - has weight %0.2f, no value", GetName(), GetFormID(), weight);
				return true;
			}
		}

		double vw = (worth > 0. && weight > 0.) ? worth / weight : 0.0;
		DBG_VMESSAGE("%s(%08x) item VW %0.2f vs threshold VW %0.2f", GetName(), GetFormID(), vw, valueWeight);
		// allow small tolerance for floating point uncertainty
		if (vw < valueWeight - 0.01)
			return true;
	}
	return false;
}

bool IHasValueWeight::IsValuable(const SInt32 itemValue) const
{
	if (itemValue > 0)
	{
		double minValue(INIFile::GetInstance()->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::config, "ValuableItemThreshold"));
		// allow small tolerance for floating point uncertainty
		if (minValue > 0. && static_cast<double>(itemValue) >= minValue - 0.01)
		{
			DBG_VMESSAGE("%s(%08x) has value %d vs threshold %0.2f: Valuable", GetName(), GetFormID(), itemValue, minValue);
			return true;
		}
	}
	return false;
}