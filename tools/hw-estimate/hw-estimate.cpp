/**
 * @file hw-estimate.cpp
 * @author Terrence Cao
 * @brief Lightweight tool to estimate the GE and FO4 of a potential hardware design
 * @details current usage: build/bin/circt-verilog <verilog file> | build/bin/hw-estimate -
 *
 */

#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/Comb/CombDialect.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqDialect.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace mlir;

// Getting input file from command line
static llvm::cl::opt<std::string> inputFileName(
    llvm::cl::Positional, llvm::cl::desc("<input .mlir file>"),
    llvm::cl::init("-")
);

// Getting the cost model JSON from the command line
static llvm::cl::opt<std::string> inputCostModelJSON(
    llvm::cl::Positional, llvm::cl::desc("<path to cost_model.json>"),
    llvm::cl::init("tools/hw-estimate/cost-model.json")
    );

namespace
{
  struct GECost
  {
    double gePerBit = 0.0;
    double fixedGE = 0.0;
  };
} // End of anonymous namespace

static llvm::StringMap<GECost> loadCostModel(llvm::StringRef path)
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

  return model;
}


int main(int argc, char** argv)
{
  llvm::InitLLVM y(argc, argv); // taking the LLVM output from the command line
  llvm::cl::ParseCommandLineOptions(argc, argv, "hw-estimate\n");

  DialectRegistry registry;
  registry.insert<circt::hw::HWDialect, circt::comb::CombDialect, circt::seq::SeqDialect>();

  MLIRContext context(registry);

  OwningOpRef<ModuleOp> module = parseSourceFile<ModuleOp>(inputFileName, &context);

  if(!module)
  {
    llvm::errs() << "Failed to parse the input\n";
    return 1;
  }

  llvm::StringMap<GECost> costModel = loadCostModel(inputCostModelJSON);
  double totalGE = 0.0;

  module->walk([&](Operation *op)
  {
    llvm::StringRef opName = op->getName().getStringRef();

    auto it = costModel.find(opName);
    if(it == costModel.end())
    {
      llvm::errs() << "Warning: No cost entry for '" << opName << "', skipping\n";
      return;
    }

    unsigned width = 0;
    if(op->getNumResults() > 0)
    {
      if(auto intTy = dyn_cast<IntegerType>(op->getResult(0).getType()))
        width = intTy.getWidth();
    }

    double ge = it->second.gePerBit * width + it->second.fixedGE;
    totalGE += ge;

    llvm::outs() << opName << " (width " << width << "): " << ge << " GE\n";
  });

  llvm::outs() << "--------\n" <<
                  "Total logic GE: " << totalGE << "\n";

  return 0;
}
