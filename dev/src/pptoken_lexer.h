#pragma once

#include <string>

struct IPPTokenStream;

void PPTokenize(const std::string& input, IPPTokenStream& output);
