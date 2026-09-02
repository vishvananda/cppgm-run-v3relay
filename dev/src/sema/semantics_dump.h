#pragma once

#include <cstddef>
#include <iosfwd>
#include <vector>

#include "parser/recog_token.h"
#include "sema/sema_tree.h"

void PrintSemanticsUnit(std::ostream& out, std::size_t unit,
                        const SemaTree& tree, const SemaModel& model,
                        const std::vector<Pa6Token>& tokens);
