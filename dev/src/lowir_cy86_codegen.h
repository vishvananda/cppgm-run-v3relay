#pragma once

#include <string>

#include "lowir_model.h"
#include "lowir_validate.h"

std::string EmitCy86Program(const lowir_model::Program & program,
                            const LowirProgramFacts & facts);
