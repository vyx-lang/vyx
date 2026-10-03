#pragma once

#include "Common/DependencyGraph.h"
#include "Common/CompileCache.h"
#include "ImportResolver.h"

#include <expected>
#include <string>

namespace vyx {

std::expected<void, std::string> compileFile(const std::string& inputFile,
                                              const std::string& outputObj,
                                              bool isEntry);

} // namespace vyx
