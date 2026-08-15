#include "pch.h"

using namespace REL::literals;

namespace CCA
{
	constexpr std::string_view kPluginName = "Mirrorborn";
	constexpr std::string_view kPluginVersion = "1.0.0";
	constexpr std::uint32_t kSerializationID = 0x4D52424E;      // MRBN
	constexpr std::uint32_t kAppearanceRecord = 0x4D524150;     // MRAP
	constexpr std::uint32_t kAppearanceVersion = 4;
	constexpr std::size_t kTraversalLimit = 4096;

	struct OriginalMetrics
	{
		float effectiveScale{ 1.0F };
		float weight{ 50.0F };
	};

	struct AppearanceState
	{
		bool active{ false };
		RE::TESNPC* playerBase{ nullptr };
		RE::TESNPC* sourceBase{ nullptr };
		RE::TESRace* sourceRace{ nullptr };
		float sourceEffectiveScale{ 1.0F };
		OriginalMetrics original{};
	};

	struct PendingState
	{
		bool present{ false };
		RE::TESNPC* sourceBase{ nullptr };
		RE::TESRace* sourceRace{ nullptr };
		RE::TESRace* legacyOriginalRace{ nullptr };
		float sourceEffectiveScale{ 1.0F };
		OriginalMetrics original{};
	};

	struct SerializedAppearanceV4
	{
		RE::FormID sourceBaseID{ 0 };
		RE::FormID sourceRaceID{ 0 };
		float sourceEffectiveScale{ 1.0F };
		float originalEffectiveScale{ 1.0F };
		float originalWeight{ 50.0F };
	};
	static_assert(sizeof(SerializedAppearanceV4) == 20);

	struct SerializedAppearanceV3
	{
		RE::FormID sourceBaseID{ 0 };
		RE::FormID sourceRaceID{ 0 };
		float sourceEffectiveScale{ 1.0F };
		float originalEffectiveScale{ 1.0F };
		float originalWeight{ 50.0F };
		RE::FormID originalPlayerRaceID{ 0 };
	};
	static_assert(sizeof(SerializedAppearanceV3) == 24);

	struct OperationResult
	{
		bool success{ false };
		std::string message;
	};

	struct TransformCopyResult
	{
		std::size_t firstPersonMatches{ 0 };
		std::size_t thirdPersonMatches{ 0 };
		bool truncated{ false };
		std::string warning;
	};

	std::recursive_mutex g_stateLock;
	AppearanceState g_state;
	PendingState g_pending;
	std::atomic_bool g_hooksInstalled{ false };
	std::atomic_bool g_commandRegistered{ false };

	using FaceBuilder_t = void (*)(RE::TESNPC*, RE::Actor*);
	using BodyRootBuilder_t = void (*)(RE::TESNPC*, RE::NiPointer<RE::NiNode>*, void*);
	using BodyAttach_t = void (*)(RE::TESNPC*, RE::Actor*, RE::NiPointer<RE::NiNode>*);

	FaceBuilder_t g_originalFaceBuilder{ nullptr };
	BodyRootBuilder_t g_originalBodyRootBuilder{ nullptr };
	BodyAttach_t g_originalBodyAttach{ nullptr };

	[[nodiscard]] bool IsFiniteInRange(float a_value, float a_min, float a_max) noexcept
	{
		return std::isfinite(a_value) && a_value >= a_min && a_value <= a_max;
	}

	void ConsolePrint(std::string_view a_message)
	{
		if (auto* console = RE::ConsoleLog::GetSingleton()) {
			const std::string text{ a_message };
			console->Print("%s", text.c_str());
		}
	}

	template <class... Args>
	void ConsolePrint(std::format_string<Args...> a_format, Args&&... a_args)
	{
		ConsolePrint(std::format(a_format, std::forward<Args>(a_args)...));
	}

	[[nodiscard]] std::string FormName(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return "<none>";
		}
		const char* name = a_form->GetName();
		return name && *name ? std::string{ name } : std::format("Form {:08X}", a_form->GetFormID());
	}

	[[nodiscard]] std::string NormalizeArgument(const char* a_argument)
	{
		std::string value = a_argument ? a_argument : "";
		const auto first = value.find_first_not_of(" \t\r\n");
		if (first == std::string::npos) {
			return {};
		}
		value.erase(0, first);
		if (const auto last = value.find_first_of(" \t\r\n"); last != std::string::npos) {
			value.resize(last);
		}
		std::ranges::transform(value, value.begin(), [](unsigned char a_ch) {
			return static_cast<char>(std::tolower(a_ch));
		});
		return value;
	}

	[[nodiscard]] bool UsesDefaultBehaviorGraph(const RE::TESRace* a_race, RE::SEX a_sex)
	{
		if (!a_race || (a_sex != RE::SEX::kMale && a_sex != RE::SEX::kFemale)) {
			return false;
		}
		const char* graphPath = a_race->behaviorGraphs[a_sex].GetModel();
		if (!graphPath || !*graphPath) {
			return false;
		}
		std::string graph{ graphPath };
		std::ranges::transform(graph, graph.begin(), [](unsigned char a_ch) {
			return static_cast<char>(std::tolower(a_ch));
		});
		if (graph.size() < 4) {
			return false;
		}
		graph.resize(graph.size() - 4);  // Match the original mod's extension-agnostic check.
		const std::string_view expected = a_sex == RE::SEX::kFemale ? "defaultfemale" : "defaultmale";
		return graph.ends_with(expected);
	}

	[[nodiscard]] std::optional<std::string> ValidateCompatibility(
		RE::PlayerCharacter* a_player,
		RE::TESNPC* a_playerBase,
		RE::Actor* a_sourceActor,
		RE::TESNPC* a_sourceBase,
		RE::TESRace* a_sourceRace,
		float a_sourceEffectiveScale)
	{
		if (!g_hooksInstalled.load()) {
			return "the 1.6.1170 appearance hooks are not installed; see the SKSE log";
		}
		if (!a_player || !a_playerBase || !a_sourceBase || !a_sourceRace) {
			return "the player or donor base data is unavailable";
		}
		if (a_sourceBase == a_playerBase || a_sourceBase->IsPlayer()) {
			return "the player cannot be used as an appearance donor";
		}
		if (a_sourceBase->IsDeleted() || a_sourceRace->IsDeleted()) {
			return "the donor uses a deleted base or race record";
		}
		if (a_sourceBase->IsDynamicForm() || a_sourceRace->IsDynamicForm()) {
			return "temporary/dynamic donor forms cannot be persisted safely";
		}
		if (a_sourceBase->race != a_sourceRace) {
			return "the donor base and race no longer agree";
		}
		if (a_playerBase->race == nullptr) {
			return "the player race is unavailable";
		}
		if (a_sourceRace->IsChildRace() || a_playerBase->race->IsChildRace()) {
			return "child races are not supported";
		}
		const auto sourceSex = a_sourceBase->GetSex();
		const auto playerSex = a_playerBase->GetSex();
		if (sourceSex != playerSex || (sourceSex != RE::SEX::kMale && sourceSex != RE::SEX::kFemale)) {
			return "the donor and player must have the same supported sex";
		}
		if (!UsesDefaultBehaviorGraph(a_sourceRace, sourceSex) ||
			!UsesDefaultBehaviorGraph(a_playerBase->race, playerSex)) {
			return "both races must use a defaultmale/defaultfemale humanoid behavior graph";
		}
		const char* skeleton = a_sourceRace->skeletonModels[sourceSex].GetModel();
		if (!skeleton || !*skeleton) {
			return "the donor race has no skeleton model";
		}
		if (!IsFiniteInRange(a_sourceBase->weight, 0.0F, 100.0F)) {
			return "the donor has an invalid weight";
		}
		if (!IsFiniteInRange(a_sourceEffectiveScale, 0.05F, 20.0F)) {
			return "the donor has an invalid effective scale";
		}
		if (!IsFiniteInRange(a_playerBase->GetHeight(), 0.05F, 20.0F)) {
			return "the player base has an invalid height multiplier";
		}
		if (a_player->IsOnMount()) {
			return "dismount before changing appearance";
		}
		if (a_sourceActor && a_sourceActor->IsOnMount()) {
			return "the selected donor must be dismounted";
		}
		return std::nullopt;
	}

	[[nodiscard]] bool StateReadyForHooksLocked(RE::PlayerCharacter* a_player) noexcept
	{
		return g_state.active && a_player && g_state.playerBase && g_state.sourceBase && g_state.sourceRace &&
			!g_state.sourceBase->IsDeleted() && !g_state.sourceRace->IsDeleted() &&
			g_state.sourceBase->race == g_state.sourceRace;
	}

	class ScopedDonorContext
	{
	public:
		explicit ScopedDonorContext(bool a_requested) :
			_lock(g_stateLock)
		{
			if (!a_requested) {
				return;
			}
			_player = RE::PlayerCharacter::GetSingleton();
			if (!StateReadyForHooksLocked(_player)) {
				return;
			}
			_playerBase = g_state.playerBase;
			_sourceBase = g_state.sourceBase;
			_sourceRace = g_state.sourceRace;
			_oldObject = _player->data.objectReference;
			_oldPlayerRace = _playerBase->race;
			_oldSourceRaceSkin = _sourceRace->skin;

			if (_sourceBase->skin) {
				_sourceRace->skin = _sourceBase->skin;
			}
			_playerBase->race = _sourceRace;
			_player->data.objectReference = _sourceBase;
			_enabled = true;
		}

		~ScopedDonorContext()
		{
			if (!_enabled) {
				return;
			}
			_player->data.objectReference = _oldObject;
			_playerBase->race = _oldPlayerRace;
			_sourceRace->skin = _oldSourceRaceSkin;
		}

		ScopedDonorContext(const ScopedDonorContext&) = delete;
		ScopedDonorContext& operator=(const ScopedDonorContext&) = delete;

		[[nodiscard]] bool Enabled() const noexcept { return _enabled; }
		[[nodiscard]] RE::TESNPC* SourceBase() const noexcept { return _sourceBase; }

	private:
		std::unique_lock<std::recursive_mutex> _lock;
		RE::PlayerCharacter* _player{ nullptr };
		RE::TESNPC* _playerBase{ nullptr };
		RE::TESNPC* _sourceBase{ nullptr };
		RE::TESRace* _sourceRace{ nullptr };
		RE::TESBoundObject* _oldObject{ nullptr };
		RE::TESRace* _oldPlayerRace{ nullptr };
		RE::TESObjectARMO* _oldSourceRaceSkin{ nullptr };
		bool _enabled{ false };
	};

	class ScopedPlayerFormIDSpoof
	{
	public:
		explicit ScopedPlayerFormIDSpoof(RE::Actor* a_actor) :
			_actor(a_actor)
		{
			if (_actor && _actor->formID == 0x14) {
				_original = _actor->formID;
				_actor->formID = 0;
				_enabled = true;
			}
		}

		~ScopedPlayerFormIDSpoof()
		{
			if (_enabled) {
				_actor->formID = _original;
			}
		}

		ScopedPlayerFormIDSpoof(const ScopedPlayerFormIDSpoof&) = delete;
		ScopedPlayerFormIDSpoof& operator=(const ScopedPlayerFormIDSpoof&) = delete;

	private:
		RE::Actor* _actor{ nullptr };
		RE::FormID _original{ 0 };
		bool _enabled{ false };
	};

	[[nodiscard]] bool GeometryConflictsWithWornMask(RE::BSGeometry* a_geometry, std::uint32_t a_wornMask)
	{
		if (!a_geometry) {
			return false;
		}
		auto* skin = a_geometry->GetGeometryRuntimeData().skinInstance.get();
		auto* dismember = netimmerse_cast<RE::BSDismemberSkinInstance*>(skin);
		if (!dismember) {
			return false;
		}
		const auto& runtime = dismember->GetRuntimeData();
		if (!runtime.partitions || runtime.numPartitions <= 0 || runtime.numPartitions > 512) {
			return false;
		}
		for (std::int32_t i = 0; i < runtime.numPartitions; ++i) {
			const auto slot = runtime.partitions[i].slot;
			if (((a_wornMask & 0x2) != 0 && (slot == 31 || slot == 131)) ||
				((a_wornMask & 0x800) != 0 && (slot == 41 || slot == 141))) {
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] bool SubtreeConflictsWithWornMask(RE::NiAVObject* a_root, std::uint32_t a_wornMask)
	{
		if (!a_root) {
			return false;
		}
		std::vector<RE::NiAVObject*> stack;
		stack.reserve(64);
		stack.push_back(a_root);
		std::size_t visited = 0;
		while (!stack.empty() && visited++ < kTraversalLimit) {
			auto* object = stack.back();
			stack.pop_back();
			if (GeometryConflictsWithWornMask(object->AsGeometry(), a_wornMask)) {
				return true;
			}
			if (auto* node = object->AsNode()) {
				const auto& children = node->GetChildren();
				const auto count = std::min<std::size_t>(children.capacity(), kTraversalLimit - std::min(visited, kTraversalLimit));
				for (std::size_t i = 0; i < count; ++i) {
					if (auto* child = children[static_cast<std::uint16_t>(i)].get()) {
						stack.push_back(child);
					}
				}
			}
		}
		return false;
	}

	std::size_t StripConflictingArmorNodes(RE::Actor* a_actor, RE::NiNode* a_root)
	{
		if (!a_actor || !a_root) {
			return 0;
		}
		auto* changes = a_actor->GetInventoryChanges(true);
		if (!changes) {
			return 0;
		}
		const auto wornMask = changes->GetWornMask();
		if ((wornMask & 0x802) == 0) {
			return 0;
		}
		std::vector<RE::NiPointer<RE::NiAVObject>> detach;
		const auto& children = a_root->GetChildren();
		const auto count = std::min<std::size_t>(children.capacity(), kTraversalLimit);
		detach.reserve(std::min<std::size_t>(count, 32));
		for (std::size_t i = 0; i < count; ++i) {
			if (auto* child = children[static_cast<std::uint16_t>(i)].get(); child && SubtreeConflictsWithWornMask(child, wornMask)) {
				detach.emplace_back(child);
			}
		}
		for (const auto& child : detach) {
			a_root->DetachChild(child.get());
		}
		return detach.size();
	}

	TransformCopyResult CopySkeletonTransforms(RE::PlayerCharacter* a_player, RE::TESRace* a_sourceRace, RE::SEX a_sex)
	{
		TransformCopyResult result;
		if (!a_player || !a_sourceRace || (a_sex != RE::SEX::kMale && a_sex != RE::SEX::kFemale)) {
			result.warning = "invalid skeleton transform input";
			return result;
		}
		const char* modelPath = a_sourceRace->skeletonModels[a_sex].GetModel();
		if (!modelPath || !*modelPath) {
			result.warning = "donor skeleton path is empty";
			return result;
		}

		RE::NiPointer<RE::NiNode> sourceRoot;
		const RE::BSModelDB::DBTraits::ArgsType args{};
		const auto error = RE::BSModelDB::Demand(modelPath, sourceRoot, args);
		if (error != RE::BSResource::ErrorCode::kNone || !sourceRoot) {
			result.warning = std::format("could not load donor skeleton '{}' (error {})", modelPath, std::to_underlying(error));
			return result;
		}

		std::array<RE::NiAVObject*, 2> targets{ a_player->Get3D(true), a_player->Get3D(false) };
		std::vector<RE::NiAVObject*> stack;
		stack.reserve(256);
		const auto& sourceChildren = sourceRoot->GetChildren();
		for (std::size_t i = 0, count = std::min<std::size_t>(sourceChildren.capacity(), kTraversalLimit); i < count; ++i) {
			if (auto* child = sourceChildren[static_cast<std::uint16_t>(i)].get()) {
				stack.push_back(child);
			}
		}

		std::size_t visited = 0;
		while (!stack.empty() && visited++ < kTraversalLimit) {
			auto* source = stack.back();
			stack.pop_back();
			if (!source->name.empty()) {
				for (std::size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
					if (auto* targetRoot = targets[targetIndex]) {
						if (auto* target = targetRoot->GetObjectByName(source->name)) {
							target->local = source->local;
							if (targetIndex == 0) {
								++result.firstPersonMatches;
							} else {
								++result.thirdPersonMatches;
							}
						}
					}
				}
			}
			if (auto* node = source->AsNode()) {
				const auto& children = node->GetChildren();
				const auto count = std::min<std::size_t>(children.capacity(), kTraversalLimit - std::min(visited, kTraversalLimit));
				for (std::size_t i = 0; i < count; ++i) {
					if (auto* child = children[static_cast<std::uint16_t>(i)].get()) {
						stack.push_back(child);
					}
				}
			}
		}
		result.truncated = !stack.empty();
		RE::NiUpdateData update{ 0.0F, RE::NiUpdateData::Flag::kDirty };
		for (auto* target : targets) {
			if (target) {
				target->UpdateWorldData(std::addressof(update));
			}
		}
		return result;
	}

	void FaceBuilderHook(RE::TESNPC* a_base, RE::Actor* a_actor)
	{
		const bool isPlayer = a_actor == RE::PlayerCharacter::GetSingleton();
		ScopedDonorContext context{ isPlayer };
		g_originalFaceBuilder(context.Enabled() ? context.SourceBase() : a_base, a_actor);
	}

	void BodyRootBuilderHook(RE::TESNPC* a_base, RE::NiPointer<RE::NiNode>* a_root, void* a_processData)
	{
		bool isPlayerBase = false;
		{
			std::scoped_lock lock{ g_stateLock };
			isPlayerBase = g_state.active && a_base == g_state.playerBase;
		}
		ScopedDonorContext context{ isPlayerBase };
		g_originalBodyRootBuilder(context.Enabled() ? context.SourceBase() : a_base, a_root, a_processData);
	}

	void BodyAttachHook(RE::TESNPC* a_base, RE::Actor* a_actor, RE::NiPointer<RE::NiNode>* a_root)
	{
		const bool isPlayer = a_actor == RE::PlayerCharacter::GetSingleton();
		ScopedDonorContext context{ isPlayer };
		if (context.Enabled()) {
			const auto stripped = StripConflictingArmorNodes(a_actor, a_root ? a_root->get() : nullptr);
			if (stripped > 0) {
				SKSE::log::debug("Removed {} conflicting armor root node(s) during player rebuild", stripped);
			}
			ScopedPlayerFormIDSpoof spoof{ a_actor };
			g_originalBodyAttach(context.SourceBase(), a_actor, a_root);
			return;
		}
		g_originalBodyAttach(a_base, a_actor, a_root);
	}

	struct HookSpec
	{
		std::string_view name;
		std::uintptr_t site{ 0 };
		std::uintptr_t expectedTarget{ 0 };
		std::array<std::uint8_t, 5> expectedBytes{};
		std::uintptr_t decodedTarget{ 0 };
		bool chained{ false };
	};

	[[nodiscard]] bool ValidateHookSite(HookSpec& a_hook)
	{
		if (*reinterpret_cast<const std::uint8_t*>(a_hook.site) != 0xE8) {
			SKSE::log::critical("{} hook site {:X} is not a rel32 call", a_hook.name, a_hook.site);
			return false;
		}
		std::int32_t displacement = 0;
		std::memcpy(std::addressof(displacement), reinterpret_cast<const void*>(a_hook.site + 1), sizeof(displacement));
		a_hook.decodedTarget = a_hook.site + 5 + displacement;
		if (std::memcmp(reinterpret_cast<const void*>(a_hook.site), a_hook.expectedBytes.data(), a_hook.expectedBytes.size()) == 0 &&
			a_hook.decodedTarget == a_hook.expectedTarget) {
			return true;
		}

		const auto text = REL::Module::get().segment(REL::Segment::textx);
		const bool targetInsideSkyrimText = a_hook.decodedTarget >= text.address() &&
			a_hook.decodedTarget < text.address() + text.size();
		if (!targetInsideSkyrimText) {
			a_hook.chained = true;
			SKSE::log::warn("{} call site is already detoured outside Skyrim; chaining target {:X}", a_hook.name, a_hook.decodedTarget);
			return true;
		}

		SKSE::log::critical(
			"{} hook mismatch at {:X}: decoded {:X}, expected {:X}",
			a_hook.name,
			a_hook.site,
			a_hook.decodedTarget,
			a_hook.expectedTarget);
		return false;
	}

	[[nodiscard]] bool InstallHooks()
	{
		if (g_hooksInstalled.load()) {
			return true;
		}
		if (REL::Module::get().version() != SKSE::RUNTIME_SSE_1_6_1170) {
			SKSE::log::critical("Unsupported runtime {}; this build requires 1.6.1170.0", REL::Module::get().version().string());
			return false;
		}

		const REL::Relocation<std::uintptr_t> faceWrapper{ REL::ID(19743) };
		const REL::Relocation<std::uintptr_t> update3DModel{ REL::ID(39395) };
		const REL::Relocation<std::uintptr_t> faceBuilder{ REL::ID(24740) };
		const REL::Relocation<std::uintptr_t> bodyRootBuilder{ REL::ID(24731) };
		const REL::Relocation<std::uintptr_t> bodyAttach{ REL::ID(24732) };

		std::array hooks{
			HookSpec{ "face builder", faceWrapper.address() + 0x30, faceBuilder.address(), { 0xE8, 0xBB, 0x57, 0x0D, 0x00 } },
			HookSpec{ "body root builder", update3DModel.address() + 0x20B, bodyRootBuilder.address(), { 0xE8, 0x30, 0x8D, 0xCD, 0xFF } },
			HookSpec{ "body attach", update3DModel.address() + 0x21E, bodyAttach.address(), { 0xE8, 0x5D, 0x8E, 0xCD, 0xFF } }
		};
		for (auto& hook : hooks) {
			if (!ValidateHookSite(hook)) {
				SKSE::log::critical("No appearance hooks were written");
				return false;
			}
		}

		try {
			SKSE::AllocTrampoline(128);
			auto& trampoline = SKSE::GetTrampoline();
			g_originalFaceBuilder = reinterpret_cast<FaceBuilder_t>(
				trampoline.write_call<5>(hooks[0].site, FaceBuilderHook, true));
			g_originalBodyRootBuilder = reinterpret_cast<BodyRootBuilder_t>(
				trampoline.write_call<5>(hooks[1].site, BodyRootBuilderHook, true));
			g_originalBodyAttach = reinterpret_cast<BodyAttach_t>(
				trampoline.write_call<5>(hooks[2].site, BodyAttachHook, true));
		} catch (const std::exception& error) {
			SKSE::log::critical("Failed while writing appearance hooks: {}", error.what());
			return false;
		}

		if (!g_originalFaceBuilder || !g_originalBodyRootBuilder || !g_originalBodyAttach) {
			SKSE::log::critical("A trampoline returned a null original target");
			return false;
		}
		g_hooksInstalled.store(true);
		SKSE::log::info("Installed three verified Skyrim 1.6.1170 appearance call wrappers");
		return true;
	}

	OperationResult ApplyAppearance(
		RE::Actor* a_sourceActor,
		RE::TESNPC* a_sourceBase,
		float a_sourceEffectiveScale,
		std::optional<OriginalMetrics> a_loadedOriginal = std::nullopt)
	{
		std::unique_lock lock{ g_stateLock };
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* playerBase = player ? player->GetActorBase() : nullptr;
		auto* sourceRace = a_sourceBase ? a_sourceBase->race : nullptr;
		if (!player || !playerBase || !a_sourceBase || !sourceRace) {
			return { false, "the player or donor base data is unavailable" };
		}
		if (const auto reason = ValidateCompatibility(player, playerBase, a_sourceActor, a_sourceBase, sourceRace, a_sourceEffectiveScale)) {
			return { false, *reason };
		}

		OriginalMetrics original;
		if (a_loadedOriginal) {
			original = *a_loadedOriginal;
		} else if (g_state.active) {
			if (g_state.playerBase != playerBase) {
				return { false, "the player base changed while an appearance copy was active; revert or reload first" };
			}
			original = g_state.original;
		} else {
			original = { player->GetScale(), playerBase->weight };
		}
		if (!IsFiniteInRange(original.effectiveScale, 0.05F, 20.0F) ||
			!IsFiniteInRange(original.weight, 0.0F, 100.0F)) {
			return { false, "the original player scale/weight snapshot is invalid" };
		}

		g_state = {
			.active = true,
			.playerBase = playerBase,
			.sourceBase = a_sourceBase,
			.sourceRace = sourceRace,
			.sourceEffectiveScale = a_sourceEffectiveScale,
			.original = original
		};

		playerBase->weight = a_sourceBase->weight;
		player->SetScale(a_sourceEffectiveScale / playerBase->GetHeight());
		player->DoReset3D(true);
		const auto transforms = CopySkeletonTransforms(player, sourceRace, a_sourceBase->GetSex());
		if (!transforms.warning.empty()) {
			SKSE::log::warn("Appearance copied, but skeleton transforms were incomplete: {}", transforms.warning);
		}
		if (transforms.truncated) {
			SKSE::log::warn("Donor skeleton traversal reached its {}-node safety limit", kTraversalLimit);
		}

		SKSE::log::info(
			"Applied donor {:08X} ({}) race {:08X}; scale {}, weight {}, skeleton matches 1P={} 3P={}",
			a_sourceBase->GetFormID(),
			FormName(a_sourceBase),
			sourceRace->GetFormID(),
			a_sourceEffectiveScale,
			a_sourceBase->weight,
			transforms.firstPersonMatches,
			transforms.thirdPersonMatches);
		return { true, std::format("copied appearance from {} [{:08X}]", FormName(a_sourceBase), a_sourceBase->GetFormID()) };
	}

	OperationResult RevertAppearance()
	{
		std::unique_lock lock{ g_stateLock };
		if (!g_state.active) {
			return { false, "no copied appearance is active" };
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* playerBase = g_state.playerBase;
		if (!player || !playerBase) {
			g_state = {};
			return { false, "the player is unavailable; state was cleared without a 3D refresh" };
		}
		if (player->IsOnMount()) {
			return { false, "dismount before reverting appearance" };
		}
		const auto original = g_state.original;
		const auto height = playerBase->GetHeight();
		if (!IsFiniteInRange(height, 0.05F, 20.0F)) {
			return { false, "the player base height is invalid; state was left active" };
		}

		g_state = {};  // Disable donor hooks before rebuilding the normal player.
		playerBase->weight = original.weight;
		player->SetScale(original.effectiveScale / height);
		player->DoReset3D(true);
		SKSE::log::info("Reverted copied appearance; restored scale {} and weight {}", original.effectiveScale, original.weight);
		return { true, "restored the player's original appearance, scale, and weight" };
	}

	void RestoreFieldsAndClearStateForTransition()
	{
		std::unique_lock lock{ g_stateLock };
		if (g_state.active && g_state.playerBase) {
			g_state.playerBase->weight = g_state.original.weight;
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				const auto height = g_state.playerBase->GetHeight();
				if (IsFiniteInRange(height, 0.05F, 20.0F) && IsFiniteInRange(g_state.original.effectiveScale, 0.05F, 20.0F)) {
					player->SetScale(g_state.original.effectiveScale / height);
				}
			}
		}
		g_state = {};
		g_pending = {};
	}

	void PrintStatus()
	{
		std::scoped_lock lock{ g_stateLock };
		ConsolePrint(
			"Mirrorborn {}: hooks={}, command={}, active={}",
			kPluginVersion,
			g_hooksInstalled.load() ? "ready" : "disabled",
			g_commandRegistered.load() ? "ready" : "unavailable",
			g_state.active ? "yes" : "no");
		if (g_state.active) {
			ConsolePrint(
				"Donor: {} [{:08X}], race [{:08X}], effective scale {:.3f}",
				FormName(g_state.sourceBase),
				g_state.sourceBase ? g_state.sourceBase->GetFormID() : 0,
				g_state.sourceRace ? g_state.sourceRace->GetFormID() : 0,
				g_state.sourceEffectiveScale);
		}
	}

	void QueueCopy(RE::Actor* a_actor)
	{
		if (!a_actor) {
			ConsolePrint("Mirrorborn: select an NPC in the console first.");
			return;
		}
		const RE::ActorHandle handle = a_actor->GetHandle();
		if (!handle) {
			ConsolePrint("Mirrorborn: the selected actor has no valid reference handle.");
			return;
		}
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask([handle]() {
				auto source = handle.get();
				if (!source) {
					ConsolePrint("Mirrorborn: the selected actor was unloaded before the copy task ran.");
					return;
				}
				if (source.get() == RE::PlayerCharacter::GetSingleton()) {
					const auto result = RevertAppearance();
					ConsolePrint("Mirrorborn: {}", result.message);
					return;
				}
				auto* sourceBase = source->GetActorBase();
				const auto result = ApplyAppearance(source.get(), sourceBase, source->GetScale());
				ConsolePrint("Mirrorborn: {}", result.message);
			});
		} else {
			ConsolePrint("Mirrorborn: the SKSE task interface is unavailable.");
		}
	}

	void QueueRevert()
	{
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask([]() {
				const auto result = RevertAppearance();
				ConsolePrint("Mirrorborn: {}", result.message);
			});
		} else {
			ConsolePrint("Mirrorborn: the SKSE task interface is unavailable.");
		}
	}

	bool CopyCommand(RE::TESObjectREFR* a_ref, const char* a_argument)
	{
		if (!NormalizeArgument(a_argument).empty()) {
			ConsolePrint("Mirrorborn: usage: skee copy");
			return true;
		}

		RE::NiPointer<RE::TESObjectREFR> selected;
		if (a_ref) {
			selected.reset(a_ref);
		} else {
			selected = RE::Console::GetSelectedRef();
		}
		auto* actor = selected ? selected->As<RE::Actor>() : nullptr;
		if (!actor) {
			ConsolePrint("Mirrorborn: select an NPC in the console first.");
			return true;
		}
		QueueCopy(actor);
		return true;
	}

	bool RevertCommand(RE::TESObjectREFR*, const char* a_argument)
	{
		if (!NormalizeArgument(a_argument).empty()) {
			ConsolePrint("Mirrorborn: usage: skee revert");
			return true;
		}
		QueueRevert();
		return true;
	}

	bool StatusCommand(RE::TESObjectREFR*, const char* a_argument)
	{
		if (!NormalizeArgument(a_argument).empty()) {
			ConsolePrint("Mirrorborn: usage: skee status");
			return true;
		}
		PrintStatus();
		return true;
	}

	[[nodiscard]] bool TryRegisterRaceMenuCommand()
	{
		if (g_commandRegistered.load()) {
			return true;
		}
		auto* messaging = SKSE::GetMessagingInterface();
		if (!messaging) {
			return false;
		}
		SKEE::InterfaceExchangeMessage exchange;
		if (!messaging->Dispatch(
				SKEE::InterfaceExchangeMessage::kMessage_ExchangeInterface,
				std::addressof(exchange),
				sizeof(exchange),
				"skee") ||
			!exchange.interfaceMap) {
			SKSE::log::warn("RaceMenu/SKEE interface exchange was unavailable");
			return false;
		}
		auto* command = static_cast<SKEE::ICommandInterface*>(exchange.interfaceMap->QueryInterface("Command"));
		if (!command) {
			SKSE::log::warn("RaceMenu did not expose its Command interface");
			return false;
		}
		const bool copyRegistered = command->RegisterCommand(
			"copy", "Copy the selected NPC's appearance to the player", CopyCommand);
		const bool revertRegistered = command->RegisterCommand(
			"revert", "Restore the player's pre-copy appearance", RevertCommand);
		const bool statusRegistered = command->RegisterCommand(
			"status", "Show Mirrorborn's hook and active-copy state", StatusCommand);
		if (!copyRegistered || !revertRegistered || !statusRegistered) {
			SKSE::log::error(
				"RaceMenu rejected a command registration (copy={}, revert={}, status={})",
				copyRegistered,
				revertRegistered,
				statusRegistered);
			return false;
		}
		g_commandRegistered.store(true);
		SKSE::log::info("Registered RaceMenu console commands: skee copy, skee revert, skee status");
		return true;
	}

	void SaveCallback(SKSE::SerializationInterface* a_intfc)
	{
		std::scoped_lock lock{ g_stateLock };
		if (!g_state.active || !g_state.sourceBase || !g_state.sourceRace) {
			return;
		}
		const SerializedAppearanceV4 record{
			.sourceBaseID = g_state.sourceBase->GetFormID(),
			.sourceRaceID = g_state.sourceRace->GetFormID(),
			.sourceEffectiveScale = g_state.sourceEffectiveScale,
			.originalEffectiveScale = g_state.original.effectiveScale,
			.originalWeight = g_state.original.weight
		};
		if (!a_intfc->WriteRecord(kAppearanceRecord, kAppearanceVersion, record)) {
			SKSE::log::error("Failed to serialize the active appearance copy");
		}
	}

	void SkipRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length)
	{
		std::array<std::byte, 256> discard{};
		std::uint32_t remaining = a_length;
		while (remaining > 0) {
			const auto request = std::min<std::uint32_t>(remaining, static_cast<std::uint32_t>(discard.size()));
			const auto read = a_intfc->ReadRecordData(discard.data(), request);
			if (read == 0) {
				break;
			}
			remaining -= std::min(remaining, read);
		}
	}

	void LoadCallback(SKSE::SerializationInterface* a_intfc)
	{
		PendingState loaded;
		std::uint32_t type = 0;
		std::uint32_t version = 0;
		std::uint32_t length = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type != kAppearanceRecord) {
				SkipRecord(a_intfc, length);
				continue;
			}

			SerializedAppearanceV4 data;
			RE::FormID legacyOriginalRaceID = 0;
			if ((version == 1 || version == 2 || version == 4) && length == sizeof(SerializedAppearanceV4)) {
				if (a_intfc->ReadRecordData(data) != sizeof(data)) {
					SKSE::log::error("Appearance serialization record was truncated");
					continue;
				}
			} else if (version == 3 && length == sizeof(SerializedAppearanceV3)) {
				SerializedAppearanceV3 oldData;
				if (a_intfc->ReadRecordData(oldData) != sizeof(oldData)) {
					SKSE::log::error("Version 3 appearance serialization record was truncated");
					continue;
				}
				data = {
					.sourceBaseID = oldData.sourceBaseID,
					.sourceRaceID = oldData.sourceRaceID,
					.sourceEffectiveScale = oldData.sourceEffectiveScale,
					.originalEffectiveScale = oldData.originalEffectiveScale,
					.originalWeight = oldData.originalWeight
				};
				legacyOriginalRaceID = oldData.originalPlayerRaceID;
			} else {
				SKSE::log::warn("Skipping unsupported appearance record version {} ({} bytes)", version, length);
				SkipRecord(a_intfc, length);
				continue;
			}

			RE::FormID sourceBaseID = 0;
			RE::FormID sourceRaceID = 0;
			if (!data.sourceBaseID || !data.sourceRaceID ||
				!a_intfc->ResolveFormID(data.sourceBaseID, sourceBaseID) ||
				!a_intfc->ResolveFormID(data.sourceRaceID, sourceRaceID)) {
				SKSE::log::warn("Saved appearance donor forms could not be resolved");
				continue;
			}
			auto* sourceBase = RE::TESForm::LookupByID<RE::TESNPC>(sourceBaseID);
			auto* sourceRace = RE::TESForm::LookupByID<RE::TESRace>(sourceRaceID);
			if (!sourceBase || !sourceRace || sourceBase->race != sourceRace || sourceBase->IsDynamicForm() || sourceRace->IsDynamicForm() ||
				!IsFiniteInRange(data.sourceEffectiveScale, 0.05F, 20.0F) ||
				!IsFiniteInRange(data.originalEffectiveScale, 0.05F, 20.0F) ||
				!IsFiniteInRange(data.originalWeight, 0.0F, 100.0F)) {
				SKSE::log::warn("Saved appearance data failed structural validation");
				continue;
			}

			RE::TESRace* legacyOriginalRace = nullptr;
			if (legacyOriginalRaceID != 0) {
				RE::FormID resolvedOriginalRace = 0;
				if (!a_intfc->ResolveFormID(legacyOriginalRaceID, resolvedOriginalRace) ||
					!(legacyOriginalRace = RE::TESForm::LookupByID<RE::TESRace>(resolvedOriginalRace))) {
					SKSE::log::warn("Version 3 original player race could not be resolved; rejecting migration record");
					continue;
				}
			}

			loaded = {
				.present = true,
				.sourceBase = sourceBase,
				.sourceRace = sourceRace,
				.legacyOriginalRace = legacyOriginalRace,
				.sourceEffectiveScale = data.sourceEffectiveScale,
				.original = { data.originalEffectiveScale, data.originalWeight }
			};
		}

		std::scoped_lock lock{ g_stateLock };
		g_pending = loaded;
		if (loaded.present) {
			SKSE::log::info("Loaded pending appearance donor {:08X}; reapply deferred until PostLoadGame", loaded.sourceBase->GetFormID());
		}
	}

	void RevertCallback(SKSE::SerializationInterface*)
	{
		RestoreFieldsAndClearStateForTransition();
	}

	void ReapplyPendingAppearance()
	{
		PendingState pending;
		{
			std::scoped_lock lock{ g_stateLock };
			pending = g_pending;
			g_pending = {};
		}
		if (!pending.present) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* playerBase = player ? player->GetActorBase() : nullptr;
		if (playerBase && pending.legacyOriginalRace && playerBase->race == pending.sourceRace) {
			playerBase->race = pending.legacyOriginalRace;
			SKSE::log::info("Migrated version 3 save state by restoring original player race {:08X}", pending.legacyOriginalRace->GetFormID());
		}
		const auto result = ApplyAppearance(nullptr, pending.sourceBase, pending.sourceEffectiveScale, pending.original);
		if (!result.success) {
			SKSE::log::error("Could not reapply saved appearance: {}", result.message);
		}
	}

	void QueuePendingReapply()
	{
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask(ReapplyPendingAppearance);
		} else {
			SKSE::log::error("Cannot queue saved appearance reapply: SKSE task interface unavailable");
		}
	}

	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}
		switch (a_message->type) {
		case SKSE::MessagingInterface::kPostPostLoad:
			(void)TryRegisterRaceMenuCommand();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			(void)InstallHooks();
			(void)TryRegisterRaceMenuCommand();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			RestoreFieldsAndClearStateForTransition();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			QueuePendingReapply();
			break;
		default:
			break;
		}
	}
}

SKSEPluginInfo(
	.Version = "1.0.0.0"_v,
	.Name = "Mirrorborn",
	.Author = "Mirrorborn project",
	.StructCompatibility = SKSE::StructCompatibility::Dependent,
	.RuntimeCompatibility = {
		SKSE::RUNTIME_SSE_1_6_1170,
		REL::Version{}, REL::Version{}, REL::Version{}, REL::Version{}, REL::Version{},
		REL::Version{}, REL::Version{}, REL::Version{}, REL::Version{}, REL::Version{},
		REL::Version{}, REL::Version{}, REL::Version{}, REL::Version{}, REL::Version{} })

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	spdlog::set_level(spdlog::level::info);
	spdlog::flush_on(spdlog::level::warn);

	if (a_skse->RuntimeVersion() != SKSE::RUNTIME_SSE_1_6_1170) {
		SKSE::log::critical("Mirrorborn 1.0.0 only supports Skyrim 1.6.1170.0");
		return false;
	}
	const auto* serialization = SKSE::GetSerializationInterface();
	const auto* messaging = SKSE::GetMessagingInterface();
	const auto* tasks = SKSE::GetTaskInterface();
	if (!serialization || !messaging || !tasks) {
		SKSE::log::critical("A required SKSE interface is unavailable");
		return false;
	}

	serialization->SetUniqueID(CCA::kSerializationID);
	serialization->SetSaveCallback(CCA::SaveCallback);
	serialization->SetLoadCallback(CCA::LoadCallback);
	serialization->SetRevertCallback(CCA::RevertCallback);
	if (!messaging->RegisterListener("SKSE", CCA::OnSKSEMessage)) {
		SKSE::log::critical("Could not register the SKSE lifecycle listener");
		return false;
	}

	SKSE::log::info(
		"Mirrorborn {} loaded; runtime {}, SKSE {:08X}",
		CCA::kPluginVersion,
		a_skse->RuntimeVersion().string(),
		a_skse->SKSEVersion());
	return true;
}
