#pragma once

#include <memory>
#include <string>
#include <vector>

#include "nsdecl_model.h"

class Pa8ImageBuilder
{
public:
	Pa8ImageBuilder(
		const std::vector<std::shared_ptr<Pa7Namespace> >& globals);

	std::vector<unsigned char> Build();

private:
	const std::vector<std::shared_ptr<Pa7Namespace> >& globals_;
};
