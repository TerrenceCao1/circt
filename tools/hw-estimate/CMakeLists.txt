# circt/tools/hw-estimate/CMakeLists.txt
set(LLVM_LINK_COMPONENTS Support)

get_property(mlir_dialect_libs GLOBAL PROPERTY MLIR_DIALECT_LIBS)
get_property(circt_dialect_libs GLOBAL PROPERTY CIRCT_DIALECT_LIBS)

add_circt_tool(hw-estimate
  hw-estimate.cpp
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
