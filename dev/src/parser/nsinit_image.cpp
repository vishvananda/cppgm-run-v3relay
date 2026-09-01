#include "nsinit_image.h"

#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>

#include "nsinit_sema.h"

using namespace std;

namespace
{

void AppendBytes(vector<unsigned char>& image, const unsigned char* bytes,
	size_t size)
{
	image.insert(image.end(), bytes, bytes + size);
}

void AlignImage(vector<unsigned char>& image, size_t alignment)
{
	if (!alignment)
		throw runtime_error("invalid image alignment");
	const size_t padding = (alignment - image.size() % alignment) % alignment;
	image.insert(image.end(), padding, 0);
}

void StoreLittleEndian(vector<unsigned char>& image, size_t offset,
	uint64_t value)
{
	if (offset > image.size() || image.size() - offset < sizeof(value))
		throw runtime_error("relocation is outside the image");
	for (size_t i = 0; i < sizeof(value); ++i)
		image[offset + i] = static_cast<unsigned char>(value >> (i * 8));
}

void ApplyRelocations(vector<unsigned char>& image, size_t source_offset,
	const Pa8Value& value, const map<string, size_t>& addresses)
{
	for (size_t i = 0; i < value.relocs.size(); ++i)
	{
		const Pa8Relocation& relocation = value.relocs[i];
		map<string, size_t>::const_iterator target = addresses.find(
			relocation.symbol);
		// A symbol with no image object (a declared-but-undefined variable,
		// or a static_assert string literal) resolves to address 0, matching
		// the reference (probes p43/p59/p82).
		const int64_t address = target == addresses.end() ? 0 :
			static_cast<int64_t>(target->second);
		const int64_t relocated = address +
			static_cast<int64_t>(relocation.addend);
		StoreLittleEndian(image, source_offset + relocation.offset,
			static_cast<uint64_t>(relocated));
	}
}

} // namespace

Pa8ImageBuilder::Pa8ImageBuilder(
	const vector<shared_ptr<Pa7Namespace> >& globals)
	: globals_(globals)
{
}

vector<unsigned char> Pa8ImageBuilder::Build()
{
	Pa8ProgramSema sema(globals_);
	sema.Analyze();
	const vector<Pa8ProgramEntity>& entities = sema.Entities();
	vector<unsigned char> image;
	const unsigned char magic[] = {'P', 'A', '8', 0};
	AppendBytes(image, magic, sizeof(magic));
	map<string, size_t> addresses;

	for (size_t i = 0; i < entities.size(); ++i)
	{
		const Pa8ProgramEntity& entity = entities[i];
		if (!entity.is_function && !entity.defined)
			continue;
		if (entity.is_function)
		{
			AlignImage(image, 4);
			const size_t offset = image.size();
			const unsigned char stub[] = {'f', 'u', 'n', 0};
			AppendBytes(image, stub, sizeof(stub));
			addresses[entity.key] = offset;
			continue;
		}

		AlignImage(image, sema.TypeAlignment(entity.type));
		const size_t offset = image.size();
		const size_t size = sema.TypeSize(entity.type);
		if (entity.value.bytes.size() != size)
			throw runtime_error("initializer size does not match object type");
		image.insert(image.end(), entity.value.bytes.begin(),
			entity.value.bytes.end());
		addresses[entity.key] = offset;
	}

	for (size_t i = 0; i < sema.Temporaries().size(); ++i)
	{
		const Pa8Temporary& temporary = sema.Temporaries()[i];
		AlignImage(image, sema.TypeAlignment(temporary.type));
		const size_t offset = image.size();
		const size_t size = sema.TypeSize(temporary.type);
		if (temporary.value.bytes.size() != size)
			throw runtime_error("temporary size does not match object type");
		image.insert(image.end(), temporary.value.bytes.begin(),
			temporary.value.bytes.end());
		addresses[temporary.symbol] = offset;
	}

	for (size_t i = 0; i < sema.Strings().size(); ++i)
	{
		const Pa8StringLiteral& literal = sema.Strings()[i];
		AlignImage(image, literal.alignment);
		addresses[literal.symbol] = image.size();
		image.insert(image.end(), literal.bytes.begin(), literal.bytes.end());
	}

	for (size_t i = 0; i < entities.size(); ++i)
	{
		const Pa8ProgramEntity& entity = entities[i];
		if (entity.is_function || !entity.defined)
			continue;
		map<string, size_t>::const_iterator source = addresses.find(entity.key);
		if (source == addresses.end())
			throw runtime_error("object address was not assigned");
		ApplyRelocations(image, source->second, entity.value, addresses);
	}
	for (size_t i = 0; i < sema.Temporaries().size(); ++i)
	{
		const Pa8Temporary& temporary = sema.Temporaries()[i];
		map<string, size_t>::const_iterator source = addresses.find(
			temporary.symbol);
		if (source == addresses.end())
			throw runtime_error("temporary address was not assigned");
		ApplyRelocations(image, source->second, temporary.value, addresses);
	}
	return image;
}
