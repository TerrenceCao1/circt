/**
 * @file hw-estimate.cpp
 * @author Terrence Cao
 * @brief Lightweight tool to estimate the GE and FO4 of a potential hardware design
 * @details current usage: build/bin/hw-estimate <mlir-file-name>.mlir
 *
 */

/*
 * TODO:
 * -Add an array size threshold to differentiate if a firreg should count for FF or Sram
 * -FO4 Analysis also
 */

#include "circt/Dialect/HW/HWTypes.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Seq/SeqTypes.h"
#include "mlir/InitAllDialects.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "circt/InitAllDialects.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/ToolOutputFile.h"

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
    "v", llvm::cl::desc("Verbosely describing each and every thing that contributes to GE"),
    llvm::cl::init(false)
    );

namespace
{
  struct GECost
  {
    double gePerBit{};
    double fixedGE{};
  };
} // End of anonymous namespace

static llvm::StringMap<GECost> loadCostModel(llvm::StringRef path, double &ffGEPerBit, double &sramGEPerBit, double &sramFixedGE)
{
  // Map from string to GECost struct of each operation (ex. "comb.add" : [1.5, 0], "comb.or" : ...)
  llvm::StringMap<GECost>model;

  auto bufferOrErr = llvm::MemoryBuffer::getFile(path);
  if(!bufferOrErr)
  {
    llvm::errs() << "Error: couldn't open cost model file: " << path << "\n";
    exit(1);
  }

  llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(bufferOrErr.get()->getBuffer());
  if(!parsed)
  {
    llvm::errs() << "Error: failed to parse JSON File: " << llvm::toString(parsed.takeError()) << "\n";
    exit(1);
  }

  llvm::json::Object *root = parsed->getAsObject(); // root is an object of the entire JSON file
  llvm::json::Object *logicGE = root->getObject("logic_ge"); // logicGE is the object of just the logic_ge parts
  if(!logicGE)
  {
    llvm::errs() << "Error: cost model is missing 'logic_ge' object\n";
    exit(1);
  }

  // For each key value in the JSON file, we place it into the model map in the form <string, GECost>
  for(auto &kv : *logicGE)
  {
    llvm::json::Object *entry = kv.second.getAsObject();
    if(!entry) continue;

    GECost cost;
    if(auto v = entry->getNumber("ge_per_bit"))
      cost.gePerBit = *v;
    if(auto v = entry->getNumber("fixed_ge"))
      cost.fixedGE = *v;

    model[kv.first] = cost;
  }

  // load cost for flip flops
  if(auto v = root->getNumber("ff_ge_per_bit"))
    ffGEPerBit = *v;
  else
    llvm::errs() << "Warning: cost model is missing 'ff_ge_per_bit', defaulting to 0\n";

  // load cost for SRAM bits
  if(auto v = root->getNumber("sram_ge_per_bit"))
    sramGEPerBit = *v;
  else
    llvm::errs() << "Warning cost mosdel is missing 'sram_ge_per_bit', defraulting to 0\n";

  if(auto v = root->getNumber("sram_fixed_ge"))
    sramFixedGE = *v;

  return model;
}

// Recursive helper for getting the total number of flip flop bits
static uint64_t getTotalBits(Type type)
{
  if(auto intTy = dyn_cast<IntegerType>(type))
    return intTy.getWidth();

  if(auto arrTy = dyn_cast<circt::hw::ArrayType>(type))
    return arrTy.getNumElements() * getTotalBits(arrTy.getElementType());

  return 0;
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


  double ffGEPerBit{}, sramGEPerBit{}, sramFixedGE{};

  llvm::StringMap<GECost> costModel = loadCostModel(inputCostModelJSON, ffGEPerBit, sramGEPerBit, sramFixedGE);

  double totalLogicGE{};

  // macros is how many arrays classify as SRAM
  long long totalFFBits{}, totalSramBits{}, totalSramMacros{};

  module->walk([&](Operation *op)
  {
    llvm::StringRef opName = op->getName().getStringRef();

    // Flip Flops
    if(auto firreg = dyn_cast<circt::seq::FirRegOp>(op))
    {
      uint64_t bits = getTotalBits(firreg.getType());
      if(bits == 0)
      {
        errorFile.os() << "Warning: seq.firreg with unrecognized type. 0 FF bits counted\n";
        return;
      }

      totalFFBits += bits;

      if(verbose)
        outputFile.os() << "FF: " << firreg.getName() << " (" << bits << " bits)\n";

      return;
    }

    // SRAM
    if(auto firmem = dyn_cast<circt::seq::FirMemOp>(op))
    {
      auto memType = dyn_cast<circt::seq::FirMemType>(firmem.getType());
      if(!memType)
      {
        errorFile.os() << "Warning: seq.firmem with unrecognized type, 0 SRAM bits counted\n";
        return;
      }

      uint64_t depth = memType.getDepth();
      uint64_t width = memType.getWidth();
      uint64_t bits  = depth * width;

      totalSramBits += bits;
      totalSramMacros++;

      if(verbose)
        outputFile.os() << "SRAM: " << firmem.getName() << " (" << depth << " x " << width << " = " << bits << " bits)\n";

      return;
    }

    auto it = costModel.find(opName);
    if(it == costModel.end())
    {
      errorFile.os() << "Warning: No cost entry for '" << opName << "', skipping\n";
      return;
    }

    unsigned width{};
    if(op->getNumResults() > 0)
    {
     if(auto intTy = dyn_cast<IntegerType>(op->getResult(0).getType()))
        width = intTy.getWidth();
    }

    double ge = it->second.gePerBit * width + it->second.fixedGE;
    totalLogicGE += ge;
    if(verbose)
      outputFile.os() << opName << " (width " << width << "): " << llvm::format("%.2f", ge) << " GE\n";
  });

  double totalFFGE = totalFFBits * ffGEPerBit;
  double totalSramGE = totalSramBits * sramGEPerBit + totalSramMacros * sramFixedGE;

  outputFile.os() << "--------\n"
                  << "Logic GE: " << llvm::format("%.2f", totalLogicGE)<< "\n"
                  << "FF GE:    " << llvm::format("%.2f", totalFFGE) << " (" << totalFFBits << " bits)\n"
                  << "SRAM GE:  " << llvm::format("%.2f", totalSramGE) << " (" << totalSramBits << " bits in " << totalSramMacros << " macro(s))\n"
                  << "Total GE: " << llvm:: format("%.2f", totalLogicGE + totalFFGE + totalSramGE) << "\n";

  outputFile.keep();
  errorFile.keep();
  return 0;
}
