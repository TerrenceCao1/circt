/**
 * @file hw-estimate.cpp
 * @author Terrence Cao
 * @brief Lightweight tool to estimate the GE and FO4 of a potential hardware design
 * @details current usage: build/bin/hw-estimate <mlir-file-name>.mlir
 *
 */

/*
 * TODO:
 * -FO4 Analysis
 */

#include "CostModel.h"
#include "GEAnalysis.h"
#include "FO4Analysis.h"
#include "Options.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/InitAllDialects.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/InitAllDialects.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;

// --------------------------------------------------------------------------
// Tool options
// --------------------------------------------------------------------------
static llvm::cl::opt<std::string> inputFileName(
    llvm::cl::Positional, llvm::cl::desc("<input .mlir file>"),
    llvm::cl::init("-")
);

static llvm::cl::opt<std::string> inputCostModelJSON(
    "cost-model-file", llvm::cl::desc("<path to cost_model.json>"),
    llvm::cl::init("tools/hw-estimate/cost-model.json")
    );

static llvm::cl::opt<std::string> outputFileName(
    "o", llvm::cl::desc("Output filename"),
    llvm::cl::value_desc("filename"), llvm::cl::init("-")
    );

static llvm::cl::opt<std::string> outputErrorFileName(
    "error-file", llvm::cl::desc("Output Error filename"),
    llvm::cl::init("-")
    );

static llvm::cl::opt<bool> verbose(
    "v", llvm::cl::desc("Verbosely describing each and every thing that contributes to GE and FO4."),
    llvm::cl::init(false)
    );

llvm::cl::opt<bool> warn(
    "w", llvm::cl::desc("Verbosely describing every single warning that comes up during GE and FO4 analysis."),
    llvm::cl::init(false)
    );

static llvm::cl::opt<std::string> topModuleName(
    "top", llvm::cl::desc("Analyze only this hw.module (default: every module in the file)"),
    llvm::cl::value_desc("module name"), llvm::cl::init("")
    );

static llvm::cl::opt<bool> runGE(
    "ge", llvm::cl::desc("Perform Gate Equivalency Analysis"),
    llvm::cl::init(false)
    );

static llvm::cl::opt<bool> runFO4(
    "fo4", llvm::cl::desc("Run fanout-of-4 (critical path) analysis"),
    llvm::cl::init(false)
    );

// Returns the hw.module named `name`, or nullptr. Uses the generic symbol-name
// attribute so it does not depend on HWModuleOp accessor names in your CIRCT version.
static Operation *findTopModule(ModuleOp module, llvm::StringRef name)
{
  Operation *found = nullptr;
  module.walk([&](circt::hw::HWModuleOp m)
  {
    auto sym = m->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName());
    if(sym && sym.getValue() == name)
      found = m;
  });
  return found;
}

int main(int argc, char** argv)
{
  llvm::InitLLVM y(argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv, "hw-estimate\n");

  DialectRegistry registry;
  mlir::registerAllDialects(registry);
  circt::registerAllDialects(registry);

  MLIRContext context(registry);

  OwningOpRef<ModuleOp> module = parseSourceFile<ModuleOp>(inputFileName, &context);

  // Output Files
  std::error_code ECOutput, ECError;
  llvm::ToolOutputFile outputFile(outputFileName, ECOutput, llvm::sys::fs::OF_None);
  if(ECOutput)
  {
    llvm::errs() << "Error: couldn't open output file: " << ECOutput.message() << "\n";
    return 1;
  }

  llvm::ToolOutputFile errorFile(outputErrorFileName, ECError, llvm::sys::fs::OF_None);
  if(ECError)
  {
    llvm::errs() << "Error: couldn't open error file: " << ECError.message() << "\n";
  }

  if(!module)
  {
    errorFile.os() << "Failed to parse the input\n";
    return 1;
  }

  bool doGE  = runGE  || (!runGE && !runFO4);
  bool doFO4 = runFO4 || (!runGE && !runFO4);

  // Analysis root: the chosen top module, or the whole file if -top is not given.
  Operation *analysisRoot = *module;
  if(!topModuleName.empty())
  {
    analysisRoot = findTopModule(*module, topModuleName);
    if(!analysisRoot)
    {
      llvm::errs() << "Error: no hw.module named '" << topModuleName << "' in input\n";
      return 1;
    }
  }
  else
  {
    unsigned numModules = 0;
    module->walk([&](circt::hw::HWModuleOp) { numModules++; });
    if(numModules > 1)
      errorFile.os() << "Warning: " << numModules << " hw.module definitions in the file are ALL being "
                     << "counted. Use -top <name> to analyze a single design.\n";
  }

  CostModel costModel;

  if(doGE)
  {
    loadGECostModel(costModel, inputCostModelJSON);
    GEResult ge = runGEAnalysis(analysisRoot, costModel, outputFile.os(), errorFile.os(), verbose);
    printGEReport(ge, outputFile.os());
  }

  if(doFO4)
  {
    loadFO4CostModel(costModel, inputCostModelJSON);
    FO4Result fo4 = runFO4Analysis(analysisRoot, costModel, outputFile.os(), errorFile.os(), verbose);
    printFO4Report(fo4, outputFile.os());
  }

  outputFile.keep();
  errorFile.keep();
  return 0;
}
