#pragma once

#include <cstddef>
#include <string>

// Return whether codepoint is a Unicode scalar value.
bool IsUnicodeScalarValue(long long codepoint);

// Decode one UTF-8 code point at offset.  The returned value is -1 when
// offset is at or past the end of input; malformed UTF-8 throws.
int DecodeUtf8At(const std::string& input, std::size_t offset,
	std::size_t& end);

// Encode one Unicode scalar value as UTF-8.  Invalid values throw.
std::string EncodeUtf8(int codepoint);
