# circt/tools/hw-estimate/CMakeLists.txt
set(LLVM_LINK_COMPONENTS Support)

get_property(mlir_dialect_libs GLOBAL PROPERTY MLIR_DIALECT_LIBS)
get_property(circt_dialect_libs GLOBAL PROPERTY CIRCT_DIALECT_LIBS)

add_circt_tool(hw-estimate
  src/hw-estimate.cpp
  src/CostModel.cpp
  src/OpClassify.cpp
  src/GEAnalysis.cpp
  src/FO4Analysis.cpp
)

target_include_directories(hw-estimate
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_link_libraries(hw-estimate
  PRIVATE
  ${mlir_dialect_libs}
  ${circt_dialect_libs}
  MLIRIR
  MLIRParser
  MLIRSupport
  MLIRRegisterAllDialects
)
