#include <Windows.h>
#undef ERROR

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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

	void Change(RE::TESObjectREFR* a_ref, std::uint32_t a_form, std::int32_t a_by)
	{
		auto* object = ItemFor(a_form);
		if (!object || a_by == 0) {
			return;
		}
		if (a_by > 0) {
			a_ref->AddObjectToContainer(object, nullptr, a_by);
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
		WriteAtomic(LinkDir() / L"box-open.txt", Hex(ref) + '\n');
		Note("open " + Hex(ref));
		return result;
	}

	void HookContainers()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::TESObjectCONT[0] };
		g_containerActivate = reinterpret_cast<Activate_t>(vtbl.write_vfunc(0x33, &ContainerActivate));
		REX::INFO("container activation hooked");
	}

	bool g_live = false;
	std::string g_lastInv;

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

	void WatchPlayer()
	{
		if (!g_live) {
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
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

	void PlayerAddBound(RE::PlayerCharacter* a_self, RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra, std::int32_t a_number)
	{
		g_addBound(a_self, a_object, a_extra, a_number);
		Changed("add");
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
				Note("eat " + Hex(a_object->GetFormID()) + ' ' + std::to_string(a_number));
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

	void HookPlayer()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::PlayerCharacter[0] };
		g_addBound = reinterpret_cast<AddBound_t>(vtbl.write_vfunc(0x45, &PlayerAddBound));
		g_remove = reinterpret_cast<Remove_t>(vtbl.write_vfunc(0x46, &PlayerRemove));
		g_addWorn = reinterpret_cast<AddWorn_t>(vtbl.write_vfunc(0x48, &PlayerAddWorn));
		g_removeWorn = reinterpret_cast<RemoveWorn_t>(vtbl.write_vfunc(0x49, &PlayerRemoveWorn));
		g_addToContainer = reinterpret_cast<AddBound_t>(vtbl.write_vfunc(0x4B, &PlayerAddToContainer));
		g_healthDamage = reinterpret_cast<HealthDamage_t>(vtbl.write_vfunc(0x111, &PlayerHealthDamage));
		REX::INFO("player inventory changes hooked");
	}

	constexpr std::uint64_t kDespawnMs = 30000;
	constexpr std::uint64_t kSweepEveryMs = 5000;
	std::map<std::uint32_t, std::uint64_t> g_lyingSince;
	std::uint64_t g_lastSweep = 0;

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
		for (auto* ref : cell->listReferences) {
			if (!ref || ref->IsDeleted() || (ref->GetFormFlags() & RE::TESForm::RecordFlags::kDisabled)) {
				continue;
			}
			if (!DeadBody(ref, player)) {
				continue;
			}
			const auto id = ref->GetFormID();
			const auto it = g_lyingSince.find(id);
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
		std::map<std::uint32_t, std::string> nodes;
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
		REX::INFO("profession file loaded: {} ({} nodes)", g_job.id.empty() ? "none" : g_job.id, g_job.nodes.size());
	}

	using FloraActivate_t = bool (*)(RE::TESFlora*, RE::TESObjectREFR*, RE::TESObjectREFR*, bool, RE::TESBoundObject*, std::int32_t);
	FloraActivate_t g_floraActivate = nullptr;

	bool FloraActivate(RE::TESFlora* a_self, RE::TESObjectREFR* a_target, RE::TESObjectREFR* a_activator, bool a_idFlag, RE::TESBoundObject* a_object, std::int32_t a_count)
	{
		const bool byPlayer = a_target && a_activator == RE::PlayerCharacter::GetSingleton();
		if (byPlayer && !g_silentPick && a_self) {
			LoadJob();
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
		Guard(RE::MENU_CLASS::kEnchantmentMenu, g_job.id == "enchanter", "enchant");
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
			return;
		}
		if (verb == "state") {
			ReportState(a_out);
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
			if (player && amount > 0 && g_healthDamage && !PlayerDead()) {
				g_healthDamage(player, nullptr, amount);
			}
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

	bool Execute(const RE::SCRIPT_PARAMETER*, const char*, RE::TESObjectREFR*, RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*, double& a_result, std::uint32_t&)
	{
		a_result = 0;
		try {
			CheckClosed();
			RunRequests();
			RunWorld();
			WatchDrops();
			WatchPlayer();
			Sweep();
			GuardMenus();
			WatchRide();
		} catch (const std::exception& e) {
			REX::ERROR("native bridge failed: {}", e.what());
		}
		a_result = 1;
		return true;
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

	void OnMessage(OBSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg && a_msg->type == OBSE::MessagingInterface::kDataLoaded) {
			std::error_code ec;
			std::filesystem::remove(LinkDir() / L"box-open.txt", ec);
			Install();
			HookContainers();
			HookPlayer();
			HookFlora();
			HookCreatures();
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
