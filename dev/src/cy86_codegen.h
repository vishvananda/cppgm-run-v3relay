#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "cy86_parse.h"
#include "x86_assembler.h"

struct Cy86Layout
{
	std::vector<std::size_t> statement_offsets;
	std::vector<std::size_t> statement_sizes;
	std::map<std::string, std::uint64_t> labels;
	std::uint64_t entry;
	std::size_t epilogue_offset;
	std::size_t body_size;

	Cy86Layout()
		: entry(0), epilogue_offset(0), body_size(0)
	{
	}
};

class Cy86ToX86Translator
{
public:
	std::vector<unsigned char> Translate(const Cy86Statement& statement,
		const std::map<std::string, std::uint64_t>& labels) const;

	static std::vector<unsigned char> Stub(std::uint64_t entry);
	static std::vector<unsigned char> Epilogue();
};

Cy86Layout BuildCy86Layout(const std::vector<Cy86Statement>& statements);
std::vector<unsigned char> BuildProgramImage(
	const std::vector<Cy86Statement>& statements);
