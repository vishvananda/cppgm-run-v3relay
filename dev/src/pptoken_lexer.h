#pragma once

#include <cstddef>
#include <string>

struct IPPTokenStream;

struct IPPTokenPositionSink
{
	virtual void on_token_line(std::size_t physical_line) = 0;
	virtual ~IPPTokenPositionSink() {}
};

void PPTokenize(const std::string& input, IPPTokenStream& output);
void PPTokenize(const std::string& input, IPPTokenStream& output,
	IPPTokenPositionSink* position_sink);
