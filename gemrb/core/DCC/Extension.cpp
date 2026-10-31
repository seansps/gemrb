// SPDX-FileCopyrightText: 2026 Contributors to the GemRB project <https://gemrb.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DCC/Extension.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace GemRB {
namespace DCC {

namespace {

constexpr char MAGIC[4] = { 'D', 'C', 'C', 'X' };
constexpr char TAG_SPELLS[4] = { 'S', 'P', 'E', 'L' };
constexpr char TAG_CORRUPTION[4] = { 'C', 'O', 'R', 'R' };
constexpr char TAG_SPELLBURN[4] = { 'B', 'U', 'R', 'N' };
constexpr char TAG_DISAPPROVAL[4] = { 'D', 'I', 'S', 'A' };
constexpr size_t RESREF_SIZE = 8;

class Out {
public:
	std::vector<uint8_t> bytes;
	void U8(uint8_t v) { bytes.push_back(v); }
	void U16(uint16_t v)
	{
		U8(uint8_t(v & 0xff));
		U8(uint8_t(v >> 8));
	}
	void U32(uint32_t v)
	{
		U16(uint16_t(v & 0xffff));
		U16(uint16_t(v >> 16));
	}
	void Tag(const char* tag) { bytes.insert(bytes.end(), tag, tag + 4); }
	void ResRef(const std::string& name)
	{
		for (size_t i = 0; i < RESREF_SIZE; ++i) {
			U8(i < name.size() ? uint8_t(name[i]) : 0);
		}
	}
	void Patch32(size_t at, uint32_t v)
	{
		for (int i = 0; i < 4; ++i) {
			bytes[at + i] = uint8_t((v >> (8 * i)) & 0xff);
		}
	}
};

class In {
	const uint8_t* data;
	size_t size;
	size_t pos = 0;
	bool ok = true;

public:
	In(const uint8_t* data, size_t size)
		: data(data), size(size) {}
	bool Need(size_t n)
	{
		if (!ok || size - pos < n) ok = false;
		return ok;
	}
	uint8_t U8() { return Need(1) ? data[pos++] : 0; }
	uint16_t U16()
	{
		uint16_t lo = U8();
		return uint16_t(lo | (U8() << 8));
	}
	uint32_t U32()
	{
		uint32_t lo = U16();
		return lo | (uint32_t(U16()) << 16);
	}
	std::string ResRef()
	{
		std::string name;
		for (size_t i = 0; i < RESREF_SIZE; ++i) {
			char c = char(U8());
			if (c) name.push_back(char(std::tolower(static_cast<unsigned char>(c))));
		}
		return name;
	}
	bool Ok() const { return ok; }
	bool AtEnd() const { return ok && pos == size; }
};

// a record: tag, version, flags, payload size (patched), payload
size_t BeginRecord(Out& out, const char* tag, uint16_t version)
{
	out.Tag(tag);
	out.U16(version);
	out.U16(0);
	size_t sizeAt = out.bytes.size();
	out.U32(0);
	return sizeAt;
}

void EndRecord(Out& out, size_t sizeAt)
{
	out.Patch32(sizeAt, uint32_t(out.bytes.size() - sizeAt - 4));
}

void WriteSpells(Out& out, const std::vector<KnownSpell>& spells)
{
	out.U16(uint16_t(spells.size()));
	for (const auto& spell : spells) {
		out.ResRef(spell.spell);
		out.U8(spell.flags);
		out.U8(spell.manifestation);
		out.U8(uint8_t(std::min<size_t>(spell.mercurial.size(), 255)));
		out.U8(spell.mercurialSub);
		out.U8(uint8_t(spell.luckyMod));
		out.U8(spell.streak);
		out.U16(spell.casts);
		for (size_t i = 0; i < spell.mercurial.size() && i < 255; ++i) {
			out.U8(spell.mercurial[i]);
		}
	}
}

bool ReadSpells(In& in, std::vector<KnownSpell>& spells)
{
	uint16_t count = in.U16();
	for (uint16_t i = 0; i < count && in.Ok(); ++i) {
		KnownSpell spell;
		spell.spell = in.ResRef();
		spell.flags = in.U8();
		spell.manifestation = in.U8();
		uint8_t mercurial = in.U8();
		spell.mercurialSub = in.U8();
		spell.luckyMod = int8_t(in.U8());
		spell.streak = in.U8();
		spell.casts = in.U16();
		for (uint8_t m = 0; m < mercurial; ++m) {
			spell.mercurial.push_back(in.U8());
		}
		spells.push_back(std::move(spell));
	}
	return in.Ok();
}

void WriteCorruptions(Out& out, const std::vector<Corruption>& list)
{
	out.U16(uint16_t(list.size()));
	for (const auto& corruption : list) {
		out.U8(uint8_t(corruption.table));
		out.U8(corruption.row);
		out.U8(corruption.sub);
		out.U8(corruption.flags);
		out.ResRef(corruption.spell);
	}
}

bool ReadCorruptions(In& in, std::vector<Corruption>& list)
{
	uint16_t count = in.U16();
	for (uint16_t i = 0; i < count && in.Ok(); ++i) {
		Corruption corruption;
		uint8_t table = in.U8();
		if (table >= uint8_t(CorruptionTable::count)) return false;
		corruption.table = CorruptionTable(table);
		corruption.row = in.U8();
		corruption.sub = in.U8();
		corruption.flags = in.U8();
		corruption.spell = in.ResRef();
		list.push_back(std::move(corruption));
	}
	return in.Ok();
}

}

bool CharacterExt::Empty() const
{
	return spells.empty() && corruptions.empty() && burn.IsDefault() &&
		disapproval.range <= 1 && disapproval.flags == 0 && other.empty();
}

KnownSpell* CharacterExt::FindSpell(const std::string& resref)
{
	for (auto& spell : spells) {
		if (spell.spell.size() == resref.size() && std::equal(spell.spell.begin(), spell.spell.end(), resref.begin(), [](char a, char b) {
			    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
		    })) {
			return &spell;
		}
	}
	return nullptr;
}

const KnownSpell* CharacterExt::FindSpell(const std::string& resref) const
{
	return const_cast<CharacterExt*>(this)->FindSpell(resref);
}

bool CharacterExt::operator==(const CharacterExt& rhs) const
{
	return WriteExtension(*this) == WriteExtension(rhs);
}

std::vector<uint8_t> WriteExtension(const CharacterExt& ext)
{
	Out out;
	out.Tag(MAGIC);
	out.U16(EXT_VERSION);
	size_t countAt = out.bytes.size();
	out.U16(0);
	out.U32(0);
	uint16_t records = 0;

	if (!ext.spells.empty()) {
		size_t at = BeginRecord(out, TAG_SPELLS, 1);
		WriteSpells(out, ext.spells);
		EndRecord(out, at);
		++records;
	}
	if (!ext.corruptions.empty()) {
		size_t at = BeginRecord(out, TAG_CORRUPTION, 1);
		WriteCorruptions(out, ext.corruptions);
		EndRecord(out, at);
		++records;
	}
	if (!ext.burn.IsDefault()) {
		size_t at = BeginRecord(out, TAG_SPELLBURN, 1);
		for (uint8_t points : ext.burn.points) {
			out.U8(points);
		}
		out.U8(ext.burn.flavour);
		out.U32(ext.burn.lastBurnDay);
		out.U8(uint8_t(ext.burn.policy));
		out.U8(ext.burn.preset);
		out.U8(uint8_t(ext.burn.corruption));
		EndRecord(out, at);
		++records;
	}
	if (ext.disapproval.range > 1 || ext.disapproval.flags) {
		size_t at = BeginRecord(out, TAG_DISAPPROVAL, 1);
		out.U8(ext.disapproval.range);
		out.U8(ext.disapproval.flags);
		out.U16(0);
		EndRecord(out, at);
		++records;
	}
	for (const auto& raw : ext.other) {
		size_t at = BeginRecord(out, raw.tag.data(), raw.version);
		out.bytes.insert(out.bytes.end(), raw.data.begin(), raw.data.end());
		EndRecord(out, at);
		++records;
	}

	out.bytes[countAt] = uint8_t(records & 0xff);
	out.bytes[countAt + 1] = uint8_t(records >> 8);
	out.Patch32(countAt + 2, uint32_t(out.bytes.size()));
	return out.bytes;
}

size_t ExtensionSize(const uint8_t* data, size_t size)
{
	if (!data || size < EXT_HEADER_SIZE || std::memcmp(data, MAGIC, 4) != 0) return 0;
	size_t total = size_t(data[8]) | (size_t(data[9]) << 8) | (size_t(data[10]) << 16) | (size_t(data[11]) << 24);
	if (total < EXT_HEADER_SIZE || total > EXT_MAX_SIZE) return 0;
	return total;
}

bool ReadExtension(const uint8_t* data, size_t size, CharacterExt& out)
{
	size_t total = ExtensionSize(data, size);
	if (!total || total > size) return false;
	In in(data, total);
	in.U32(); // the magic
	uint16_t version = in.U16();
	if (version < 1) return false;
	uint16_t records = in.U16();
	in.U32(); // the size

	CharacterExt ext;
	for (uint16_t r = 0; r < records; ++r) {
		if (!in.Need(EXT_RECORD_HEADER_SIZE)) return false;
		std::array<char, 4> tag {};
		for (char& c : tag) {
			c = char(in.U8());
		}
		uint16_t recordVersion = in.U16();
		in.U16(); // flags
		uint32_t length = in.U32();
		if (!in.Need(length)) return false;
		// the payload is the next length bytes: read them through a reader of their own
		std::vector<uint8_t> bytes(length);
		for (uint32_t i = 0; i < length; ++i) {
			bytes[i] = in.U8();
		}
		In body(bytes.data(), bytes.size());
		if (std::memcmp(tag.data(), TAG_SPELLS, 4) == 0 && recordVersion >= 1) {
			if (!ReadSpells(body, ext.spells)) return false;
		} else if (std::memcmp(tag.data(), TAG_CORRUPTION, 4) == 0 && recordVersion >= 1) {
			if (!ReadCorruptions(body, ext.corruptions)) return false;
		} else if (std::memcmp(tag.data(), TAG_SPELLBURN, 4) == 0 && recordVersion >= 1) {
			for (uint8_t& points : ext.burn.points) {
				points = body.U8();
			}
			ext.burn.flavour = body.U8();
			ext.burn.lastBurnDay = body.U32();
			ext.burn.policy = SpellburnPolicy(std::min<uint8_t>(body.U8(), uint8_t(SpellburnPolicy::count) - 1));
			ext.burn.preset = body.U8();
			ext.burn.corruption = CorruptionBurn(std::min<uint8_t>(body.U8(), uint8_t(CorruptionBurn::count) - 1));
			if (!body.Ok()) return false;
		} else if (std::memcmp(tag.data(), TAG_DISAPPROVAL, 4) == 0 && recordVersion >= 1) {
			ext.disapproval.range = std::max<uint8_t>(1, body.U8());
			ext.disapproval.flags = body.U8();
			if (!body.Ok()) return false;
		} else {
			RawRecord raw;
			raw.tag = tag;
			raw.version = recordVersion;
			raw.data = std::move(bytes);
			ext.other.push_back(std::move(raw));
		}
	}
	if (!in.Ok()) return false;
	out = std::move(ext);
	return true;
}

}
}
