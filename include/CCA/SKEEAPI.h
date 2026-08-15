#pragma once

#include <cstdint>

namespace RE
{
	class TESObjectREFR;
}

namespace CCA::SKEE
{
	class IPluginInterface
	{
	public:
		IPluginInterface() = default;
		virtual ~IPluginInterface() = default;

		virtual std::uint32_t GetVersion() = 0;
		virtual void Revert() = 0;
	};

	class IInterfaceMap
	{
	public:
		virtual IPluginInterface* QueryInterface(const char* a_name) = 0;
		virtual bool AddInterface(const char* a_name, IPluginInterface* a_interface) = 0;
		virtual IPluginInterface* RemoveInterface(const char* a_name) = 0;
	};

	struct InterfaceExchangeMessage
	{
		static constexpr std::uint32_t kMessage_ExchangeInterface = 0x9E3779B9;

		IInterfaceMap* interfaceMap{ nullptr };
	};

	class ICommandInterface : public IPluginInterface
	{
	public:
		using CommandCallback = bool (*)(RE::TESObjectREFR*, const char*);

		virtual bool RegisterCommand(const char* a_command, const char* a_description, CommandCallback a_callback) = 0;
	};
}
