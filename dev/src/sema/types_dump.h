#pragma once

#include <cstddef>
#include <iosfwd>

#include "sema/scope_model.h"

void PrintTypesUnit(std::ostream& out, std::size_t unit,
                    const SemaModel& model);
