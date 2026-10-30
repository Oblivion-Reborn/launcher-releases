#include <Windows.h>
#undef ERROR

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <numbers>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	constexpr const char* kCommandName = "RebornNative";
	constexpr const char* kCommandShort = "rnative";
	constexpr std::string_view kSpareCommands[] = { "TestCode", "TestSeenData", "TestLocalMap", "TestAllCells", "TestAllCellsQuick" };

	using Counts = std::map<std::uint32_t, std::int32_t>;

	std::uint32_t g_openRef = 0;

	std::filesystem::path LinkDir()
	{
		wchar_t* base = nullptr;
		std::size_t len = 0;
		if (_wdupenv_s(&base, &len, L"LOCALAPPDATA") != 0 || !base) {
			return {};
		}
		std::filesystem::path dir = std::filesystem::path(base) / L"OblivionReborn" / L"link";
		std::free(base);
		return dir;
	}

	bool g_selfPump = false;
	bool g_questGuard = false;
	bool g_unlockNow = false;

	std::string Hex(std::uint32_t a_value)
	{
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%08X", a_value);
		return buf;
	}

	std::uint32_t ParseHex(const std::string& a_text)
	{
		return static_cast<std::uint32_t>(std::stoul(a_text, nullptr, 16));
	}

	bool Shared(std::uint32_t a_form)
	{
		return (a_form >> 24) != 0xFF;
	}

	void WriteAtomic(const std::filesystem::path& a_file, const std::string& a_text)
	{
		const auto tmp = std::filesystem::path(a_file).concat(L".tmp");
		{
			std::ofstream out(tmp, std::ios::trunc);
			out << a_text;
		}
		std::error_code ec;
		std::filesystem::rename(tmp, a_file, ec);
	}

	void Note(const std::string& a_text)
	{
		std::ofstream out(LinkDir() / L"native-events.txt", std::ios::app);
		out << a_text << '\n';
	}

	RE::TESObjectREFR* RefFor(const std::string& a_text)
	{
		if (a_text == "player") {
			return RE::PlayerCharacter::GetSingleton();
		}
		auto* form = RE::TESForm::LookupByID(ParseHex(a_text));
		return form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr;
	}

	RE::TESBoundObject* ItemFor(std::uint32_t a_form)
	{
		auto* form = RE::TESForm::LookupByID(a_form);
		return form ? RE::oblivion_cast<RE::TESBoundObject*>(form) : nullptr;
	}

	template <class R, class... A>
	R ActorSlot(RE::Actor* a_actor, std::size_t a_slot, A... a_args)
	{
		using func_t = R (*)(RE::Actor*, A...);
		return reinterpret_cast<func_t>((*reinterpret_cast<std::uintptr_t**>(a_actor))[a_slot])(a_actor, a_args...);
	}

	float ValueOf(RE::Actor* a_actor, std::int32_t a_index)
	{
		return ActorSlot<float>(a_actor, 0xB0, static_cast<RE::ActorValue::Index>(a_index));
	}

	void SetValue(RE::Actor* a_actor, std::int32_t a_index, float a_value)
	{
		ActorSlot<void>(a_actor, 0xB1, static_cast<RE::ActorValue::Index>(a_index), a_value);
	}

	void ScriptMod(RE::Actor* a_actor, std::int32_t a_index, float a_value)
	{
		ActorSlot<void>(a_actor, 0xB5, static_cast<RE::ActorValue::Index>(a_index), a_value, static_cast<RE::Actor*>(nullptr));
	}

	std::int32_t BaseValue(RE::PlayerCharacter* a_player, std::int32_t a_index)
	{
		const float base = ValueOf(a_player, a_index) - a_player->magicActorValueModifiers[a_index] - a_player->scriptActorValueModifiers[a_index] - a_player->gameActorValueModifiers[a_index];
		return static_cast<std::int32_t>(std::lround(base));
	}

	RE::TESActorBaseData* PlayerBaseData(RE::PlayerCharacter* a_player)
	{
		auto* base = a_player ? a_player->data.objectReference : nullptr;
		auto* npc = base ? RE::oblivion_cast<RE::TESNPC*>(static_cast<RE::TESForm*>(base)) : nullptr;
		return npc ? static_cast<RE::TESActorBaseData*>(npc) : nullptr;
	}

	struct ContainerEntry
	{
		std::int32_t count;
		RE::TESForm* form;
	};

	RE::TESContainer* BaseContainer(RE::TESObjectREFR* a_ref)
	{
		auto* base = a_ref->data.objectReference;
		if (!base) {
			return nullptr;
		}
		auto* form = static_cast<RE::TESForm*>(base);
		if (auto* cont = RE::oblivion_cast<RE::TESObjectCONT*>(form)) {
			return static_cast<RE::TESContainer*>(cont);
		}
		if (auto* npc = RE::oblivion_cast<RE::TESNPC*>(form)) {
			return static_cast<RE::TESContainer*>(npc);
		}
		return nullptr;
	}

	bool Worn(RE::ItemChange* a_change)
	{
		if (!a_change || !a_change->extraData) {
			return false;
		}
		for (auto* list : *a_change->extraData) {
			for (auto* extra = list ? list->head : nullptr; extra; extra = extra->next) {
				const auto type = extra->type.get();
				if (type == RE::EXTRA_DATA_TYPE::Worn || type == RE::EXTRA_DATA_TYPE::WornLeft) {
					return true;
				}
			}
		}
		return false;
	}

	Counts Totals(RE::TESObjectREFR* a_ref)
	{
		Counts out;
		if (auto* cont = BaseContainer(a_ref)) {
			for (auto* raw : cont->objectList) {
				auto* entry = reinterpret_cast<ContainerEntry*>(raw);
				if (entry && entry->form && entry->form->GetFormType() != RE::FormType::LeveledItem) {
					out[entry->form->GetFormID()] += entry->count;
				}
			}
		}
		auto* extra = a_ref->extra.GetExtraData<RE::ExtraContainerChanges>();
		if (extra && extra->changes && extra->changes->list) {
			for (auto* change : *extra->changes->list) {
				if (change && change->object) {
					out[change->object->GetFormID()] += change->count;
				}
			}
		}
		return out;
	}

	std::string CountsText(const Counts& a_counts)
	{
		std::ostringstream out;
		bool first = true;
		for (const auto& [form, count] : a_counts) {
			if (count > 0 && Shared(form)) {
				out << (first ? "" : ",") << Hex(form) << ':' << count;
				first = false;
			}
		}
		return out.str();
	}

	bool g_quietRemove = false;
	bool g_quietAdd = false;

	void Change(RE::TESObjectREFR* a_ref, std::uint32_t a_form, std::int32_t a_by)
	{
		auto* object = ItemFor(a_form);
		if (!object || a_by == 0) {
			return;
		}
		if (a_by > 0) {
			g_quietAdd = true;
			a_ref->AddObjectToContainer(object, nullptr, a_by);
			g_quietAdd = false;
		} else {
			g_quietRemove = true;
			a_ref->RemoveItem(object, nullptr, -a_by, false, false, nullptr, nullptr, nullptr, true, false);
			g_quietRemove = false;
		}
	}

	std::size_t SetExact(RE::TESObjectREFR* a_ref, const Counts& a_want)
	{
		const auto have = Totals(a_ref);
		std::size_t changed = 0;
		for (const auto& [form, count] : have) {
			if (!Shared(form) || count <= 0) {
				continue;
			}
			const auto it = a_want.find(form);
			const auto want = it == a_want.end() ? 0 : it->second;
			if (want != count) {
				Change(a_ref, form, want - count);
				++changed;
			}
		}
		for (const auto& [form, count] : a_want) {
			const auto it = have.find(form);
			if ((it == have.end() || it->second <= 0) && count > 0 && Shared(form)) {
				Change(a_ref, form, count);
				++changed;
			}
		}
		return changed;
	}

	struct Box
	{
		std::string state;
		bool known = false;
		bool found = false;
		Counts items;
	};

	struct Policy
	{
		bool empty = false;
		bool roll = false;
		std::map<std::uint32_t, bool> strip;
	};

	Policy ReadPolicy()
	{
		Policy p;
		std::ifstream in(LinkDir() / L"boxes" / L"_default.txt");
		if (!in) {
			return p;
		}
		std::string mode;
		std::getline(in, mode);
		if (!mode.empty() && mode.back() == '\r') {
			mode.pop_back();
		}
		p.empty = mode == "empty";
		p.roll = mode == "roll";
		for (std::string line; std::getline(in, line);) {
			if (line.rfind("strip ", 0) != 0) {
				continue;
			}
			std::istringstream list(line.substr(6));
			for (std::string part; std::getline(list, part, ',');) {
				while (!part.empty() && (part.back() == '\r' || part.back() == ' ')) {
					part.pop_back();
				}
				if (part.size() == 8) {
					try {
						p.strip[ParseHex(part)] = true;
					} catch (const std::exception&) {
					}
				}
			}
		}
		return p;
	}

	Box ReadBox(std::uint32_t a_ref)
	{
		Box box;
		std::ifstream in(LinkDir() / L"boxes" / (Hex(a_ref) + ".txt"));
		if (!in) {
			return box;
		}
		box.found = true;
		std::getline(in, box.state);
		for (std::string line; std::getline(in, line);) {
			std::istringstream row(line);
			std::string word, form;
			std::int32_t count = 0;
			row >> word;
			if (word == "items") {
				box.known = true;
			} else if (word == "item" && (row >> form >> count) && count > 0) {
				try {
					box.items[ParseHex(form)] = count;
				} catch (const std::exception&) {
				}
			}
		}
		return box;
	}

	void FinishOpen()
	{
		if (!g_openRef) {
			return;
		}
		const auto ref = g_openRef;
		g_openRef = 0;
		std::error_code ec;
		std::filesystem::remove(LinkDir() / L"box-open.txt", ec);
		auto* form = RE::TESForm::LookupByID(ref);
		auto* refr = form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr;
		if (!refr) {
			Note("lost " + Hex(ref));
			return;
		}
		Note("closed " + Hex(ref) + ' ' + CountsText(Totals(refr)));
		REX::INFO("container {} closed", Hex(ref));
	}

	void CheckClosed()
	{
		if (g_openRef && !RE::Tile::GetMenuByClass(RE::MENU_CLASS::kContainerMenu)) {
			FinishOpen();
		}
	}

	bool g_openStrips = false;

	struct RollWait
	{
		std::uint32_t ref = 0;
		RE::TESObjectCONT* base = nullptr;
		bool idFlag = false;
		RE::TESBoundObject* object = nullptr;
		std::int32_t count = 0;
		std::uint64_t at = 0;
	};
	RollWait g_rollWait;
	std::map<std::uint32_t, bool> g_rollGaveUp;
	constexpr std::uint64_t kRollWaitMs = 4000;

	using Activate_t = bool (*)(RE::TESObjectCONT*, RE::TESObjectREFR*, RE::TESObjectREFR*, bool, RE::TESBoundObject*, std::int32_t);
	Activate_t g_containerActivate = nullptr;

	bool ContainerActivate(RE::TESObjectCONT* a_self, RE::TESObjectREFR* a_target, RE::TESObjectREFR* a_activator, bool a_idFlag, RE::TESBoundObject* a_object, std::int32_t a_count)
	{
		const bool byPlayer = a_activator && a_activator == RE::PlayerCharacter::GetSingleton();
		if (!byPlayer || !a_target) {
			return g_containerActivate(a_self, a_target, a_activator, a_idFlag, a_object, a_count);
		}
		const auto ref = a_target->GetFormID();
		if (g_openRef && g_openRef != ref) {
			FinishOpen();
		}
		auto box = ReadBox(ref);
		Policy policy;
		if (!box.found) {
			policy = ReadPolicy();
			if (policy.empty) {
				box.known = true;
			}
		}
		if (!box.found && policy.roll && !g_rollGaveUp.count(ref)) {
			if (g_rollWait.ref != ref) {
				g_rollWait = { ref, a_self, a_idFlag, a_object, a_count, GetTickCount64() };
				Note("roll " + Hex(ref) + ' ' + Hex(a_self ? a_self->GetFormID() : 0));
				REX::INFO("container {} asks the server for this run's loot", Hex(ref));
			}
			return false;
		}
		if (box.state == "busy") {
			REX::INFO("container {} is in use, kept shut", Hex(ref));
			WriteAtomic(LinkDir() / L"box-busy.txt", Hex(ref) + '\n');
			Note("busy " + Hex(ref));
			return false;
		}
		std::size_t before = 0;
		if (box.known) {
			before = SetExact(a_target, box.items);
		}
		const bool result = g_containerActivate(a_self, a_target, a_activator, a_idFlag, a_object, a_count);
		std::size_t after = 0;
		if (box.known) {
			after = SetExact(a_target, box.items);
		} else if (!policy.strip.empty()) {
			for (const auto& [form, count] : Totals(a_target)) {
				if (count > 0 && policy.strip.count(form)) {
					Change(a_target, form, -count);
					++after;
				}
			}
		}
		REX::INFO("container {} opened, {} changed before and {} after", Hex(ref), before, after);
		g_openRef = ref;
		g_openStrips = !box.found && !policy.empty;
		WriteAtomic(LinkDir() / L"box-open.txt", Hex(ref) + '\n');
		Note("open " + Hex(ref));
		return result;
	}

	void CheckRoll()
	{
		if (!g_rollWait.ref) {
			return;
		}
		const bool ready = ReadBox(g_rollWait.ref).found;
		const bool late = GetTickCount64() - g_rollWait.at > kRollWaitMs;
		if (!ready && !late) {
			return;
		}
		const auto wait = g_rollWait;
		g_rollWait = {};
		if (!ready) {
			g_rollGaveUp[wait.ref] = true;
			REX::INFO("container {} got no loot from the server in time, opening it with the game's loot", Hex(wait.ref));
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* form = RE::TESForm::LookupByID(wait.ref);
		auto* target = form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr;
		if (!player || !target || !wait.base || player->IsDead(false)) {
			return;
		}
		ContainerActivate(wait.base, target, player, wait.idFlag, wait.object, wait.count);
	}

	void HookContainers()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::TESObjectCONT[0] };
		g_containerActivate = reinterpret_cast<Activate_t>(vtbl.write_vfunc(0x33, &ContainerActivate));
		REX::INFO("container activation hooked");
	}

	bool g_live = false;
	std::string g_lastInv;
	std::uint64_t g_lastInvCheck = 0;

	Counts ParseCounts(const std::string& a_text)
	{
		Counts out;
		std::istringstream in(a_text);
		for (std::string part; std::getline(in, part, ',');) {
			const auto colon = part.find(':');
			if (colon == std::string::npos) {
				continue;
			}
			try {
				const auto count = std::stoi(part.substr(colon + 1));
				if (count > 0) {
					out[ParseHex(part.substr(0, colon))] = count;
				}
			} catch (const std::exception&) {
			}
		}
		return out;
	}

	std::string PlayerText(RE::TESObjectREFR* a_player)
	{
		std::map<std::uint32_t, bool> worn;
		auto* extra = a_player->extra.GetExtraData<RE::ExtraContainerChanges>();
		if (extra && extra->changes && extra->changes->list) {
			for (auto* change : *extra->changes->list) {
				if (change && change->object && Worn(change)) {
					worn[change->object->GetFormID()] = true;
				}
			}
		}
		std::ostringstream out;
		for (const auto& [form, count] : Totals(a_player)) {
			if (count > 0 && Shared(form)) {
				out << "item " << Hex(form) << ' ' << count << ' ' << (worn.count(form) ? 1 : 0) << '\n';
			}
		}
		return out.str();
	}

	const std::map<std::uint32_t, std::array<std::uint32_t, 5>> kFilled{
		{ 0x23D67, { 0x382D1, 0, 0, 0, 0 } },
		{ 0x23D69, { 0x382D2, 0x382D3, 0, 0, 0 } },
		{ 0x15B8B, { 0x382D4, 0x382D5, 0x382D6, 0, 0 } },
		{ 0x15B8C, { 0x382D7, 0x382D8, 0x382D9, 0x382DA, 0 } },
		{ 0x15B8E, { 0x382DB, 0x382DC, 0x382DD, 0x382DE, 0x382DF } },
		{ 0x00192, { 0, 0, 0, 0, 0x382E0 } },
		{ 0x1EC14, { 0, 0, 0, 0, 0x382E0 } }
	};

	void FillGems(RE::PlayerCharacter* a_player)
	{
		auto* extra = a_player->extra.GetExtraData<RE::ExtraContainerChanges>();
		if (!extra || !extra->changes || !extra->changes->list) {
			return;
		}
		struct Swap
		{
			RE::TESBoundObject* from;
			RE::ExtraDataList*  list;
			std::int32_t        count;
			std::uint32_t       to;
			std::uint8_t        soul;
		};
		std::vector<Swap> swaps;
		for (auto* change : *extra->changes->list) {
			if (!change || !change->object || !change->extraData) {
				continue;
			}
			const auto it = kFilled.find(change->object->GetFormID());
			if (it == kFilled.end()) {
				continue;
			}
			for (auto* list : *change->extraData) {
				std::uint8_t soul = 0;
				std::int32_t count = 1;
				for (auto* x = list ? list->head : nullptr; x; x = x->next) {
					const auto type = x->type.get();
					if (type == RE::EXTRA_DATA_TYPE::Soul) {
						soul = *reinterpret_cast<const std::uint8_t*>(reinterpret_cast<const char*>(x) + 0x18);
					} else if (type == RE::EXTRA_DATA_TYPE::Count) {
						count = *reinterpret_cast<const std::int16_t*>(reinterpret_cast<const char*>(x) + 0x18);
					}
				}
				if (soul < 1 || soul > 5 || count < 1 || count > 1000) {
					continue;
				}
				const auto to = it->second[soul - 1];
				if (to) {
					swaps.push_back({ change->object, list, count, to, soul });
				}
			}
		}
		for (const auto& sw : swaps) {
			auto* filled = ItemFor(sw.to);
			if (!filled) {
				continue;
			}
			g_quietRemove = true;
			a_player->RemoveItem(sw.from, sw.list, sw.count, false, false, nullptr, nullptr, nullptr, true, false);
			g_quietRemove = false;
			g_quietAdd = true;
			a_player->AddObjectToContainer(filled, nullptr, sw.count);
			g_quietAdd = false;
			REX::INFO("soul gems: {} {} holding a level {} soul became {}", sw.count, Hex(sw.from->GetFormID()), sw.soul, Hex(sw.to));
		}
	}

	void WatchPlayer()
	{
		if (!g_live) {
			return;
		}
		const auto now = GetTickCount64();
		if (now - g_lastInvCheck < 500) {
			return;
		}
		g_lastInvCheck = now;
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		if (!player->IsDead(false)) {
			FillGems(player);
		}
		auto text = PlayerText(player);
		if (text.empty() || text == g_lastInv) {
			return;
		}
		g_lastInv = text;
		WriteAtomic(LinkDir() / L"inv.txt", text);
	}

	std::map<std::string, int> g_hookSeen;

	struct Drop
	{
		std::uint32_t ref;
		std::uint32_t object;
		std::uint64_t at;
	};
	constexpr std::size_t kDropsKept = 32;
	constexpr std::uint64_t kDeathDropMs = 20000;
	std::vector<Drop> g_drops;

	bool g_placing = false;
	bool g_silentPick = false;
	std::map<std::uint32_t, std::uint32_t> g_dropByRef;
	std::map<std::uint32_t, std::uint32_t> g_refByDrop;
	std::map<std::uint32_t, RE::NiPoint3> g_dropAt;
	constexpr float kPickupReach = 600.0f;

	std::string Coords(const RE::NiPoint3& a_at)
	{
		char buf[96];
		std::snprintf(buf, sizeof(buf), "%.1f %.1f %.1f", a_at.x, a_at.y, a_at.z);
		return buf;
	}

	bool PlayerDead()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		return player && player->IsDead(false);
	}

	void Changed(const char* a_what)
	{
		if (++g_hookSeen[a_what] <= 5) {
			REX::INFO("player inventory changed by {}", a_what);
		}
		if (!PlayerDead()) {
			WatchPlayer();
		}
	}

	using AddBound_t = void (*)(RE::PlayerCharacter*, RE::TESBoundObject*, RE::ExtraDataList*, std::int32_t);
	using Remove_t = RE::TESObjectREFR* (*)(RE::PlayerCharacter*, RE::TESBoundObject*, RE::ExtraDataList*, std::int32_t, bool, bool, RE::TESObjectREFR*, RE::NiPoint3*, RE::NiPoint3*, bool, bool);
	using AddWorn_t = bool (*)(RE::PlayerCharacter*, RE::TESBoundObject*, std::int32_t, RE::ExtraDataList*, bool);
	using RemoveWorn_t = bool (*)(RE::PlayerCharacter*, RE::TESBoundObject*, std::int32_t, RE::ExtraDataList*);
	AddBound_t g_addBound = nullptr;
	Remove_t g_remove = nullptr;
	AddWorn_t g_addWorn = nullptr;
	RemoveWorn_t g_removeWorn = nullptr;
	AddBound_t g_addToContainer = nullptr;

	std::vector<std::pair<std::uint32_t, std::int32_t>> g_takeBack;

	struct Grab
	{
		std::uint32_t form;
		std::int32_t number;
		std::uint64_t at;
	};
	std::vector<Grab> g_grabs;

	bool LooseKind(RE::FormType a_type)
	{
		switch (a_type) {
		case RE::FormType::Apparatus:
		case RE::FormType::Armor:
		case RE::FormType::Book:
		case RE::FormType::Clothing:
		case RE::FormType::Ingredient:
		case RE::FormType::Misc:
		case RE::FormType::Weapon:
		case RE::FormType::Ammo:
		case RE::FormType::SoulGem:
		case RE::FormType::KeyMaster:
		case RE::FormType::AlchemyItem:
		case RE::FormType::SigilStone:
			return true;
		default:
			return false;
		}
	}

	void PlayerAddBound(RE::PlayerCharacter* a_self, RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra, std::int32_t a_number)
	{
		g_addBound(a_self, a_object, a_extra, a_number);
		if (!g_quietAdd && !g_placing && a_object && a_number > 0 && (g_openRef == 0 || g_openStrips) && RE::Tile::GetMenuByClass(RE::MENU_CLASS::kContainerMenu)) {
			g_takeBack.emplace_back(a_object->GetFormID(), a_number);
		}
		if (!g_quietAdd && !g_placing && a_object && a_number > 0 && Shared(a_object->GetFormID()) && LooseKind(a_object->GetFormType()) && g_grabs.size() < 16) {
			g_grabs.push_back({ a_object->GetFormID(), a_number, GetTickCount64() });
		}
		Changed("add");
	}

	void TakeBack()
	{
		if (g_takeBack.empty()) {
			return;
		}
		auto list = std::move(g_takeBack);
		g_takeBack.clear();
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || PlayerDead()) {
			return;
		}
		const auto policy = ReadPolicy();
		if (policy.strip.empty()) {
			return;
		}
		const auto have = Totals(player);
		for (const auto& [form, number] : list) {
			if (!policy.strip.count(form)) {
				continue;
			}
			const auto it = have.find(form);
			const auto held = it == have.end() ? 0 : it->second;
			const auto take = (std::min)(number, held);
			if (take > 0) {
				Change(player, form, -take);
				Note("deny blocked " + Hex(form) + ' ' + std::to_string(take));
				REX::INFO("took back {} of the blocked item {}, the rest they carry stays", take, Hex(form));
			}
		}
	}

	RE::TESObjectREFR* PlayerRemove(RE::PlayerCharacter* a_self, RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra, std::int32_t a_number, bool a_stealing, bool a_dropWorld, RE::TESObjectREFR* a_container, RE::NiPoint3* a_point, RE::NiPoint3* a_rotate, bool a_delete, bool a_preferStolen)
	{
		if (a_dropWorld && !a_container && PlayerDead()) {
			REX::INFO("kept {} that dying would have dropped", a_object ? Hex(a_object->GetFormID()) : std::string("-"));
			return nullptr;
		}
		auto* result = g_remove(a_self, a_object, a_extra, a_number, a_stealing, a_dropWorld, a_container, a_point, a_rotate, a_delete, a_preferStolen);
		if (!a_dropWorld && !a_container && !g_quietRemove && a_object && !PlayerDead()) {
			const auto type = a_object->GetFormType();
			const bool food = type == RE::FormType::Ingredient || type == RE::FormType::AlchemyItem;
			const bool busy = RE::Tile::GetMenuByClass(RE::MENU_CLASS::kAlchemyMenu) || RE::Tile::GetMenuByClass(RE::MENU_CLASS::kContainerMenu);
			if (food && !busy) {
				const char* name = RE::TESFullName::GetFullName(a_object);
				std::string clean = name ? name : "";
				for (auto& ch : clean) {
					if (ch == '\n' || ch == '\r') {
						ch = ' ';
					}
				}
				Note("eat " + Hex(a_object->GetFormID()) + ' ' + std::to_string(a_number) + (clean.empty() ? "" : " " + clean));
			}
			static int toldFood = 0;
			if ((food || type == RE::FormType::Misc) && ++toldFood <= 20) {
				REX::INFO("food check: {} left the player, type {}, counted as eaten {}, menu busy {}", Hex(a_object->GetFormID()), static_cast<int>(type), food && !busy, busy);
			}
		} else if (a_object && !PlayerDead()) {
			static int toldOther = 0;
			if (++toldOther <= 20) {
				REX::INFO("food check: {} left the player but was not counted, to world {}, to a container {}, quiet {}", Hex(a_object->GetFormID()), a_dropWorld, a_container != nullptr, g_quietRemove);
			}
		}
		if (a_dropWorld && result && a_object) {
			g_drops.push_back({ result->GetFormID(), a_object->GetFormID(), GetTickCount64() });
			if (g_drops.size() > kDropsKept) {
				g_drops.erase(g_drops.begin());
			}
			if (!g_placing && !PlayerDead() && Shared(a_object->GetFormID())) {
				const auto id = result->GetFormID();
				g_dropByRef[id] = 0;
				g_dropAt[id] = result->data.location;
				Note("drop " + Hex(id) + ' ' + Hex(a_object->GetFormID()) + ' ' + std::to_string(a_number) + ' ' + Coords(result->data.location));
			}
		}
		Changed("remove");
		return result;
	}

	bool PlayerAddWorn(RE::PlayerCharacter* a_self, RE::TESBoundObject* a_object, std::int32_t a_number, RE::ExtraDataList* a_extra, bool a_force)
	{
		const bool result = g_addWorn(a_self, a_object, a_number, a_extra, a_force);
		Changed("wear");
		return result;
	}

	bool PlayerRemoveWorn(RE::PlayerCharacter* a_self, RE::TESBoundObject* a_object, std::int32_t a_number, RE::ExtraDataList* a_extra)
	{
		const bool result = g_removeWorn(a_self, a_object, a_number, a_extra);
		Changed("take off");
		return result;
	}

	void PlayerAddToContainer(RE::PlayerCharacter* a_self, RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra, std::int32_t a_number)
	{
		g_addToContainer(a_self, a_object, a_extra, a_number);
		Changed("add to container");
	}

	using HealthDamage_t = void (*)(RE::PlayerCharacter*, RE::Actor*, float);
	HealthDamage_t g_healthDamage = nullptr;
	bool g_deathNoted = false;
	std::map<std::uint32_t, bool> g_deathWorn;
	constexpr float kNoReload = 1000000.0f;

	std::map<std::uint32_t, bool> WornNow(RE::TESObjectREFR* a_ref)
	{
		std::map<std::uint32_t, bool> out;
		auto* extra = a_ref->extra.GetExtraData<RE::ExtraContainerChanges>();
		if (extra && extra->changes && extra->changes->list) {
			for (auto* change : *extra->changes->list) {
				if (change && change->object && Worn(change)) {
					out[change->object->GetFormID()] = true;
				}
			}
		}
		return out;
	}

	void PlayerHealthDamage(RE::PlayerCharacter* a_self, RE::Actor* a_attacker, float a_damage)
	{
		g_healthDamage(a_self, a_attacker, a_damage);
		if (!g_deathNoted && a_self && a_self->IsDead(false)) {
			g_deathNoted = true;
			g_deathWorn = WornNow(a_self);
			WriteAtomic(LinkDir() / L"died.txt", "dead\n");
			REX::INFO("the player died");
		}
	}

	void TuneDeath()
	{
		auto* settings = RE::GameSettingCollection::GetSingleton();
		if (!settings) {
			return;
		}
		for (auto& entry : settings->settings) {
			auto* setting = entry.second;
			const char* name = setting ? setting->name : nullptr;
			if (!name) {
				continue;
			}
			std::string lower(name);
			for (auto& c : lower) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			if (lower.find("death") == std::string::npos) {
				continue;
			}
			if (lower == "fplayerdeathreloadtime") {
				setting->data.f = kNoReload;
				REX::INFO("game setting {} raised so the load screen never opens after death", name);
			} else if (lower == "ideathdropweaponchance") {
				setting->data.i = 0;
				REX::INFO("game setting {} set to 0 so dying never drops your weapon", name);
			} else {
				REX::INFO("game setting {} left alone", name);
			}
		}
	}

	std::map<std::uint32_t, float> g_wear;

	using DamageEquipment_t = bool (*)(RE::PlayerCharacter*, RE::ItemChange*, float, bool);
	DamageEquipment_t g_damageEquipment = nullptr;

	bool PlayerDamageEquipment(RE::PlayerCharacter* a_self, RE::ItemChange* a_item, float a_damage, bool a_ignoreSkill)
	{
		if (a_item && a_item->object && !g_wear.empty()) {
			const auto it = g_wear.find(a_item->object->GetFormID());
			if (it != g_wear.end()) {
				a_damage *= it->second;
			}
		}
		return g_damageEquipment(a_self, a_item, a_damage, a_ignoreSkill);
	}

	void HookPlayer()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::PlayerCharacter[0] };
		g_addBound = reinterpret_cast<AddBound_t>(vtbl.write_vfunc(0x45, &PlayerAddBound));
		g_remove = reinterpret_cast<Remove_t>(vtbl.write_vfunc(0x46, &PlayerRemove));
		g_addWorn = reinterpret_cast<AddWorn_t>(vtbl.write_vfunc(0x48, &PlayerAddWorn));
		g_removeWorn = reinterpret_cast<RemoveWorn_t>(vtbl.write_vfunc(0x49, &PlayerRemoveWorn));
		g_addToContainer = reinterpret_cast<AddBound_t>(vtbl.write_vfunc(0x4B, &PlayerAddToContainer));
		g_healthDamage = reinterpret_cast<HealthDamage_t>(vtbl.write_vfunc(0x111, &PlayerHealthDamage));
		g_damageEquipment = reinterpret_cast<DamageEquipment_t>(vtbl.write_vfunc(0xC1, &PlayerDamageEquipment));
		REX::INFO("player inventory changes hooked, gear wear can be slowed by perks");
	}

	constexpr std::uint64_t kDespawnMs = 30000;
	constexpr std::uint64_t kSweepEveryMs = 5000;
	std::map<std::uint32_t, std::uint64_t> g_lyingSince;
	std::uint64_t g_lastSweep = 0;

	bool OurBodyId(std::uint32_t a_id);

	bool DeadBody(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_player)
	{
		if (a_ref == a_player) {
			return false;
		}
		auto* actor = RE::oblivion_cast<RE::Actor*>(static_cast<RE::TESForm*>(a_ref));
		return actor && actor->IsDead(false);
	}

	void Sweep()
	{
		const auto now = GetTickCount64();
		if (now - g_lastSweep < kSweepEveryMs) {
			return;
		}
		g_lastSweep = now;
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->parentCell : nullptr;
		if (!cell || PlayerDead()) {
			return;
		}
		const bool looting = RE::Tile::GetMenuByClass(RE::MENU_CLASS::kContainerMenu) != nullptr;
		std::map<std::uint32_t, std::uint64_t> still;
		std::vector<RE::TESObjectREFR*> expired;
		bool havePolicy = false;
		Policy policy;
		for (auto* ref : cell->listReferences) {
			if (!ref || ref->IsDeleted() || (ref->GetFormFlags() & RE::TESForm::RecordFlags::kDisabled)) {
				continue;
			}
			if (!DeadBody(ref, player) || OurBodyId(ref->GetFormID())) {
				continue;
			}
			const auto id = ref->GetFormID();
			const auto it = g_lyingSince.find(id);
			if (it == g_lyingSince.end()) {
				if (!havePolicy) {
					policy = ReadPolicy();
					havePolicy = true;
				}
				std::size_t stripped = 0;
				for (const auto& [form, count] : Totals(ref)) {
					if (count > 0 && policy.strip.count(form)) {
						Change(ref, form, -count);
						++stripped;
					}
				}
				if (stripped) {
					REX::INFO("took {} blocked items off the body {}", stripped, Hex(id));
				}
			}
			const auto since = it == g_lyingSince.end() ? now : it->second;
			if (!looting && now - since >= kDespawnMs) {
				expired.push_back(ref);
			} else {
				still[id] = since;
			}
		}
		g_lyingSince = still;
		for (auto* ref : expired) {
			REX::INFO("despawned {} after it lay there 30 s", Hex(ref->GetFormID()));
			ref->SetDelete(true);
		}
	}

	void WatchDrops()
	{
		if (g_dropByRef.empty()) {
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || PlayerDead()) {
			return;
		}
		std::vector<std::uint32_t> gone;
		for (const auto& [ref, net] : g_dropByRef) {
			auto* form = RE::TESForm::LookupByID(ref);
			auto* refr = form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr;
			const bool vanished = !refr || refr->IsDeleted() || (refr->GetFormFlags() & RE::TESForm::RecordFlags::kDisabled) || !refr->parentCell;
			if (!vanished) {
				g_dropAt[ref] = refr->data.location;
				continue;
			}
			gone.push_back(ref);
			const auto at = g_dropAt.count(ref) ? g_dropAt[ref] : RE::NiPoint3{};
			const auto me = player->data.location;
			const float dx = at.x - me.x, dy = at.y - me.y, dz = at.z - me.z;
			const bool close = dx * dx + dy * dy + dz * dz <= kPickupReach * kPickupReach;
			if (!close) {
				continue;
			}
			if (net) {
				Note("taken " + std::to_string(net));
			} else {
				Note("takenref " + Hex(ref));
			}
		}
		for (const auto ref : gone) {
			const auto net = g_dropByRef[ref];
			g_dropByRef.erase(ref);
			g_dropAt.erase(ref);
			if (net) {
				g_refByDrop.erase(net);
			}
		}
	}

	struct Job
	{
		std::string id;
		bool hunter = false;
		std::set<std::uint32_t> animals;
		std::set<std::uint32_t> denyFlora;
		std::set<std::uint32_t> denyRefs;
		std::map<std::uint32_t, std::string> nodes;
		std::vector<std::array<std::uint32_t, 3>> packs;
		std::set<std::uint32_t> perks;
		std::set<std::uint32_t> perkAll;
		std::map<std::uint32_t, float> wear;
		bool perksSaid = false;
	};

	Job g_job;
	std::filesystem::file_time_type g_jobStamp{};
	std::uint64_t g_jobChecked = 0;
	std::set<std::uint32_t> g_skinned;

	std::set<std::uint32_t> ParseList(const std::string& a_text)
	{
		std::set<std::uint32_t> out;
		std::istringstream list(a_text);
		for (std::string part; std::getline(list, part, ',');) {
			while (!part.empty() && (part.back() == '\r' || part.back() == ' ')) {
				part.pop_back();
			}
			if (part.size() == 8) {
				try {
					out.insert(ParseHex(part));
				} catch (const std::exception&) {
				}
			}
		}
		return out;
	}

	void LoadJob()
	{
		const auto now = GetTickCount64();
		if (now - g_jobChecked < 1500) {
			return;
		}
		g_jobChecked = now;
		const auto file = LinkDir() / L"job.txt";
		std::error_code ec;
		const auto stamp = std::filesystem::last_write_time(file, ec);
		if (ec || stamp == g_jobStamp) {
			return;
		}
		std::ifstream in(file);
		if (!in) {
			return;
		}
		g_jobStamp = stamp;
		Job job;
		for (std::string line; std::getline(in, line);) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			const auto space = line.find(' ');
			const auto key = line.substr(0, space);
			const auto rest = space == std::string::npos ? std::string() : line.substr(space + 1);
			if (key == "job") {
				job.id = rest;
			} else if (key == "hunter") {
				job.hunter = rest == "1";
			} else if (key == "animals") {
				job.animals = ParseList(rest);
			} else if (key == "denyflora") {
				job.denyFlora = ParseList(rest);
			} else if (key == "perks") {
				job.perks = ParseList(rest);
				job.perksSaid = true;
			} else if (key == "wear") {
				std::istringstream list(rest);
				for (std::string part; std::getline(list, part, ',');) {
					const auto colon = part.find(':');
					if (colon == std::string::npos) {
						continue;
					}
					try {
						const float mult = std::stof(part.substr(colon + 1));
						if (mult > 0.0f && mult <= 1.0f) {
							job.wear[ParseHex(part.substr(0, colon))] = mult;
						}
					} catch (const std::exception&) {
					}
				}
			} else if (key == "perkall") {
				job.perkAll = ParseList(rest);
			} else if (key == "denyref") {
				job.denyRefs = ParseList(rest);
			} else if (key == "pack") {
				std::istringstream cols(rest);
				std::string form, local;
				std::uint32_t bonus = 0;
				if (cols >> form >> local >> bonus) {
					try {
						job.packs.push_back({ ParseHex(form), ParseHex(local), bonus });
					} catch (const std::exception&) {
					}
				}
			} else if (key == "node") {
				const auto gap = rest.find(' ');
				if (gap == std::string::npos) {
					continue;
				}
				const auto kind = rest.substr(0, gap);
				for (const auto id : ParseList(rest.substr(gap + 1))) {
					job.nodes[id] = kind;
				}
			}
		}
		g_job = job;
		g_wear = job.wear;
		REX::INFO("profession file loaded: {} ({} nodes)", g_job.id.empty() ? "none" : g_job.id, g_job.nodes.size());
	}

	using FloraActivate_t = bool (*)(RE::TESFlora*, RE::TESObjectREFR*, RE::TESObjectREFR*, bool, RE::TESBoundObject*, std::int32_t);
	FloraActivate_t g_floraActivate = nullptr;

	bool FloraActivate(RE::TESFlora* a_self, RE::TESObjectREFR* a_target, RE::TESObjectREFR* a_activator, bool a_idFlag, RE::TESBoundObject* a_object, std::int32_t a_count)
	{
		const bool byPlayer = a_target && a_activator == RE::PlayerCharacter::GetSingleton();
		if (byPlayer && !g_silentPick && a_self) {
			LoadJob();
			if (g_job.denyRefs.count(a_target->GetFormID())) {
				Note("deny farm " + Hex(a_target->GetFormID()));
				return false;
			}
			if (g_job.denyFlora.count(a_self->GetFormID())) {
				Note("deny flora " + Hex(a_target->GetFormID()));
				return false;
			}
		}
		const bool result = g_floraActivate(a_self, a_target, a_activator, a_idFlag, a_object, a_count);
		if (!g_silentPick && result && byPlayer) {
			Note("pick " + Hex(a_target->GetFormID()) + ' ' + Hex(a_self ? a_self->GetFormID() : 0));
		}
		return result;
	}

	using CreatureActivate_t = bool (*)(RE::TESCreature*, RE::TESObjectREFR*, RE::TESObjectREFR*, bool, RE::TESBoundObject*, std::int32_t);
	CreatureActivate_t g_creatureActivate = nullptr;

	bool CreatureActivate(RE::TESCreature* a_self, RE::TESObjectREFR* a_target, RE::TESObjectREFR* a_activator, bool a_idFlag, RE::TESBoundObject* a_object, std::int32_t a_count)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		const bool looting = a_self && a_target && a_activator == player && DeadBody(a_target, player);
		bool animal = false;
		if (looting) {
			LoadJob();
			animal = g_job.animals.count(a_self->GetFormID()) > 0;
			if (animal && !g_job.hunter) {
				Note("deny loot " + Hex(a_target->GetFormID()));
				return false;
			}
		}
		const bool result = g_creatureActivate(a_self, a_target, a_activator, a_idFlag, a_object, a_count);
		if (looting && animal && result && g_skinned.insert(a_target->GetFormID()).second) {
			Note("skin " + Hex(a_target->GetFormID()) + ' ' + Hex(a_self->GetFormID()));
		}
		return result;
	}

	void HookCreatures()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::TESCreature[0] };
		g_creatureActivate = reinterpret_cast<CreatureActivate_t>(vtbl.write_vfunc(0x33, &CreatureActivate));
		REX::INFO("creature activation hooked");
	}

	struct HealthExtra : RE::BSExtraData
	{
		float health;
	};

	float MaxHealth(RE::TESBoundObject* a_object)
	{
		if (!a_object) {
			return 0;
		}
		const auto type = a_object->GetFormType();
		RE::TESHealthForm* form = nullptr;
		if (type == RE::FormType::Weapon) {
			form = static_cast<RE::TESObjectWEAP*>(static_cast<RE::TESForm*>(a_object));
		} else if (type == RE::FormType::Armor) {
			form = static_cast<RE::TESObjectARMO*>(static_cast<RE::TESForm*>(a_object));
		}
		return form ? static_cast<float>(form->health) : 0.0f;
	}

	HealthExtra* HealthOf(RE::ExtraDataList* a_list)
	{
		if (!a_list) {
			return nullptr;
		}
		return static_cast<HealthExtra*>(a_list->GetExtraData(RE::EXTRA_DATA_TYPE::Health));
	}

	void RunGear(std::ostringstream& a_out)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* extra = player ? player->extra.GetExtraData<RE::ExtraContainerChanges>() : nullptr;
		if (!extra || !extra->changes || !extra->changes->list) {
			a_out << "gear end\n";
			return;
		}
		for (auto* change : *extra->changes->list) {
			if (!change || !change->object) {
				continue;
			}
			const float max = MaxHealth(change->object);
			if (max <= 0) {
				continue;
			}
			float low = max;
			if (change->extraData) {
				for (auto* list : *change->extraData) {
					if (auto* h = HealthOf(list)) {
						low = (std::min)(low, h->health);
					}
				}
			}
			a_out << "gear " << Hex(change->object->GetFormID()) << ' ' << static_cast<int>(low * 100.0f / max + 0.5f) << '\n';
		}
		a_out << "gear end\n";
	}

	void RunRepair(std::ostringstream& a_out, std::uint32_t a_form, int a_pct)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* extra = player ? player->extra.GetExtraData<RE::ExtraContainerChanges>() : nullptr;
		if (!extra || !extra->changes || !extra->changes->list) {
			return;
		}
		int fixed = 0;
		for (auto* change : *extra->changes->list) {
			if (!change || !change->object || change->object->GetFormID() != a_form || !change->extraData) {
				continue;
			}
			const float max = MaxHealth(change->object);
			for (auto* list : *change->extraData) {
				if (auto* h = HealthOf(list)) {
					h->health = max * static_cast<float>(a_pct) / 100.0f;
					++fixed;
				}
			}
		}
		a_out << "repaired " << Hex(a_form) << ' ' << fixed << '\n';
		Changed("repair");
	}

	RE::TESSpellList* PlayerSpells()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* base = player ? player->data.objectReference : nullptr;
		auto* npc = base ? RE::oblivion_cast<RE::TESNPC*>(static_cast<RE::TESForm*>(base)) : nullptr;
		return npc ? static_cast<RE::TESSpellList*>(npc) : nullptr;
	}

	void RunDiseases(std::ostringstream& a_out)
	{
		if (auto* list = PlayerSpells()) {
			for (auto* spell : list->spells) {
				if (spell && spell->data.spellType == RE::MagicSystem::SpellType::kDisease) {
					const char* name = RE::TESFullName::GetFullName(spell);
					a_out << "disease " << Hex(spell->GetFormID()) << ' ' << (name ? name : "Disease") << '\n';
				}
			}
		}
		a_out << "diseases end\n";
	}

	void RunCure(std::ostringstream& a_out, std::uint32_t a_form)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* list = PlayerSpells();
		if (!player || !list) {
			return;
		}
		RE::SpellItem* hit = nullptr;
		for (auto* spell : list->spells) {
			if (spell && spell->GetFormID() == a_form && spell->data.spellType == RE::MagicSystem::SpellType::kDisease) {
				hit = spell;
			}
		}
		if (hit) {
			player->RemoveSpell(hit);
			a_out << "cured " << Hex(a_form) << '\n';
		}
	}

	std::uint32_t g_ridingRef = 0;

	void WatchRide()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		auto* horse = player->GetHorse();
		const std::uint32_t now = horse ? horse->GetFormID() : 0;
		if (now != g_ridingRef) {
			g_ridingRef = now;
			Note("ride " + Hex(now));
		}
	}

	RE::Actor* ActorByRef(const std::string& a_text)
	{
		if (a_text.empty()) {
			return nullptr;
		}
		try {
			auto* form = RE::TESForm::LookupByID(ParseHex(a_text));
			return form ? RE::oblivion_cast<RE::Actor*>(form) : nullptr;
		} catch (const std::exception&) {
			return nullptr;
		}
	}

	std::map<int, bool> g_guarded;

	void Guard(RE::MENU_CLASS a_class, bool a_allowed, const char* a_what)
	{
		const int key = static_cast<int>(a_class);
		auto* tile = RE::Tile::GetMenuByClass(a_class);
		if (!tile) {
			g_guarded.erase(key);
			return;
		}
		if (a_allowed) {
			return;
		}
		auto* menu = static_cast<RE::TileMenu*>(tile)->menu;
		if (menu) {
			menu->StartFadeOut();
		}
		if (!g_guarded[key]) {
			g_guarded[key] = true;
			Note(std::string("deny ") + a_what);
			REX::INFO("closed the {} menu", a_what);
		}
	}

	void GuardMenus()
	{
		LoadJob();
		Guard(RE::MENU_CLASS::kSleepWaitMenu, false, "rest");
		Guard(RE::MENU_CLASS::kAlchemyMenu, g_job.id == "herbalist", "alchemy");
		Guard(RE::MENU_CLASS::kEnchantmentMenu, false, "enchant");
		Guard(RE::MENU_CLASS::kRepairMenu, g_job.id == "smith", "repair");
	}

	void RunNear(std::ostringstream& a_out, const std::string& a_kinds, float a_reach)
	{
		LoadJob();
		std::set<std::string> want;
		std::istringstream list(a_kinds);
		for (std::string k; std::getline(list, k, ',');) {
			want.insert(k);
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->parentCell : nullptr;
		if (!cell) {
			a_out << "near none\n";
			return;
		}
		const auto at = player->data.location;
		RE::TESObjectREFR* best = nullptr;
		std::string bestKind;
		float bestD = a_reach * a_reach;
		for (auto* ref : cell->listReferences) {
			if (!ref || ref == player || ref->IsDeleted() || (ref->GetFormFlags() & RE::TESForm::RecordFlags::kDisabled) || !ref->data.objectReference) {
				continue;
			}
			const auto it = g_job.nodes.find(ref->data.objectReference->GetFormID());
			if (it == g_job.nodes.end() || !want.count(it->second)) {
				continue;
			}
			const float dx = ref->data.location.x - at.x;
			const float dy = ref->data.location.y - at.y;
			const float dz = ref->data.location.z - at.z;
			const float d = dx * dx + dy * dy + dz * dz;
			if (d < bestD) {
				bestD = d;
				best = ref;
				bestKind = it->second;
			}
		}
		if (!best) {
			a_out << "near none\n";
			return;
		}
		a_out << "near " << bestKind << ' ' << Hex(best->GetFormID()) << ' ' << Coords(best->data.location) << '\n';
	}

	void HookFlora()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::TESFlora[0] };
		g_floraActivate = reinterpret_cast<FloraActivate_t>(vtbl.write_vfunc(0x33, &FloraActivate));
		REX::INFO("flora activation hooked");
	}

	void ReportInventory(std::ostringstream& a_out, RE::TESObjectREFR* a_ref)
	{
		const auto totals = Totals(a_ref);
		for (const auto& [form, count] : totals) {
			a_out << "item " << Hex(form) << ' ' << count << '\n';
		}
		a_out << "items " << totals.size() << '\n';
	}

	void ReportBase(std::ostringstream& a_out, RE::TESObjectREFR* a_ref)
	{
		auto* cont = BaseContainer(a_ref);
		if (!cont) {
			a_out << "base none\n";
			return;
		}
		for (auto* raw : cont->objectList) {
			auto* entry = reinterpret_cast<ContainerEntry*>(raw);
			if (entry && entry->form) {
				a_out << "base " << Hex(entry->form->GetFormID()) << ' ' << entry->count << ' ' << static_cast<int>(entry->form->GetFormType()) << '\n';
			}
		}
		auto* extra = a_ref->extra.GetExtraData<RE::ExtraContainerChanges>();
		if (extra && extra->changes && extra->changes->list) {
			for (auto* change : *extra->changes->list) {
				if (change && change->object) {
					a_out << "change " << Hex(change->object->GetFormID()) << ' ' << change->count << '\n';
				}
			}
		}
	}

	void ReportState(std::ostringstream& a_out)
	{
		auto* ui = RE::InterfaceManager::GetInstance(false, false);
		if (!ui) {
			a_out << "state none\n";
			return;
		}
		auto id = [](RE::TESObjectREFR* a_ref) { return a_ref ? Hex(a_ref->GetFormID()) : std::string("-"); };
		a_out << "state mode " << static_cast<int>(ui->menuMode) << " crosshair " << id(ui->crosshairRef)
			  << " containermenu " << (RE::Tile::GetMenuByClass(RE::MENU_CLASS::kContainerMenu) ? 1 : 0) << " open " << (g_openRef ? Hex(g_openRef) : std::string("-")) << '\n';
	}

	struct ScriptStub
	{
		alignas(16) std::uint8_t script[0x88]{};
		alignas(16) std::uint8_t locals[0x80]{};

		ScriptStub()
		{
			*reinterpret_cast<std::uint32_t*>(script + 0x0C) = 0x8;
		}
	};

	class Args
	{
	public:
		Args& Axis(char a_axis)
		{
			_types.push_back(RE::SCRIPT_PARAM_TYPE::kAxis);
			_bytes.push_back(static_cast<std::uint8_t>(a_axis));
			return *this;
		}

		Args& Number(RE::SCRIPT_PARAM_TYPE a_type, double a_value)
		{
			_types.push_back(a_type);
			_bytes.push_back('z');
			Put(a_value);
			return *this;
		}

		Args& Group(std::int16_t a_group)
		{
			_types.push_back(RE::SCRIPT_PARAM_TYPE::kAnimGroup);
			Put(a_group);
			return *this;
		}

		std::vector<std::uint8_t> Compiled() const
		{
			std::vector<std::uint8_t> out(2);
			const auto n = static_cast<std::uint16_t>(_types.size());
			std::memcpy(out.data(), &n, sizeof(n));
			out.insert(out.end(), _bytes.begin(), _bytes.end());
			return out;
		}

		const std::vector<RE::SCRIPT_PARAM_TYPE>& Types() const { return _types; }

	private:
		template <class T>
		void Put(T a_value)
		{
			const auto* raw = reinterpret_cast<const std::uint8_t*>(&a_value);
			_bytes.insert(_bytes.end(), raw, raw + sizeof(T));
		}

		std::vector<RE::SCRIPT_PARAM_TYPE> _types;
		std::vector<std::uint8_t>          _bytes;
	};

	std::atomic<bool> g_inputTick = false;
	std::atomic<bool> g_bodiesBroken = false;
	bool g_bodyJob = false;

	int ExecGuarded(RE::SCRIPT_FUNCTION::Execute_t* a_fn, const RE::SCRIPT_PARAMETER* a_params, const char* a_data, RE::TESObjectREFR* a_ref, void* a_script, void* a_locals, double* a_result)
	{
		__try {
			std::uint32_t offset = 0;
			return a_fn(a_params, a_data, a_ref, a_ref, static_cast<RE::Script*>(a_script), static_cast<RE::ScriptLocals*>(a_locals), *a_result, offset) ? 1 : 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return -1;
		}
	}

	bool RunCommand(const char* a_name, RE::TESObjectREFR* a_ref, const Args& a_args, double* a_result = nullptr)
	{
		auto* command = RE::SCRIPT_FUNCTION::LocateScriptCommand(a_name);
		const auto& types = a_args.Types();
		if (!a_ref || !command || !command->executeFunction || command->paramCount < types.size()) {
			REX::WARN("bodies: the game command {} is not there on this build", a_name);
			return false;
		}
		for (std::size_t i = 0; i < types.size(); ++i) {
			if (command->parameters[i].paramType != types[i]) {
				REX::WARN("bodies: the game command {} takes other arguments on this build", a_name);
				return false;
			}
		}
		static ScriptStub stub;
		const auto data = a_args.Compiled();
		double result = 0;
		const int rc = ExecGuarded(command->executeFunction, command->parameters, reinterpret_cast<const char*>(data.data()), a_ref, stub.script, stub.locals, &result);
		if (rc < 0) {
			if (g_bodyJob) {
				g_bodiesBroken = true;
				REX::ERROR("bodies: the game command {} faulted on {}, native bodies are off for this session", a_name, Hex(a_ref->GetFormID()));
			} else {
				REX::ERROR("the game command {} faulted on {}, skipped it", a_name, Hex(a_ref->GetFormID()));
			}
			return false;
		}
		if (a_result) {
			*a_result = result;
		}
		return rc == 1;
	}

	using CreateRef_t = RE::TESObjectREFR* (*)(RE::TESDataHandler*, RE::TESBoundObject*, RE::NiPoint3*, RE::NiPoint3*, RE::TESObjectCELL*, RE::TESWorldSpace*, RE::TESObjectREFR*);

	RE::TESObjectREFR* CreateGuarded(CreateRef_t a_fn, RE::TESDataHandler* a_dh, RE::TESBoundObject* a_base, RE::NiPoint3* a_at, RE::NiPoint3* a_turn, RE::TESObjectCELL* a_cell, RE::TESWorldSpace* a_world, bool* a_fault)
	{
		__try {
			return a_fn(a_dh, a_base, a_at, a_turn, a_cell, a_world, nullptr);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			*a_fault = true;
			return nullptr;
		}
	}

	struct Body
	{
		std::string           base;
		RE::TESObjectCELL*    cell = nullptr;
		bool                  parked = false;
		bool                  ai = false;
		std::uint64_t         parkedAt = 0;
		std::uint64_t         calmedAt = 0;
	};

	constexpr std::size_t   kBodiesParked = 4;
	constexpr std::uint64_t kPuppetRecalmMs = 2000;
	constexpr std::size_t   kPuppetsPerTick = 4;
	constexpr std::size_t   kBodyStepsPerTick = 24;
	constexpr std::uint64_t kSpawnGapMs = 250;
	std::map<std::uint32_t, Body> g_bodies;
	std::mutex               g_bodyLock;
	std::vector<std::string> g_bodyJobs;
	std::string              g_bodyOut;

	bool OurBodyId(std::uint32_t a_id)
	{
		return g_bodies.count(a_id) != 0;
	}
	std::uint64_t            g_lastSpawn = 0;

	bool BodyVerb(const std::string& a_verb)
	{
		return a_verb == "body" || a_verb == "adopt" || a_verb == "unbody" || a_verb == "move" || a_verb == "face" || a_verb == "sneak" || a_verb == "group" || a_verb == "glide" || a_verb == "mode";
	}

	struct Glide
	{
		float         x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0, deg = 0;
		float         sx = 0, sy = 0, sz = 0, sdeg = 0;
		std::uint64_t at = 0;
		std::uint64_t drawn = 0;
		bool          shown = false;
		bool          still = false;
	};

	constexpr std::uint64_t kGlideFrameMs = 15;
	constexpr float         kGlideAhead = 0.4f;
	constexpr float         kGlideLead = 0.12f;
	constexpr float         kGlideCatch = 14.0f;
	constexpr float         kGlideSnap = 700.0f;
	std::map<std::uint32_t, Glide> g_glides;
	std::uint64_t                  g_glideDrawn = 0;

	void SetGlide(const std::string& a_line)
	{
		std::istringstream in(a_line);
		std::string verb, refText, deg;
		float x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0;
		in >> verb >> refText;
		if (!(in >> x >> y >> z)) {
			return;
		}
		in >> deg >> vx >> vy >> vz;
		std::uint32_t id = 0;
		try {
			id = ParseHex(refText);
		} catch (const std::exception&) {
			return;
		}
		auto& g = g_glides[id];
		g.x = x;
		g.y = y;
		g.z = z;
		g.vx = vx;
		g.vy = vy;
		g.vz = vz;
		if (!deg.empty() && deg != "-") {
			try {
				g.deg = std::stof(deg);
			} catch (const std::exception&) {
			}
		}
		g.at = GetTickCount64();
		g.still = false;
	}

	constexpr std::int32_t kProcessScan = 0x800;
	std::int32_t                            g_moveOffset = -2;
	std::map<std::int32_t, int>             g_moveHits;
	std::uint16_t                           g_lastSeenMode = 0xFFFF;
	int                                     g_modeChanges = 0;
	std::set<std::uint16_t>                 g_modesSeen;
	std::map<std::uint32_t, std::uint16_t>  g_bodyMode;

	bool ReadU16(const std::uint8_t* a_at, std::uint16_t& a_out)
	{
		__try {
			a_out = *reinterpret_cast<const std::uint16_t*>(a_at);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool WriteU16(std::uint8_t* a_at, std::uint16_t a_value)
	{
		__try {
			*reinterpret_cast<std::uint16_t*>(a_at) = a_value;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	void LoadMoveOffset()
	{
		g_moveOffset = -1;
		std::ifstream in(LinkDir() / L"move-offset.txt");
		std::int32_t off = -1;
		if (in >> std::hex >> off && off > 0 && off < kProcessScan) {
			g_moveOffset = off;
			REX::INFO("bodies: movement flags sit at process +{:X} (learned before)", off);
		}
	}

	void LearnMoveOffset()
	{
		if (g_moveOffset == -2) {
			LoadMoveOffset();
		}
		if (g_moveOffset >= 0) {
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->currentProcess) {
			return;
		}
		const std::uint16_t mode = player->lastAnimState.moveMode;
		if (mode == g_lastSeenMode) {
			return;
		}
		g_lastSeenMode = mode;
		const auto* base = reinterpret_cast<const std::uint8_t*>(player->currentProcess);
		std::vector<std::int32_t> hits;
		for (std::int32_t off = 8; off < kProcessScan; off += 2) {
			std::uint16_t v = 0;
			if (!ReadU16(base + off, v)) {
				break;
			}
			if (v == mode) {
				hits.push_back(off);
			}
		}
		if (g_modeChanges == 0) {
			g_moveHits.clear();
			for (const auto off : hits) {
				g_moveHits[off] = 1;
			}
		} else {
			std::map<std::int32_t, int> next;
			for (const auto off : hits) {
				if (g_moveHits.count(off)) {
					next[off] = g_moveHits[off] + 1;
				}
			}
			g_moveHits.swap(next);
		}
		++g_modeChanges;
		g_modesSeen.insert(mode);
		if (g_moveHits.empty()) {
			g_modeChanges = 0;
			g_modesSeen.clear();
			return;
		}
		if (g_modesSeen.size() >= 3 && g_modeChanges >= 5 && g_moveHits.size() <= 2) {
			g_moveOffset = g_moveHits.begin()->first;
			std::ofstream out(LinkDir() / L"move-offset.txt", std::ios::trunc);
			out << std::hex << g_moveOffset << "\n";
			REX::INFO("bodies: learned where the game keeps movement flags: process +{:X} ({} candidates, {} changes watched)", g_moveOffset, g_moveHits.size(), g_modeChanges);
		}
	}

	void ApplyModes()
	{
		LearnMoveOffset();
		if (g_moveOffset < 0 || g_bodiesBroken) {
			return;
		}
		std::vector<std::pair<std::uint32_t, std::uint16_t>> work;
		{
			std::lock_guard lock(g_bodyLock);
			work.assign(g_bodyMode.begin(), g_bodyMode.end());
		}
		static int told = 0;
		for (const auto& [id, mode] : work) {
			if (!g_bodies.count(id) || g_bodies[id].parked) {
				continue;
			}
			auto* form = RE::TESForm::LookupByID(id);
			auto* actor = form ? RE::oblivion_cast<RE::Actor*>(form) : nullptr;
			if (!actor || actor->IsDeleted() || !actor->currentProcess) {
				continue;
			}
			auto* at = reinterpret_cast<std::uint8_t*>(actor->currentProcess) + g_moveOffset;
			if (WriteU16(at, mode) && told < 3) {
				++told;
				REX::INFO("bodies: {} now moves with flags {:04X}, the game reports {:04X}", Hex(id), mode, actor->lastAnimState.moveMode);
			}
		}
	}

	void QueueBody(const std::string& a_verb, const std::string& a_line)
	{
		std::lock_guard lock(g_bodyLock);
		if (a_verb == "glide") {
			SetGlide(a_line);
			return;
		}
		if (a_verb == "mode") {
			std::istringstream in(a_line);
			std::string verb, refText;
			unsigned int mode = 0;
			in >> verb >> refText >> mode;
			try {
				g_bodyMode[ParseHex(refText)] = static_cast<std::uint16_t>(mode & 0xFFFF);
			} catch (const std::exception&) {
			}
			return;
		}
		if (a_verb == "move" || a_verb == "face") {
			std::istringstream in(a_line);
			std::string verb, ref;
			in >> verb >> ref;
			for (auto& job : g_bodyJobs) {
				std::istringstream old(job);
				std::string oldVerb, oldRef;
				old >> oldVerb >> oldRef;
				if (oldVerb == a_verb && oldRef == ref) {
					job = a_line;
					return;
				}
			}
		}
		g_bodyJobs.push_back(a_line);
	}

	RE::TESObjectREFR* OurBody(const std::string& a_text)
	{
		std::uint32_t id = 0;
		try {
			id = ParseHex(a_text);
		} catch (const std::exception&) {
			return nullptr;
		}
		if (!g_bodies.count(id)) {
			return nullptr;
		}
		auto* form = RE::TESForm::LookupByID(id);
		auto* ref = form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr;
		if (!ref || ref->IsDeleted()) {
			g_bodies.erase(id);
			return nullptr;
		}
		return ref;
	}

	using PersonActivate_t = bool (*)(RE::TESNPC*, RE::TESObjectREFR*, RE::TESObjectREFR*, bool, RE::TESBoundObject*, std::int32_t);
	PersonActivate_t g_personActivate = nullptr;

	bool PersonActivate(RE::TESNPC* a_self, RE::TESObjectREFR* a_target, RE::TESObjectREFR* a_activator, bool a_idFlag, RE::TESBoundObject* a_object, std::int32_t a_count)
	{
		if (a_target && a_activator == RE::PlayerCharacter::GetSingleton() && g_bodies.count(a_target->GetFormID())) {
			static int told = 0;
			if (++told <= 3) {
				REX::INFO("bodies: blocked talking to {}, it is another player", Hex(a_target->GetFormID()));
			}
			return false;
		}
		return g_personActivate(a_self, a_target, a_activator, a_idFlag, a_object, a_count);
	}

	void HookPeople()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::TESNPC[0] };
		g_personActivate = reinterpret_cast<PersonActivate_t>(vtbl.write_vfunc(0x33, &PersonActivate));
		REX::INFO("person activation hooked, other players' bodies cannot be talked to");
	}

	void PlaceBody(RE::TESObjectREFR* a_ref, float a_x, float a_y, float a_z, const std::string& a_deg)
	{
		RunCommand("SetPos", a_ref, Args().Axis('X').Number(RE::SCRIPT_PARAM_TYPE::kFloat, a_x));
		RunCommand("SetPos", a_ref, Args().Axis('Y').Number(RE::SCRIPT_PARAM_TYPE::kFloat, a_y));
		RunCommand("SetPos", a_ref, Args().Axis('Z').Number(RE::SCRIPT_PARAM_TYPE::kFloat, a_z));
		if (!a_deg.empty() && a_deg != "-") {
			RunCommand("SetAngle", a_ref, Args().Axis('Z').Number(RE::SCRIPT_PARAM_TYPE::kFloat, std::stod(a_deg)));
		}
	}

	void Calm(RE::TESObjectREFR* a_ref, bool a_ai)
	{
		if (a_ai) {
			return;
		}
		RunCommand("SetActorsAI", a_ref, Args().Number(RE::SCRIPT_PARAM_TYPE::kInt, 0));
		if (auto* actor = RE::oblivion_cast<RE::Actor*>(static_cast<RE::TESForm*>(a_ref))) {
			SetValue(actor, 33, 0.0f);
		}
	}

	void Pacify(RE::TESObjectREFR* a_ref)
	{
		static const bool canStop = RE::SCRIPT_FUNCTION::LocateScriptCommand("StopCombat") != nullptr;
		if (canStop) {
			RunCommand("StopCombat", a_ref, Args());
		}
		if (auto* actor = RE::oblivion_cast<RE::Actor*>(static_cast<RE::TESForm*>(a_ref))) {
			SetValue(actor, 33, 0.0f);
		}
	}

	RE::TESObjectREFR* Reuse(const std::string& a_base, RE::TESObjectCELL* a_cell)
	{
		for (auto& [id, body] : g_bodies) {
			if (!body.parked || body.ai || body.base != a_base || body.cell != a_cell) {
				continue;
			}
			auto* form = RE::TESForm::LookupByID(id);
			auto* ref = form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr;
			if (ref && !ref->IsDeleted() && !ref->IsDead(false)) {
				body.parked = false;
				return ref;
			}
		}
		return nullptr;
	}

	void SpawnBody(std::istringstream& a_in)
	{
		std::string net, base, deg;
		float x = 0, y = 0, z = 0;
		int ai = 0;
		a_in >> net >> base >> x >> y >> z >> deg >> ai;
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->parentCell : nullptr;
		if (net.empty() || base.empty() || !cell) {
			g_bodyOut += "error body " + net + '\n';
			return;
		}
		if (auto* again = ai ? nullptr : Reuse(base, cell)) {
			PlaceBody(again, x, y, z, deg);
			RunCommand("Enable", again, Args());
			Calm(again, false);
			g_bodyOut += "body " + net + ' ' + Hex(again->GetFormID()) + " reused\n";
			REX::INFO("bodies: reused {} for {}", Hex(again->GetFormID()), net);
			return;
		}
		RE::TESObjectREFR* made = nullptr;
		if (base == "copy") {
			double result = 0;
			if (RunCommand("CreateFullActorCopy", player, Args(), &result) && result > 0) {
				auto* form = RE::TESForm::LookupByID(static_cast<std::uint32_t>(result));
				made = form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr;
			}
		} else {
			std::uint32_t id = 0;
			try {
				id = ParseHex(base);
			} catch (const std::exception&) {
			}
			auto* object = id ? ItemFor(id) : nullptr;
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (object && dh) {
				static REL::Relocation<CreateRef_t> create{ REL::ID(407442) };
				RE::NiPoint3 at{ x, y, z };
				const float turn = (deg.empty() || deg == "-") ? 0.0f : static_cast<float>(std::stod(deg) * std::numbers::pi / 180.0);
				RE::NiPoint3 rot{ 0.0f, 0.0f, turn };
				bool fault = false;
				made = CreateGuarded(create.get(), dh, object, &at, &rot, cell, player->GetWorldSpace(), &fault);
				if (fault) {
					g_bodiesBroken = true;
					REX::ERROR("bodies: creating {} faulted, native bodies are off for this session", base);
				}
			}
		}
		if (!made) {
			g_bodyOut += "error body " + net + '\n';
			REX::WARN("bodies: could not make {} for {}", base, net);
			return;
		}
		Calm(made, ai != 0);
		g_bodies[made->GetFormID()] = { base, cell, false, ai != 0, 0, GetTickCount64() };
		g_bodyOut += "body " + net + ' ' + Hex(made->GetFormID()) + '\n';
		REX::INFO("bodies: made {} from {} for {}", Hex(made->GetFormID()), base, net);
	}

	void ParkBody(const std::string& a_ref)
	{
		auto* ref = OurBody(a_ref);
		if (!ref) {
			return;
		}
		auto& body = g_bodies[ref->GetFormID()];
		{
			std::lock_guard lock(g_bodyLock);
			g_glides.erase(ref->GetFormID());
			g_bodyMode.erase(ref->GetFormID());
		}
		RunCommand("Disable", ref, Args());
		g_bodyOut += "unbody " + Hex(ref->GetFormID()) + '\n';
		if (body.ai) {
			ref->SetDelete(true);
			g_bodies.erase(ref->GetFormID());
			return;
		}
		body.parked = true;
		body.parkedAt = GetTickCount64();
		std::vector<std::pair<std::uint64_t, std::uint32_t>> parked;
		for (const auto& [id, b] : g_bodies) {
			if (b.parked) {
				parked.emplace_back(b.parkedAt, id);
			}
		}
		std::sort(parked.begin(), parked.end());
		for (std::size_t i = 0; i + kBodiesParked < parked.size(); ++i) {
			auto* form = RE::TESForm::LookupByID(parked[i].second);
			if (auto* old = form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr) {
				old->SetDelete(true);
			}
			g_bodies.erase(parked[i].second);
		}
	}

	void AdoptBody(std::istringstream& a_in)
	{
		std::string net, refText;
		a_in >> net >> refText;
		auto* player = RE::PlayerCharacter::GetSingleton();
		RE::TESObjectREFR* ref = nullptr;
		try {
			auto* form = RE::TESForm::LookupByID(ParseHex(refText));
			ref = form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr;
		} catch (const std::exception&) {
		}
		if (!ref || ref->IsDeleted() || !player || !RE::oblivion_cast<RE::Actor*>(static_cast<RE::TESForm*>(ref))) {
			g_bodyOut += "error body " + net + '\n';
			return;
		}
		Pacify(ref);
		g_bodies[ref->GetFormID()] = { "console", player->parentCell, false, false, 0, GetTickCount64() };
		g_bodyOut += "body " + net + ' ' + Hex(ref->GetFormID()) + " adopted\n";
		REX::INFO("bodies: took over {} for {} as a puppet, AI on for animation, aggression 0 and combat stopped", Hex(ref->GetFormID()), net);
	}

	RE::TESObjectCELL* g_puppetCell = nullptr;
	std::uint32_t      g_puppetNext = 0;

	void KeepPuppets()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->parentCell : nullptr;
		if (cell != g_puppetCell) {
			g_puppetCell = cell;
			for (auto& [id, body] : g_bodies) {
				body.calmedAt = 0;
			}
		}
		const auto now = GetTickCount64();
		std::size_t done = 0;
		std::vector<std::uint32_t> due;
		auto it = g_bodies.lower_bound(g_puppetNext);
		for (std::size_t seen = 0; seen < g_bodies.size() && due.size() < kPuppetsPerTick; ++seen, ++it) {
			if (it == g_bodies.end()) {
				it = g_bodies.begin();
			}
			const auto& body = it->second;
			if (!body.ai && !body.parked && now - body.calmedAt >= kPuppetRecalmMs) {
				due.push_back(it->first);
			}
		}
		if (it == g_bodies.end()) {
			it = g_bodies.begin();
		}
		g_puppetNext = it != g_bodies.end() ? it->first : 0;
		for (const auto id : due) {
			auto* form = RE::TESForm::LookupByID(id);
			auto* ref = form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr;
			if (!ref || ref->IsDeleted()) {
				continue;
			}
			g_bodies[id].calmedAt = now;
			Pacify(ref);
			++done;
		}
		static int told = 0;
		if (done && told < 1) {
			++told;
			REX::INFO("bodies: puppets kept out of combat, checked again every {} ms and on every cell change", kPuppetRecalmMs);
		}
	}

	void RunBodyJob(const std::string& a_line)
	{
		struct Mark
		{
			Mark() { g_bodyJob = true; }
			~Mark() { g_bodyJob = false; }
		} mark;
		std::istringstream in(a_line);
		std::string verb;
		in >> verb;
		if (verb == "body") {
			SpawnBody(in);
			return;
		}
		if (verb == "adopt") {
			AdoptBody(in);
			return;
		}
		std::string refText;
		in >> refText;
		if (verb == "unbody") {
			ParkBody(refText);
			return;
		}
		auto* ref = OurBody(refText);
		if (!ref || g_bodies[ref->GetFormID()].parked) {
			return;
		}
		if (verb == "move") {
			float x = 0, y = 0, z = 0;
			std::string deg;
			if (in >> x >> y >> z) {
				in >> deg;
				PlaceBody(ref, x, y, z, deg);
			}
		} else if (verb == "face") {
			std::string deg;
			in >> deg;
			if (!deg.empty() && deg != "-") {
				RunCommand("SetAngle", ref, Args().Axis('Z').Number(RE::SCRIPT_PARAM_TYPE::kFloat, std::stod(deg)));
			}
		} else if (verb == "sneak") {
			int on = 0;
			in >> on;
			RunCommand("SetForceSneak", ref, Args().Number(RE::SCRIPT_PARAM_TYPE::kInt, on ? 1 : 0));
		} else if (verb == "group") {
			static const std::map<std::string, std::int16_t> groups{
				{ "idle", 0 }, { "forward", 3 }, { "backward", 4 }, { "left", 5 }, { "right", 6 }, { "fastforward", 7 }, { "fastbackward", 8 }, { "fastleft", 9 }, { "fastright", 10 }, { "equip", 17 }, { "unequip", 18 }, { "attackleft", 20 }, { "attackright", 21 }, { "attackpower", 22 },
				{ "blockidle", 27 }, { "blockhit", 28 }, { "recoil", 30 }, { "stagger", 31 }, { "castself", 34 }, { "casttouch", 35 },
				{ "casttarget", 36 }, { "jumpstart", 40 }, { "jumploop", 41 }, { "jumpland", 42 }
			};
			std::string name;
			int flags = 1;
			in >> name >> flags;
			for (auto& ch : name) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			const auto it = groups.find(name);
			if (it != groups.end()) {
				RunCommand("PlayGroup", ref, Args().Group(it->second).Number(RE::SCRIPT_PARAM_TYPE::kInt, flags));
			}
		}
	}

	float g_lastHour = -1.0f;
	std::uint64_t g_lastClock = 0;

	void KeepTime()
	{
		auto* ui = RE::InterfaceManager::GetInstance(false, false);
		auto* cal = RE::Calendar::GetSingleton();
		const auto now = GetTickCount64();
		if (!ui || !cal || !cal->gameHour || !cal->timeScale || !cal->gameDaysPassed) {
			g_lastClock = 0;
			return;
		}
		const bool menu = ui->menuMode != 1;
		if (g_lastClock && menu && cal->gameHour->value == g_lastHour && now - g_lastClock < 2000 && !PlayerDead()) {
			static constexpr int kMonthDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
			float hour = cal->gameHour->value + static_cast<float>(now - g_lastClock) / 1000.0f * cal->timeScale->value / 3600.0f;
			if (hour >= 24.0f) {
				hour -= 24.0f;
				cal->gameDaysPassed->value += 1.0f;
				if (cal->gameDay && cal->gameMonth) {
					const int month = std::clamp(static_cast<int>(cal->gameMonth->value), 0, 11);
					cal->gameDay->value += 1.0f;
					if (cal->gameDay->value > static_cast<float>(kMonthDays[month])) {
						cal->gameDay->value = 1.0f;
						cal->gameMonth->value = static_cast<float>((month + 1) % 12);
						if (month == 11 && cal->gameYear) {
							cal->gameYear->value += 1.0f;
						}
					}
				}
			}
			cal->gameHour->value = hour;
		}
		g_lastClock = now;
		g_lastHour = cal->gameHour->value;
	}

	bool MoveGuarded(RE::Actor* a_actor, float a_dt, RE::NiPoint3& a_delta, std::uint32_t a_flags)
	{
		__try {
			a_actor->Move(a_dt, a_delta, a_flags);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool g_walkBroken = false;

	void DrawGlides()
	{
		const auto now = GetTickCount64();
		if (now - g_glideDrawn < kGlideFrameMs || g_bodiesBroken) {
			return;
		}
		g_glideDrawn = now;
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->parentCell) {
			return;
		}
		struct Mark
		{
			Mark() { g_bodyJob = true; }
			~Mark() { g_bodyJob = false; }
		} mark;
		std::vector<std::pair<std::uint32_t, Glide>> work;
		{
			std::lock_guard lock(g_bodyLock);
			for (auto& [id, g] : g_glides) {
				if (!g.still) {
					work.emplace_back(id, g);
				}
			}
		}
		for (auto& [id, g] : work) {
			if (!g_bodies.count(id) || g_bodies[id].parked) {
				continue;
			}
			auto* form = RE::TESForm::LookupByID(id);
			auto* ref = form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr;
			if (!ref || ref->IsDeleted()) {
				continue;
			}
			const float ahead = (std::min)(kGlideAhead, kGlideLead + (now - g.at) / 1000.0f);
			const float px = g.x + g.vx * ahead;
			const float py = g.y + g.vy * ahead;
			const float pz = g.z + g.vz * ahead;
			const float dt = (std::min)(0.1f, g.drawn ? (now - g.drawn) / 1000.0f : 0.1f);
			const float off = std::sqrt((px - g.sx) * (px - g.sx) + (py - g.sy) * (py - g.sy) + (pz - g.sz) * (pz - g.sz));
			if (!g.shown || off > kGlideSnap) {
				g.sx = px;
				g.sy = py;
				g.sz = pz;
				g.sdeg = g.deg;
				g.shown = true;
			} else {
				const float k = 1.0f - std::exp(-dt * kGlideCatch);
				g.sx += (px - g.sx) * k;
				g.sy += (py - g.sy) * k;
				g.sz += (pz - g.sz) * k;
				const float turn = std::fmod(g.deg - g.sdeg + 540.0f, 360.0f) - 180.0f;
				g.sdeg += turn * (std::min)(1.0f, dt * 18.0f);
			}
			g.drawn = now;
			const bool resting = off < 1.0f && std::abs(g.vx) + std::abs(g.vy) + std::abs(g.vz) < 1.0f;
			auto* walker = RE::oblivion_cast<RE::Actor*>(static_cast<RE::TESForm*>(ref));
			const auto here = ref->data.location;
			const float ex = g.sx - here.x;
			const float ey = g.sy - here.y;
			const float gap = std::sqrt(ex * ex + ey * ey);
			bool walked = false;
			if (walker && !g_walkBroken && gap < 300.0f) {
				std::uint16_t flags = 0;
				{
					std::lock_guard lock(g_bodyLock);
					const auto mf = g_bodyMode.find(id);
					if (mf != g_bodyMode.end()) {
						flags = mf->second;
					}
				}
				RE::NiPoint3 step{ ex, ey, 0.0f };
				walked = MoveGuarded(walker, dt, step, flags);
				if (!walked) {
					g_walkBroken = true;
					REX::ERROR("bodies: the engine's own move faulted on {}, using SetPos for bodies from now on", Hex(id));
				}
				static int toldWalk = 0;
				if (walked && gap > 5.0f && ++toldWalk <= 3) {
					const auto after = ref->data.location;
					REX::INFO("bodies: walked {} with the engine's own move, gap {:.0f}, moved {:.1f}, flags {:04X}", Hex(id), gap, std::sqrt((after.x - here.x) * (after.x - here.x) + (after.y - here.y) * (after.y - here.y)), flags);
				}
			}
			if (!walked) {
				RunCommand("SetPos", ref, Args().Axis('X').Number(RE::SCRIPT_PARAM_TYPE::kFloat, g.sx));
				RunCommand("SetPos", ref, Args().Axis('Y').Number(RE::SCRIPT_PARAM_TYPE::kFloat, g.sy));
				RunCommand("SetPos", ref, Args().Axis('Z').Number(RE::SCRIPT_PARAM_TYPE::kFloat, g.sz));
			}
			RunCommand("SetAngle", ref, Args().Axis('Z').Number(RE::SCRIPT_PARAM_TYPE::kFloat, g.sdeg));
			std::lock_guard lock(g_bodyLock);
			auto it = g_glides.find(id);
			if (it != g_glides.end() && it->second.at == g.at) {
				it->second = g;
				it->second.still = resting;
			} else if (it != g_glides.end()) {
				it->second.sx = g.sx;
				it->second.sy = g.sy;
				it->second.sz = g.sz;
				it->second.sdeg = g.sdeg;
				it->second.drawn = g.drawn;
				it->second.shown = true;
			}
		}
		static bool told = false;
		if (!told && !work.empty()) {
			told = true;
			REX::INFO("bodies: other players now glide every frame with {} ms look-ahead", static_cast<int>(kGlideAhead * 1000));
		}
	}

	void PumpBodies()
	{
		g_inputTick = true;
		try {
			KeepTime();
		} catch (const std::exception& e) {
			REX::ERROR("game time in menus failed: {}", e.what());
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->parentCell) {
			return;
		}
		std::vector<std::string> jobs;
		{
			std::lock_guard lock(g_bodyLock);
			jobs.swap(g_bodyJobs);
		}
		std::vector<std::string> later;
		std::size_t steps = 0;
		const auto now = GetTickCount64();
		bool spawned = false;
		for (const auto& job : jobs) {
			const bool spawn = job.rfind("body ", 0) == 0;
			if (g_bodiesBroken) {
				continue;
			}
			if (steps >= kBodyStepsPerTick || (spawn && (spawned || now - g_lastSpawn < kSpawnGapMs))) {
				later.push_back(job);
				continue;
			}
			try {
				RunBodyJob(job);
			} catch (const std::exception& e) {
				REX::ERROR("bodies: {} failed: {}", job, e.what());
			}
			++steps;
			if (spawn) {
				spawned = true;
				g_lastSpawn = now;
			}
		}
		if (!later.empty()) {
			std::lock_guard lock(g_bodyLock);
			g_bodyJobs.insert(g_bodyJobs.begin(), later.begin(), later.end());
		}
		if (!g_bodiesBroken) {
			try {
				KeepPuppets();
			} catch (const std::exception& e) {
				REX::ERROR("bodies: keeping puppets calm failed: {}", e.what());
			}
		}
		static bool toldOff = false;
		if (g_bodiesBroken && !toldOff) {
			toldOff = true;
			g_bodyOut += "bodies off\n";
		}
		if (!g_bodyOut.empty()) {
			std::ofstream out(LinkDir() / L"native-bodies.txt", std::ios::app);
			if (out) {
				out << g_bodyOut;
				g_bodyOut.clear();
			}
		}
	}

	using PairIdle_t = void (*)(RE::Actor*, const char*, void*, const std::uint32_t*, const std::uint16_t*);
	using PairBare_t = void (*)(RE::Actor*, const char*, void*);
	using PairFlag_t = void (*)(RE::Actor*, const char*, void*, const bool*);
	using DynamicIdle_t = void (*)(RE::Actor*, std::uint32_t);

	struct IdlePairing
	{
		PairIdle_t  start;
		void*       startFn;
		const char* startName;
		PairBare_t  stop;
		void*       stopFn;
		const char* stopName;
		PairFlag_t  flag;
		void*       flagFn;
		const char* loopOn;
		const char* loopOff;
	};

	const IdlePairing& Pairing()
	{
		static const IdlePairing pairing{
			reinterpret_cast<PairIdle_t>(REL::ID(405849).address()),
			reinterpret_cast<void*>(REL::ID(303960).address()),
			reinterpret_cast<const char*>(REL::ID(1038233).address()),
			reinterpret_cast<PairBare_t>(REL::ID(405322).address()),
			reinterpret_cast<void*>(REL::ID(303961).address()),
			reinterpret_cast<const char*>(REL::ID(1038231).address()),
			reinterpret_cast<PairFlag_t>(REL::ID(405330).address()),
			reinterpret_cast<void*>(REL::ID(303911).address()),
			reinterpret_cast<const char*>(REL::ID(1038232).address()),
			reinterpret_cast<const char*>(REL::ID(1038234).address())
		};
		return pairing;
	}

	constexpr std::uint16_t kSpecialIdleGroup = 2;
	constexpr std::size_t   kDynamicIdleSlot = 0xFD;

	int PairIdleGuarded(const IdlePairing* a_p, RE::Actor* a_actor, std::uint32_t a_form, int a_how, bool a_reverse)
	{
		__try {
			if (a_how == 0) {
				const std::uint16_t group = kSpecialIdleGroup;
				a_p->start(a_actor, a_p->startName, a_p->startFn, &a_form, &group);
			} else if (a_how == 1) {
				auto** vtbl = *reinterpret_cast<void***>(a_actor);
				reinterpret_cast<DynamicIdle_t>(vtbl[kDynamicIdleSlot])(a_actor, a_form);
				if (a_reverse) {
					const bool on = true;
					a_p->flag(a_actor, a_p->loopOn, a_p->flagFn, &on);
				}
			} else {
				a_p->stop(a_actor, a_p->stopName, a_p->stopFn);
				if (a_reverse) {
					const bool off = false;
					a_p->flag(a_actor, a_p->loopOff, a_p->flagFn, &off);
				}
				a_actor->lastDynamicIdleFormId = 0;
			}
			return 1;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return -1;
		}
	}

	void RunLine(std::ostringstream& a_out, const std::string& a_line)
	{
		std::istringstream in(a_line);
		std::string verb;
		in >> verb;
		if (verb.empty() || verb == "seq") {
			return;
		}
		if (verb == "ping") {
			a_out << "pong\n";
			if (g_selfPump) {
				a_out << "pump self\n";
			}
			if (g_inputTick && !g_bodiesBroken) {
				a_out << "bodies on\n";
			}
			return;
		}
		if (BodyVerb(verb)) {
			QueueBody(verb, a_line);
			return;
		}
		if (verb == "state") {
			ReportState(a_out);
			return;
		}
		if (verb == "spell") {
			std::string how, idText;
			in >> how >> idText;
			auto* player = RE::PlayerCharacter::GetSingleton();
			RE::SpellItem* sp = nullptr;
			try {
				auto* form = RE::TESForm::LookupByID(ParseHex(idText));
				sp = form ? RE::oblivion_cast<RE::SpellItem*>(form) : nullptr;
			} catch (const std::exception&) {
			}
			if (!player || !sp) {
				a_out << "error spell " << how << ' ' << idText << '\n';
				return;
			}
			if (how == "remove") {
				player->RemoveSpell(sp);
			} else {
				player->AddSpell(sp);
			}
			return;
		}
		if (verb == "skills" || verb == "readylevel" || verb == "levelup") {
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				a_out << "error " << verb << " no player\n";
				return;
			}
			if (verb == "readylevel") {
				player->skillAdvanceCount = 10;
				player->readyToLevelUp = true;
			}
			if (verb == "levelup") {
				int level = 0;
				in >> level;
				auto* data = PlayerBaseData(player);
				if (!data || level < 2 || level > 255) {
					a_out << "error levelup did not take\n";
					return;
				}
				const auto before = player->GetLevel();
				data->actorData.level = static_cast<std::int16_t>(level);
				player->skillAdvanceCount = 0;
				player->readyToLevelUp = false;
				REX::INFO("levelled up from {} to {} without the base menu", before, player->GetLevel());
			}
			a_out << "skills " << player->GetLevel() << ' ' << player->skillAdvanceCount << ' ' << (player->readyToLevelUp ? 1 : 0);
			for (int i = 12; i <= 32; ++i) {
				a_out << ' ' << BaseValue(player, i);
			}
			for (int i = 0; i <= 7; ++i) {
				a_out << ' ' << BaseValue(player, i);
			}
			a_out << '\n';
			return;
		}
		if (verb == "toggle") {
			std::string name;
			in >> name;
			auto* command = RE::SCRIPT_FUNCTION::LocateConsoleCommand(name);
			if (!command) {
				for (auto& c : RE::SCRIPT_FUNCTION::GetConsoleFunctions()) {
					if (c.shortName && _stricmp(c.shortName, name.c_str()) == 0) {
						command = std::addressof(c);
						break;
					}
				}
			}
			if (!command || !command->executeFunction || command->paramCount != 0) {
				a_out << "error toggle " << name << '\n';
				return;
			}
			double result = 0;
			std::uint32_t offset = 0;
			const bool ran = command->executeFunction(command->parameters, nullptr, nullptr, nullptr, nullptr, nullptr, result, offset);
			a_out << "toggled " << name << ' ' << (ran ? 1 : 0) << '\n';
			return;
		}
		if (verb == "unlock") {
			g_unlockNow = true;
			return;
		}
		if (verb == "setav") {
			static const std::map<std::string, int> names{
				{ "strength", 0 }, { "intelligence", 1 }, { "willpower", 2 }, { "agility", 3 }, { "speed", 4 }, { "endurance", 5 },
				{ "personality", 6 }, { "luck", 7 }, { "health", 8 }, { "magicka", 9 }, { "fatigue", 10 },
				{ "armorer", 12 }, { "athletics", 13 }, { "blade", 14 }, { "block", 15 }, { "blunt", 16 }, { "handtohand", 17 },
				{ "heavyarmor", 18 }, { "alchemy", 19 }, { "alteration", 20 }, { "conjuration", 21 }, { "destruction", 22 },
				{ "illusion", 23 }, { "mysticism", 24 }, { "restoration", 25 }, { "acrobatics", 26 }, { "lightarmor", 27 },
				{ "marksman", 28 }, { "mercantile", 29 }, { "security", 30 }, { "sneak", 31 }, { "speechcraft", 32 }
			};
			std::string name;
			float value = 0;
			in >> name >> value;
			for (auto& ch : name) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (name == "level") {
				auto* data = PlayerBaseData(player);
				if (!data || value < 1 || value > 255) {
					a_out << "error setav level\n";
					return;
				}
				data->actorData.level = static_cast<std::int16_t>(value);
				return;
			}
			const auto it = names.find(name);
			if (!player || it == names.end()) {
				a_out << "error setav " << name << '\n';
				return;
			}
			SetValue(player, it->second, value);
			return;
		}
		if (verb == "questguard") {
			g_questGuard = true;
			a_out << "questguard on\n";
			return;
		}
		if (verb == "quests") {
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				a_out << "error quests no data handler\n";
				return;
			}
			int n = 0;
			for (auto* q : dh->listQuests) {
				if (!q) {
					continue;
				}
				const bool on = (q->data.flags & 1) != 0;
				if (!on && q->currentStage == 0) {
					continue;
				}
				const char* ed = q->formEditorID.c_str();
				const char* nm = q->fullName.c_str();
				a_out << "quest " << Hex(q->GetFormID()) << " " << (ed && *ed ? ed : "-") << " stage " << static_cast<int>(q->currentStage) << (on ? " running" : "") << " " << (nm ? nm : "") << "\n";
				++n;
			}
			a_out << "quests " << n << "\n";
			return;
		}
		if (verb == "respawn") {
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				a_out << "error respawn no player\n";
				return;
			}
			const bool wasDead = player->IsDead(false);
			player->Resurrect(false, true, true);
			g_deathNoted = false;
			std::error_code ec;
			std::filesystem::remove(LinkDir() / L"died.txt", ec);
			const auto have = Totals(player);
			std::istringstream rows(g_lastInv);
			std::size_t back = 0, worn = 0, gone = 0;
			std::map<std::uint32_t, bool> returned;
			for (std::string row; std::getline(rows, row);) {
				std::istringstream cols(row);
				std::string word, form;
				std::int32_t count = 0, on = 0;
				if (!(cols >> word >> form >> count >> on) || word != "item") {
					continue;
				}
				const auto id = ParseHex(form);
				const auto it = have.find(id);
				const auto now = it == have.end() ? 0 : it->second;
				if (now < count) {
					Change(player, id, count - now);
					returned[id] = true;
					++back;
				}
			}
			const auto wearing = WornNow(player);
			for (const auto& [id, was] : g_deathWorn) {
				if (was && !wearing.count(id)) {
					if (auto* item = ItemFor(id)) {
						player->EquipObject(item, 1, nullptr, false, false);
						++worn;
					}
				}
			}
			g_deathWorn.clear();
			const auto nowMs = GetTickCount64();
			for (const auto& drop : g_drops) {
				if (nowMs - drop.at > kDeathDropMs || !returned.count(drop.object)) {
					continue;
				}
				auto* form = RE::TESForm::LookupByID(drop.ref);
				if (auto* dropped = form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr) {
					dropped->SetDelete(true);
					REX::INFO("removed the dropped copy {} of {} after giving it back", Hex(drop.ref), Hex(drop.object));
					++gone;
				}
			}
			g_drops.clear();
			a_out << "respawned " << (wasDead ? "from death" : "while alive") << ", " << back << " items put back, " << worn << " worn again, " << gone << " dropped copies removed\n";
			return;
		}
		if (verb == "mine") {
			std::uint32_t net = 0;
			std::string refText;
			in >> net >> refText;
			if (net && !refText.empty()) {
				const auto ref = ParseHex(refText);
				if (g_dropByRef.count(ref)) {
					g_dropByRef[ref] = net;
					g_refByDrop[net] = ref;
				}
			}
			return;
		}
		if (verb == "place") {
			std::uint32_t net = 0;
			std::string formText;
			std::int32_t count = 0;
			RE::NiPoint3 at{};
			in >> net >> formText >> count >> at.x >> at.y >> at.z;
			if (!net || g_refByDrop.count(net)) {
				return;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* item = formText.empty() ? nullptr : ItemFor(ParseHex(formText));
			if (!player || !item || count <= 0) {
				a_out << "error place " << net << '\n';
				return;
			}
			RE::NiPoint3 turn{};
			g_placing = true;
			player->AddObjectToContainer(item, nullptr, count);
			auto* placed = player->RemoveItem(item, nullptr, count, false, true, nullptr, &at, &turn, false, false);
			g_placing = false;
			if (placed) {
				g_refByDrop[net] = placed->GetFormID();
				g_dropByRef[placed->GetFormID()] = net;
				g_dropAt[placed->GetFormID()] = placed->data.location;
				a_out << "placed " << net << ' ' << Hex(placed->GetFormID()) << '\n';
			}
			return;
		}
		if (verb == "unplace") {
			std::uint32_t net = 0;
			in >> net;
			const auto it = g_refByDrop.find(net);
			if (it == g_refByDrop.end()) {
				return;
			}
			auto* form = RE::TESForm::LookupByID(it->second);
			if (auto* refr = form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr) {
				refr->SetDelete(true);
			}
			g_dropByRef.erase(it->second);
			g_dropAt.erase(it->second);
			g_refByDrop.erase(it);
			return;
		}
		if (verb == "idle" || verb == "idlestop") {
			std::string what, refText, how = "once", word;
			if (verb == "idle") {
				in >> what;
			}
			while (in >> word) {
				if (word == "once" || word == "loop") {
					how = word;
				} else if (word != "-") {
					refText = word;
				}
			}
			static bool idleBroken = false;
			if (idleBroken) {
				a_out << "error idle off\n";
				return;
			}
			RE::TESObjectREFR* who = refText.empty() ? static_cast<RE::TESObjectREFR*>(RE::PlayerCharacter::GetSingleton()) : RefFor(refText);
			auto* actor = who ? RE::oblivion_cast<RE::Actor*>(static_cast<RE::TESForm*>(who)) : nullptr;
			if (!actor) {
				a_out << "error " << verb << " no actor " << refText << '\n';
				return;
			}
			const bool reverse = actor == RE::PlayerCharacter::GetSingleton();
			if (verb == "idlestop") {
				if (PairIdleGuarded(&Pairing(), actor, 0, 2, reverse) < 0) {
					idleBroken = true;
					REX::ERROR("stopping an idle on {} faulted, game idles are off for this session", Hex(actor->GetFormID()));
					a_out << "error idle off\n";
					return;
				}
				a_out << "idle stopped\n";
				return;
			}
			RE::TESForm* form = RE::TESForm::LookupByEditorID(what);
			if (!form && !what.empty() && what.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
				try {
					form = RE::TESForm::LookupByID(ParseHex(what));
				} catch (const std::exception&) {
				}
			}
			if (!form || !form->Is(RE::FormType::Idle)) {
				a_out << "error idle no idle called " << what << '\n';
				return;
			}
			if (PairIdleGuarded(&Pairing(), actor, form->GetFormID(), how == "loop" ? 1 : 0, reverse) < 0) {
				idleBroken = true;
				REX::ERROR("playing the idle {} on {} faulted, game idles are off for this session", what, Hex(actor->GetFormID()));
				a_out << "error idle off\n";
				return;
			}
			a_out << "idle playing " << what << ' ' << Hex(form->GetFormID()) << ' ' << how << '\n';
			REX::INFO("asked the game to play the idle {} {} {} on {}", what, Hex(form->GetFormID()), how, Hex(actor->GetFormID()));
			return;
		}
		if (verb == "relocate" || verb == "delref") {
			std::string refText;
			RE::NiPoint3 at{};
			float turn = 0;
			in >> refText >> at.x >> at.y >> at.z >> turn;
			auto* ref = refText.empty() ? nullptr : RefFor(refText);
			if (!ref) {
				a_out << "error " << verb << ' ' << refText << '\n';
				return;
			}
			if (verb == "delref") {
				ref->SetDelete(true);
				a_out << "deleted " << Hex(ref->GetFormID()) << '\n';
				return;
			}
			ref->data.location = at;
			ref->data.angle.z = turn;
			if (g_dropAt.count(ref->GetFormID())) {
				g_dropAt[ref->GetFormID()] = at;
			}
			a_out << "relocated " << Hex(ref->GetFormID()) << '\n';
			return;
		}
		if (verb == "diseases") {
			RunDiseases(a_out);
			return;
		}
		if (verb == "cure") {
			std::string formText;
			in >> formText;
			try {
				RunCure(a_out, ParseHex(formText));
			} catch (const std::exception&) {
			}
			return;
		}
		if (verb == "gear") {
			RunGear(a_out);
			return;
		}
		if (verb == "repair") {
			std::string formText;
			int pct = 100;
			in >> formText >> pct;
			try {
				RunRepair(a_out, ParseHex(formText), (std::max)(1, (std::min)(125, pct)));
			} catch (const std::exception&) {
			}
			return;
		}
		if (verb == "mount") {
			std::string riderText, horseText;
			in >> riderText >> horseText;
			auto* rider = ActorByRef(riderText);
			auto* horseActor = ActorByRef(horseText);
			auto* horse = horseActor ? RE::oblivion_cast<RE::Creature*>(static_cast<RE::TESForm*>(horseActor)) : nullptr;
			if (!rider || !horse || rider == RE::PlayerCharacter::GetSingleton()) {
				a_out << "error mount " << riderText << ' ' << horseText << '\n';
				return;
			}
			if (rider->GetHorse() != horse) {
				rider->InitiateMountHorsePackage(horse);
			}
			a_out << "mounted " << riderText << '\n';
			return;
		}
		if (verb == "dismount") {
			std::string riderText;
			in >> riderText;
			auto* rider = ActorByRef(riderText);
			if (rider && rider != RE::PlayerCharacter::GetSingleton() && rider->GetHorse()) {
				rider->InitiateDismountHorsePackage();
			}
			a_out << "dismounted " << riderText << '\n';
			return;
		}
		if (verb == "give" || verb == "take") {
			std::string formText;
			std::int32_t count = 0;
			in >> formText >> count;
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || formText.empty() || count <= 0) {
				return;
			}
			try {
				Change(player, ParseHex(formText), verb == "give" ? count : -count);
			} catch (const std::exception&) {
			}
			return;
		}
		if (verb == "hurt") {
			float amount = 0;
			in >> amount;
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player && amount > 0 && !PlayerDead()) {
				ScriptMod(player, 8, -amount);
			}
			return;
		}
		if (verb == "boxes") {
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->parentCell : nullptr;
			std::size_t n = 0;
			if (cell) {
				for (auto* ref : cell->listReferences) {
					if (!ref || ref == player || ref->IsDeleted() || (ref->GetFormFlags() & RE::TESForm::RecordFlags::kDisabled) || !ref->data.objectReference) {
						continue;
					}
					const bool box = ref->data.objectReference->Is(RE::FormType::Container);
					const bool body = !box && DeadBody(ref, player);
					if (!box && !body) {
						continue;
					}
					std::size_t kinds = 0;
					for (const auto& [form, count] : Totals(ref)) {
						if (count > 0) {
							++kinds;
						}
					}
					a_out << "box " << Hex(ref->GetFormID()) << ' ' << Hex(ref->data.objectReference->GetFormID()) << ' ' << (box ? 'c' : 'b') << ' ' << kinds << '\n';
					if (++n >= 120) {
						break;
					}
				}
			}
			a_out << "boxes " << n << '\n';
			return;
		}
		if (verb == "disable") {
			std::size_t done = 0, missing = 0;
			for (std::string refText; in >> refText;) {
				auto* ref = RefFor(refText);
				if (!ref) {
					++missing;
					continue;
				}
				static std::set<std::uint32_t> faulted;
				const auto id = ref->GetFormID();
				if (faulted.count(id)) {
					++missing;
					continue;
				}
				if (!(ref->GetFormFlags() & RE::TESForm::RecordFlags::kDisabled)) {
					if (!ref->Get3D()) {
						ref->formFlags |= RE::TESForm::RecordFlags::kDisabled;
					} else if (!RunCommand("Disable", ref, Args())) {
						faulted.insert(id);
						REX::WARN("left {} alone from now on, disabling it did not work", Hex(id));
					}
				}
				++done;
			}
			a_out << "disabled " << done << ' ' << missing << '\n';
			return;
		}
		if (verb == "near") {
			std::string kinds;
			float reach = 300.0f;
			in >> kinds >> reach;
			RunNear(a_out, kinds, reach > 0 ? reach : 300.0f);
			return;
		}
		if (verb == "picked") {
			std::string refText;
			in >> refText;
			auto* form = refText.empty() ? nullptr : RE::TESForm::LookupByID(ParseHex(refText));
			auto* target = form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr;
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* base = target && target->data.objectReference ? RE::oblivion_cast<RE::TESFlora*>(static_cast<RE::TESForm*>(target->data.objectReference)) : nullptr;
			if (!target || !player || !base || !g_floraActivate) {
				return;
			}
			const auto before = Totals(player);
			g_silentPick = true;
			g_floraActivate(base, target, player, false, nullptr, 1);
			g_silentPick = false;
			for (const auto& [id, count] : Totals(player)) {
				const auto it = before.find(id);
				const auto had = it == before.end() ? 0 : it->second;
				if (count > had) {
					Change(player, id, had - count);
				}
			}
			return;
		}
		if (verb == "live") {
			g_live = true;
			g_lastInv.clear();
			a_out << "live on\n";
			return;
		}
		std::string refText, formText;
		std::int32_t number = 0;
		in >> refText >> formText >> number;
		auto* ref = refText.empty() ? nullptr : RefFor(refText);
		if (!ref) {
			a_out << "error " << verb << " no such reference " << refText << '\n';
			return;
		}
		if (verb == "inv") {
			a_out << "inv " << Hex(ref->GetFormID()) << '\n';
			ReportInventory(a_out, ref);
			return;
		}
		if (verb == "base") {
			ReportBase(a_out, ref);
			return;
		}
		if (verb == "setinv") {
			const auto changed = SetExact(ref, ParseCounts(formText));
			a_out << "setinv " << Hex(ref->GetFormID()) << ' ' << changed << '\n';
			return;
		}
		if (verb == "equip") {
			auto* actor = RE::oblivion_cast<RE::Actor*>(static_cast<RE::TESForm*>(ref));
			auto* item = formText.empty() ? nullptr : ItemFor(ParseHex(formText));
			if (!actor || !item) {
				a_out << "error equip " << formText << '\n';
				return;
			}
			actor->EquipObject(item, 1, nullptr, false, false);
			a_out << "equipped " << Hex(item->GetFormID()) << '\n';
			return;
		}
		auto* object = formText.empty() ? nullptr : ItemFor(ParseHex(formText));
		if (!object || number <= 0) {
			a_out << "error " << verb << " bad item or count " << formText << ' ' << number << '\n';
			return;
		}
		if (verb == "add") {
			Change(ref, object->GetFormID(), number);
			a_out << "added " << Hex(ref->GetFormID()) << ' ' << Hex(object->GetFormID()) << ' ' << number << '\n';
		} else if (verb == "remove") {
			Change(ref, object->GetFormID(), -number);
			a_out << "removed " << Hex(ref->GetFormID()) << ' ' << Hex(object->GetFormID()) << ' ' << number << '\n';
		} else {
			a_out << "error unknown " << verb << '\n';
		}
	}

	void RunWorld()
	{
		const auto file = LinkDir() / L"native-world.txt";
		const auto work = std::filesystem::path(file).concat(L".work");
		std::error_code ec;
		std::filesystem::rename(file, work, ec);
		if (ec) {
			return;
		}
		std::vector<std::string> lines;
		{
			std::ifstream req(work);
			for (std::string line; std::getline(req, line);) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				if (!line.empty()) {
					lines.push_back(line);
				}
			}
		}
		std::filesystem::remove(work, ec);
		for (const auto& line : lines) {
			std::ostringstream out;
			try {
				RunLine(out, line);
			} catch (const std::exception& e) {
				out << "error " << e.what() << '\n';
			}
			std::istringstream back(out.str());
			for (std::string row; std::getline(back, row);) {
				if (!row.empty()) {
					Note(row);
				}
			}
		}
	}

	void RunRequests()
	{
		const auto dir = LinkDir();
		const auto file = dir / L"native-req.txt";
		std::vector<std::string> lines;
		{
			std::ifstream req(file);
			if (!req) {
				return;
			}
			for (std::string line; std::getline(req, line);) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				lines.push_back(line);
			}
		}
		std::error_code ec;
		std::filesystem::remove(file, ec);
		std::ostringstream out;
		out << (lines.empty() ? std::string("seq 0") : lines.front()) << '\n';
		for (const auto& line : lines) {
			try {
				RunLine(out, line);
			} catch (const std::exception& e) {
				out << "error " << e.what() << '\n';
			}
		}
		WriteAtomic(dir / L"native-res.txt", out.str());
	}

	std::uint64_t g_lastQuestSweep = 0;

	std::uint32_t g_packSpell = 0;
	RE::TESObjectCELL* g_unlockedCell = nullptr;
	std::uint64_t g_lastUnlock = 0;

	void UnlockDoors()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->parentCell : nullptr;
		if (!cell) {
			return;
		}
		const auto now = GetTickCount64();
		if (cell == g_unlockedCell && now - g_lastUnlock < 5000 && !g_unlockNow) {
			return;
		}
		g_unlockNow = false;
		g_unlockedCell = cell;
		g_lastUnlock = now;
		std::size_t n = 0;
		for (auto* ref : cell->listReferences) {
			auto* base = ref ? ref->data.objectReference : nullptr;
			if (!base || !base->Is(RE::FormType::Door)) {
				continue;
			}
			if (ref->extra.GetExtraData(RE::EXTRA_DATA_TYPE::Lock)) {
				ref->extra.RemoveExtra(RE::EXTRA_DATA_TYPE::Lock);
				++n;
			}
		}
		if (n) {
			REX::INFO("unlocked {} doors here, our own system decides who may open them", n);
		}
	}

	void WatchPacks()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || g_job.packs.empty() || PlayerDead()) {
			return;
		}
		const auto have = Totals(player);
		std::uint32_t want = 0;
		std::uint32_t best = 0;
		for (const auto& p : g_job.packs) {
			const auto it = have.find(p[0]);
			if (it != have.end() && it->second > 0 && p[2] > best) {
				best = p[2];
				want = (p[0] & 0xFF000000) | (p[1] & 0x00FFFFFF);
			}
		}
		if (want == g_packSpell) {
			return;
		}
		auto spellOf = [](std::uint32_t a_id) -> RE::SpellItem* {
			auto* form = a_id ? RE::TESForm::LookupByID(a_id) : nullptr;
			return form ? RE::oblivion_cast<RE::SpellItem*>(form) : nullptr;
		};
		if (auto* old = spellOf(g_packSpell)) {
			player->RemoveSpell(old);
		}
		if (auto* add = spellOf(want)) {
			player->AddSpell(add);
		}
		REX::INFO("pack bonus is now +{} carry weight", best);
		g_packSpell = want;
	}

	std::set<std::uint32_t> g_perksOn;
	bool g_perksSwept = false;

	void WatchPerks()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !g_job.perksSaid || PlayerDead()) {
			return;
		}
		auto spellOf = [](std::uint32_t a_id) -> RE::SpellItem* {
			auto* form = a_id ? RE::TESForm::LookupByID(a_id) : nullptr;
			return form ? RE::oblivion_cast<RE::SpellItem*>(form) : nullptr;
		};
		if (!g_perksSwept) {
			g_perksSwept = true;
			for (const auto id : g_job.perkAll) {
				if (!g_job.perks.count(id)) {
					if (auto* sp = spellOf(id)) {
						player->RemoveSpell(sp);
					}
				}
			}
		}
		for (auto it = g_perksOn.begin(); it != g_perksOn.end();) {
			if (!g_job.perks.count(*it)) {
				if (auto* sp = spellOf(*it)) {
					player->RemoveSpell(sp);
				}
				it = g_perksOn.erase(it);
			} else {
				++it;
			}
		}
		for (const auto id : g_job.perks) {
			if (g_perksOn.count(id)) {
				continue;
			}
			if (auto* sp = spellOf(id)) {
				player->AddSpell(sp);
				g_perksOn.insert(id);
				REX::INFO("perk {} is on", Hex(id));
			}
		}
	}

	void GuardQuests()
	{
		if (!g_questGuard) {
			return;
		}
		const auto now = GetTickCount64();
		if (now - g_lastQuestSweep < 1000) {
			return;
		}
		g_lastQuestSweep = now;
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return;
		}
		static const std::set<std::string> keep{ "crime", "disease", "vampire", "charactergen", "dispositionblockers" };
		for (auto* q : dh->listQuests) {
			if (!q || !(q->data.flags & 1)) {
				continue;
			}
			const char* ed = q->formEditorID.c_str();
			std::string low = ed ? ed : "";
			for (auto& ch : low) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			if (keep.count(low)) {
				continue;
			}
			q->data.flags = static_cast<std::uint8_t>(q->data.flags & ~1);
			REX::INFO("quest {} started during play, stopped it", ed ? ed : "-");
		}
	}

	struct Loose
	{
		std::uint32_t base;
		RE::NiPoint3 at;
	};
	std::map<std::uint32_t, Loose> g_loose;
	std::set<std::uint32_t> g_restored;
	RE::TESObjectCELL* g_looseCell = nullptr;
	std::uint64_t g_looseAt = 0;
	bool g_takeOk = false;
	std::uint64_t g_takeOkAt = 0;
	constexpr std::uint64_t kGrabMs = 1500;
	constexpr float kGrabReach = 700.0f;

	bool Gone(RE::TESObjectREFR* a_ref)
	{
		return !a_ref || a_ref->IsDeleted() || (a_ref->GetFormFlags() & RE::TESForm::RecordFlags::kDisabled) || !a_ref->parentCell;
	}

	bool TakeAllowed()
	{
		const auto now = GetTickCount64();
		if (now - g_takeOkAt >= 2000) {
			g_takeOkAt = now;
			std::ifstream in(LinkDir() / L"take-world.txt");
			std::string v;
			g_takeOk = in && std::getline(in, v) && !v.empty() && v[0] == '1';
		}
		return g_takeOk;
	}

	void ScanLoose()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->parentCell : nullptr;
		const auto now = GetTickCount64();
		if (!cell || (cell == g_looseCell && now - g_looseAt < 1000) || !g_grabs.empty()) {
			return;
		}
		g_looseAt = now;
		if (cell != g_looseCell) {
			g_restored.clear();
		}
		g_looseCell = cell;
		g_loose.clear();
		for (auto* ref : cell->listReferences) {
			if (!ref || Gone(ref) || !ref->data.objectReference) {
				continue;
			}
			const auto id = ref->GetFormID();
			if ((!Shared(id) && !g_restored.count(id)) || g_dropByRef.count(id)) {
				continue;
			}
			if (!LooseKind(ref->data.objectReference->GetFormType())) {
				continue;
			}
			g_loose[id] = { ref->data.objectReference->GetFormID(), ref->data.location };
		}
	}

	void WatchGrabs()
	{
		if (g_grabs.empty()) {
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		const auto now = GetTickCount64();
		if (!player || PlayerDead() || TakeAllowed()) {
			g_grabs.clear();
			return;
		}
		const auto me = player->data.location;
		std::vector<Grab> keep;
		for (const auto& grab : g_grabs) {
			std::uint32_t hit = 0;
			float best = kGrabReach * kGrabReach;
			for (const auto& [id, loose] : g_loose) {
				if (loose.base != grab.form) {
					continue;
				}
				auto* form = RE::TESForm::LookupByID(id);
				auto* refr = form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr;
				if (refr && !Gone(refr)) {
					continue;
				}
				const float dx = loose.at.x - me.x, dy = loose.at.y - me.y, dz = loose.at.z - me.z;
				const float d = dx * dx + dy * dy + dz * dz;
				if (d <= best) {
					best = d;
					hit = id;
				}
			}
			if (!hit) {
				if (now - grab.at < kGrabMs) {
					keep.push_back(grab);
				}
				continue;
			}
			const auto at = g_loose[hit].at;
			g_loose.erase(hit);
			auto* form = RE::TESForm::LookupByID(hit);
			auto* refr = form ? RE::oblivion_cast<RE::TESObjectREFR*>(form) : nullptr;
			if (refr) {
				refr->SetDelete(false);
				RunCommand("Enable", refr, Args());
			}
			if (refr && !Gone(refr)) {
				Change(player, grab.form, -grab.number);
				g_loose[hit] = { grab.form, at };
				REX::INFO("put back {} of {} at its ref {}, taking things lying about is not allowed", grab.number, Hex(grab.form), Hex(hit));
			} else if (auto* item = ItemFor(grab.form)) {
				RE::NiPoint3 spot = at;
				RE::NiPoint3 turn{};
				g_placing = true;
				auto* placed = player->RemoveItem(item, nullptr, grab.number, false, true, nullptr, &spot, &turn, false, false);
				g_placing = false;
				if (placed) {
					g_restored.insert(placed->GetFormID());
					g_loose[placed->GetFormID()] = { grab.form, placed->data.location };
				}
				REX::INFO("dropped {} of {} back where it lay, its old ref {} could not come back", grab.number, Hex(grab.form), Hex(hit));
			}
			Note("deny world " + Hex(grab.form) + ' ' + std::to_string(grab.number));
		}
		g_grabs = keep;
	}

	void Pump()
	{
		try {
			TakeBack();
			CheckRoll();
			CheckClosed();
			RunRequests();
			GuardQuests();
			WatchPacks();
			WatchPerks();
			UnlockDoors();
			RunWorld();
			WatchDrops();
			WatchGrabs();
			ScanLoose();
			WatchPlayer();
			Sweep();
			GuardMenus();
			WatchRide();
		} catch (const std::exception& e) {
			REX::ERROR("native bridge failed: {}", e.what());
		}
	}

	bool Execute(const RE::SCRIPT_PARAMETER*, const char*, RE::TESObjectREFR*, RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*, double& a_result, std::uint32_t&)
	{
		Pump();
		a_result = 1;
		return true;
	}

	using PlayerUpdate_t = void (*)(RE::PlayerCharacter*, float);
	PlayerUpdate_t g_playerUpdate = nullptr;
	std::uint64_t g_lastPump = 0;

	void PlayerUpdate(RE::PlayerCharacter* a_this, float a_delta)
	{
		g_playerUpdate(a_this, a_delta);
		g_selfPump = true;
		const auto now = GetTickCount64();
		if (now - g_lastPump >= 100) {
			g_lastPump = now;
			Pump();
		}
	}

	void QuietQuests()
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return;
		}
		const std::set<std::string> keep{ "crime", "disease", "vampire", "charactergen", "dispositionblockers" };
		int n = 0;
		for (auto* q : dh->listQuests) {
			if (!q || !(q->data.flags & 1)) {
				continue;
			}
			const char* ed = q->formEditorID.c_str();
			std::string low = ed ? ed : "";
			for (auto& ch : low) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			if (keep.count(low)) {
				continue;
			}
			q->data.flags = static_cast<std::uint8_t>(q->data.flags & ~1);
			++n;
		}
		REX::INFO("{} quests will not start with a new game", n);
	}

	void HookPlayerUpdate()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::PlayerCharacter[0] };
		g_playerUpdate = reinterpret_cast<PlayerUpdate_t>(vtbl.write_vfunc(0x99, &PlayerUpdate));
		g_selfPump = false;
		REX::INFO("player update hooked, the bridge runs itself every 100 ms");
	}

	void Install()
	{
		for (const auto spare : kSpareCommands) {
			auto* command = RE::SCRIPT_FUNCTION::LocateConsoleCommand(spare);
			if (!command) {
				continue;
			}
			DWORD old = 0;
			VirtualProtect(command, sizeof(RE::SCRIPT_FUNCTION), PAGE_READWRITE, &old);
			command->functionName = kCommandName;
			command->shortName = kCommandShort;
			command->helpString = "Oblivion Reborn native bridge";
			command->referenceFunction = false;
			command->SetParameters();
			command->executeFunction = Execute;
			VirtualProtect(command, sizeof(RE::SCRIPT_FUNCTION), old, &old);
			REX::INFO("took over the console command {} as {}", spare, kCommandName);
			return;
		}
		REX::WARN("no spare console command found, {} is not available", kCommandName);
	}

	using XInputGetState_t = DWORD(WINAPI*)(DWORD, void*);
	XInputGetState_t g_xinputPrevious = nullptr;
	DWORD g_gameThread = 0;
	bool g_dataReady = false;
	std::uint64_t g_lastTick = 0;

	DWORD WINAPI TickOnInput(DWORD a_user, void* a_state)
	{
		const DWORD rc = g_xinputPrevious ? g_xinputPrevious(a_user, a_state) : ERROR_DEVICE_NOT_CONNECTED;
		static bool told = false;
		if (!told && g_dataReady) {
			told = true;
			REX::INFO("input read seen on thread {}, game thread is {}", GetCurrentThreadId(), g_gameThread);
		}
		if (a_user == 0 && g_dataReady && GetCurrentThreadId() == g_gameThread) {
			const auto now = GetTickCount64();
			if (now - g_lastTick >= 100) {
				g_lastTick = now;
				g_selfPump = true;
				g_inputTick = true;
				Pump();
				PumpBodies();
			}
		}
		return rc;
	}

	using HudIdle_t = void (*)(RE::HUDMainMenu*);
	HudIdle_t g_hudIdle = nullptr;

	void HoldForConsole()
	{
		std::error_code ec;
		const auto want = LinkDir() / L"gate-want.txt";
		if (!std::filesystem::exists(want, ec)) {
			return;
		}
		const auto held = LinkDir() / L"gate-held.txt";
		WriteAtomic(held, "held\n");
		const auto from = GetTickCount64();
		while (std::filesystem::exists(want, ec) && GetTickCount64() - from < 600) {
			Sleep(1);
		}
		std::filesystem::remove(held, ec);
		static int told = 0;
		if (++told <= 5) {
			REX::INFO("held the game thread {} ms while a console command ran", GetTickCount64() - from);
		}
	}

	void HudIdle(RE::HUDMainMenu* a_self)
	{
		g_hudIdle(a_self);
		if (g_dataReady && GetCurrentThreadId() == g_gameThread) {
			HoldForConsole();
		}
		static bool told = false;
		if (!told && g_dataReady) {
			told = true;
			REX::INFO("the HUD idle runs on thread {}, game thread is {}", GetCurrentThreadId(), g_gameThread);
		}
		if (!g_dataReady || GetCurrentThreadId() != g_gameThread) {
			return;
		}
		DrawGlides();
		ApplyModes();
		const auto now = GetTickCount64();
		if (now - g_lastTick >= 100) {
			g_lastTick = now;
			g_selfPump = true;
			g_inputTick = true;
			Pump();
			PumpBodies();
		}
	}

	void HookHudTick()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::HUDMainMenu[0] };
		g_hudIdle = reinterpret_cast<HudIdle_t>(vtbl.write_vfunc(0x12, &HudIdle));
		REX::INFO("the HUD idle now runs the bridge every 100 ms");
	}

	bool HookInputTick()
	{
		g_gameThread = GetCurrentThreadId();
		auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress) {
			return false;
		}
		for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
			const char* dll = reinterpret_cast<const char*>(base + desc->Name);
			if (_strnicmp(dll, "xinput", 6) != 0) {
				continue;
			}
			auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
			auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
			for (; names->u1.AddressOfData; ++names, ++slots) {
				const bool match = IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)
				                       ? IMAGE_ORDINAL64(names->u1.Ordinal) == 2
				                       : std::strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name, "XInputGetState") == 0;
				if (!match) {
					continue;
				}
				auto* slot = reinterpret_cast<XInputGetState_t*>(&slots->u1.Function);
				DWORD old = 0;
				if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
					return false;
				}
				g_xinputPrevious = *slot;
				*slot = &TickOnInput;
				VirtualProtect(slot, sizeof(void*), old, &old);
				REX::INFO("the game's {} input read now runs the bridge every 100 ms on the game thread", dll);
				return true;
			}
		}
		REX::WARN("the game imports no XInputGetState, the bridge falls back to console wake-ups");
		return false;
	}

	using PeekMessage_t = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
	PeekMessage_t g_peekPrevious = nullptr;

	BOOL WINAPI TickOnMessages(LPMSG a_msg, HWND a_wnd, UINT a_min, UINT a_max, UINT a_remove)
	{
		const BOOL rc = g_peekPrevious(a_msg, a_wnd, a_min, a_max, a_remove);
		if (!g_dataReady) {
			return rc;
		}
		static bool told = false;
		if (!told) {
			told = true;
			REX::INFO("the message check runs on thread {}, game thread is {}", GetCurrentThreadId(), g_gameThread);
		}
		if (GetCurrentThreadId() == g_gameThread) {
			DrawGlides();
			ApplyModes();
			const auto now = GetTickCount64();
			if (now - g_lastTick >= 100) {
				g_lastTick = now;
				g_selfPump = true;
				g_inputTick = true;
				Pump();
				PumpBodies();
			}
		}
		return rc;
	}

	bool HookMessageTick()
	{
		auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress) {
			return false;
		}
		for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
			const char* dll = reinterpret_cast<const char*>(base + desc->Name);
			if (_stricmp(dll, "user32.dll") != 0) {
				continue;
			}
			auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
			auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
			for (; names->u1.AddressOfData; ++names, ++slots) {
				if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal) || std::strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name, "PeekMessageW") != 0) {
					continue;
				}
				auto* slot = reinterpret_cast<PeekMessage_t*>(&slots->u1.Function);
				DWORD old = 0;
				if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
					return false;
				}
				g_peekPrevious = *slot;
				*slot = &TickOnMessages;
				VirtualProtect(slot, sizeof(void*), old, &old);
				REX::INFO("the game's message check now runs the bridge every 100 ms on the game thread");
				return true;
			}
		}
		REX::WARN("the game imports no PeekMessageW, the message check cannot run the bridge");
		return false;
	}

	void OnMessage(OBSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg && a_msg->type == OBSE::MessagingInterface::kPostLoad) {
			HookInputTick();
			HookMessageTick();
		}
		if (a_msg && a_msg->type == OBSE::MessagingInterface::kDataLoaded) {
			g_gameThread = GetCurrentThreadId();
			g_dataReady = true;
			REX::INFO("the bridge ticks on game thread {}", g_gameThread);
			std::error_code ec;
			std::filesystem::remove(LinkDir() / L"box-open.txt", ec);
			Install();
			HookContainers();
			HookHudTick();
			HookPlayer();
			HookFlora();
			HookCreatures();
			HookPeople();
			HookPlayerUpdate();
			QuietQuests();
			TuneDeath();
			std::filesystem::remove(LinkDir() / L"died.txt", ec);
		}
	}
}

OBSE_PLUGIN_PRELOAD(const OBSE::PreLoadInterface* a_obse)
{
	OBSE::Init(a_obse);
	return true;
}

OBSE_PLUGIN_LOAD(const OBSE::LoadInterface* a_obse)
{
	OBSE::Init(a_obse);
	const auto messaging = OBSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnMessage)) {
		REX::ERROR("could not listen for OBSE messages");
		return false;
	}
	REX::INFO("RebornNative loaded, waiting for game data");
	return true;
}
