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

#include <cmath>
#include <unordered_map>
#include "circt/Dialect/Comb/CombDialect.h"
#include "circt/Dialect/HW/HWTypes.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Seq/SeqTypes.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/InitAllDialects.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "circt/InitAllDialects.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
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
    "v", llvm::cl::desc("Verbosely describing each and every thing that contributes to GE"),
    llvm::cl::init(false)
    );

static llvm::cl::opt<bool> runGE(
    "ge", llvm::cl::desc("Perform Gate Equivalency Analysis"),
    llvm::cl::init(false)
    );

static llvm::cl::opt<bool> runFO4(
    "fo4", llvm::cl::desc("Run fanout-of-4 (critical path) analysis"),
    llvm::cl::init(false)
    );

// --------------------------------------------------------------------------
// Helper Structs
// --------------------------------------------------------------------------
namespace
{
  struct GECost
  {
    double gePerBit{};
    double fixedGE{};
  };

  struct GEResult
  {
    double totalLogicGE{};
    double totalFFGE{};
    double totalSramGE{};
    long long totalFFBits{};
    long long totalSramBits{};
    long long totalSramMacros{};
  };

  struct FO4Cost
  {
    bool hasLog2Model{false};
    double log2Coeff{};
    double log2Const{};
    double fixed{};
    double perBit{};
  };

  struct FO4Result
  {
    double          criticalFO4{};
    long long       numTimingPaths{};
    std::string     criticalSinkName;
  };

  struct CostModel
  {
    llvm::StringMap<GECost> logicGE;
    llvm::StringMap<FO4Cost>  delayFO4;
    llvm::StringMap<std::string> icmpPredicateGroup;
    double ffGEPerBit{};
    double sramGEPerBit{};
    double sramFixedGE{};
  };
} // End of anonymous namespace

// --------------------------------------------------------------------------
// Running GE Analysis
// --------------------------------------------------------------------------
static void loadGECostModel(CostModel& model, llvm::StringRef path)
{
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

  llvm::json::Object *logicGE = root->getObject("logic_ge");

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

    model.logicGE[kv.first] = cost;
  }

  // load cost for flip flops
  if(auto v = root->getNumber("ff_ge_per_bit"))
    model.ffGEPerBit = *v;
  else
    llvm::errs() << "Warning: cost model is missing 'ff_ge_per_bit', defaulting to 0\n";

  // load cost for SRAM bits
  if(auto v = root->getNumber("sram_ge_per_bit"))
    model.sramGEPerBit = *v;
  else
    llvm::errs() << "Warning cost mosdel is missing 'sram_ge_per_bit', defraulting to 0\n";

  if(auto v = root->getNumber("sram_fixed_ge"))
    model.sramFixedGE = *v;
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

static GEResult runGEAnalysis(ModuleOp module, const CostModel &costModel, llvm::raw_ostream &out,
                              llvm::raw_ostream &err, bool verbose)
{
  GEResult result;
  std::unordered_map<std::string, unsigned> missingOperationsTable;

  module.walk([&](Operation *op)
  {
    llvm::StringRef opName = op->getName().getStringRef();

    if(auto firreg = dyn_cast<circt::seq::FirRegOp>(op))
    {
      uint64_t bits = getTotalBits(firreg.getType());
      if(bits == 0)
      {
        err << "Warning: seq.firreg with unrecognized type. 0 FF bits counted\n";
        return;
      }

      result.totalFFBits += bits;

      if(verbose)
        out << "FF: " << firreg.getName() << " (" << bits << " bits)\n";

      return;
    }

    // SRAM
    if(auto firmem = dyn_cast<circt::seq::FirMemOp>(op))
    {
      auto memType = dyn_cast<circt::seq::FirMemType>(firmem.getType());
      if(!memType)
      {
        err << "Warning: seq.firmem with unrecognized type, 0 SRAM bits counted\n";
        return;
      }

      uint64_t depth = memType.getDepth();
      uint64_t width = memType.getWidth();
      uint64_t bits  = depth * width;

      result.totalSramBits += bits;
      result.totalSramMacros++;

      if(verbose)
        out << "SRAM: " << firmem.getName() << " (" << depth << " x " << width << " = " << bits << " bits)\n";

      return;
    }

    auto it = costModel.logicGE.find(opName);
    if(it == costModel.logicGE.end())
    {
      missingOperationsTable[opName.str()]++;
      return;
    }

    unsigned width{};
    if(op->getNumResults() > 0)
    {
     if(auto intTy = dyn_cast<IntegerType>(op->getResult(0).getType()))
        width = intTy.getWidth();
    }

    double ge = it->second.gePerBit * width + it->second.fixedGE;
    result.totalLogicGE += ge;
    if(verbose)
      out << opName << " (width " << width << "): " << llvm::format("%.2f", ge) << " GE\n";
  });

  for(const auto& [key, value] : missingOperationsTable)
  {
    out << "Warning: No GE entry for '" << key << "', assuming 0 GE for " << value << " instances\n";
  }

  result.totalFFGE = result.totalFFBits * costModel.ffGEPerBit;
  result.totalSramGE = result.totalSramBits * costModel.sramGEPerBit + result.totalSramMacros * costModel.sramFixedGE;

  return result;
}

static void printGEReport(const GEResult &r, llvm::raw_ostream &out)
{

  out << "--------\n"
      << "Logic GE: " << llvm::format("%.2f", r.totalLogicGE)<< "\n"
      << "FF GE:    " << llvm::format("%.2f", r.totalFFGE) << " (" << r.totalFFBits << " bits)\n"
      << "SRAM GE:  " << llvm::format("%.2f", r.totalSramGE) << " (" << r.totalSramBits << " bits in " << r.totalSramMacros << " macro(s))\n"
      << "Total GE: " << llvm:: format("%.2f", r.totalLogicGE + r.totalFFGE + r.totalSramGE) << "\n\n";
}

// --------------------------------------------------------------------------
// Running FO4 Analysis
// --------------------------------------------------------------------------
static FO4Cost parseFO4Entry(llvm::json::Object *entry)
{
  FO4Cost cost;

  if(auto v = entry->getNumber("fo4"))
    cost.fixed = *v;
  if(auto v = entry->getNumber("fo4_fixed"))
    cost.fixed = *v;
  if(auto v = entry->getNumber("fo4_per_bit"))
    cost.perBit = *v;

  if(auto v = entry->getNumber("fo4_log2_coeff"))
  {
    cost.log2Coeff = *v;
    cost.hasLog2Model = true;
  }
  if(auto v = entry->getNumber("fo4_log2_const"))
    cost.log2Const = *v;

  return cost;
}

static void loadFO4CostModel(CostModel& model, llvm::StringRef path)
{
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

    llvm::json::Object *fo4Delays = root->getObject("delay_fo4");

    if(!fo4Delays)
    {
      llvm::errs() << "Error: cost model is missing 'delay_fo4' object\n";
      exit(1);
    }

    for(auto &kv : *fo4Delays)
    {
      llvm::json::Object *entry = kv.second.getAsObject();
      if(!entry) continue;

      if(kv.first == "comb.icmp")
      {
        if(auto *eq = entry->getObject("eq_predicates"))
        {
          model.delayFO4["comb.icmp.eq"] = parseFO4Entry(eq);
          if(auto *preds = eq->getArray("predicates"))
            for(auto &p : *preds)
              if(auto s = p.getAsString())
                model.icmpPredicateGroup[*s] = "comb.icmp.eq";
        }
        if(auto *mag = entry->getObject("mag_predicates"))
        {
          model.delayFO4["comb.icmp.mag"] = parseFO4Entry(mag);
          if(auto *preds = mag->getArray("predicates"))
            for(auto &p : *preds)
              if(auto s = p.getAsString())
                model.icmpPredicateGroup[*s] = "comb.icmp.mag";
        }
        continue;
      }
      model.delayFO4[kv.first] = parseFO4Entry(entry);
    }
}


static double evalFO4Cost(const FO4Cost & cost, unsigned width)
{
  if(cost.hasLog2Model)
  {
    double bits = static_cast<double>(std::max<unsigned>(width, 1));
    return cost.log2Coeff * std::log2(bits) + cost.log2Const;
  }
  return cost.fixed + cost.perBit * width;
}

static FO4Result runFO4Analysis(ModuleOp module, const CostModel &costModel, llvm::raw_ostream &out,
                              llvm::raw_ostream &err, bool verbose)
{
  FO4Result result;
  llvm::DenseMap<Value, double> arrival; // the map is in format <operation, arrival time>
  std::unordered_map<std::string, unsigned> missingOperationsTable;

  auto getArrival = [&](Value v) -> double
  {
    auto it = arrival.find(v);
    if(it != arrival.end())
      return it->second;
    return 0.0;
  };

  auto recordTimingPath = [&](double delay, llvm::StringRef sinkName)
  {
    result.numTimingPaths++;
    if(delay > result.criticalFO4)
    {
      result.criticalFO4 = delay;
      result.criticalSinkName = sinkName.str();
    }
    if(verbose)
      out << "Path -> " << sinkName << ": " << llvm::format("%.2f", delay) << " FO4\n";
  };

  module.walk([&](Operation *op)
  {
    llvm::StringRef opName = op->getName().getStringRef();

    if(auto firreg = dyn_cast<circt::seq::FirRegOp>(op))
    {
      recordTimingPath(getArrival(firreg.getNext()), firreg.getName());
      arrival[firreg.getResult()] = 0.0;
      return;
    }

    if(op->hasTrait<OpTrait::IsTerminator>())
    {
      for(Value operand : op->getOperands())
        recordTimingPath(getArrival(operand), "output");
      return;
    }

    if(op->getNumResults() == 0)
      return;

    llvm::StringRef lookupName = opName;
    unsigned width = 0;
    if(auto icmp = dyn_cast<circt::comb::ICmpOp>(op))
    {
      std::string predName = circt::comb::stringifyICmpPredicate(icmp.getPredicate()).str();
      auto groupIt = costModel.icmpPredicateGroup.find(predName);
      if(groupIt != costModel.icmpPredicateGroup.end())
        lookupName = groupIt->second;

      if(auto intTy = dyn_cast<IntegerType>(icmp.getLhs().getType()))
        width = intTy.getWidth();
    }
    else if(auto intTy = dyn_cast<IntegerType>(op->getResult(0).getType()))
    {
      width = intTy.getWidth();
    }

    double opDelay{};
    auto it = costModel.delayFO4.find(lookupName);
    if(it != costModel.delayFO4.end())
      opDelay = evalFO4Cost(it->second, width);
    else
      missingOperationsTable[opName.str()]++;

    double maxOperandArrival = 0.0;
    for(Value operand : op->getOperands())
      maxOperandArrival = std::max(maxOperandArrival, getArrival(operand));

    double thisArrival = maxOperandArrival + opDelay;
    for(Value res : op->getResults())
      arrival[res] = thisArrival;

    if(verbose)
      out << opName << " (width " << width << "): arrival" << llvm::format("%.2f", thisArrival) << " FO4\n";
  });

  for(const auto& [key, value] : missingOperationsTable)
  {
    out << "Warning: No FO4 entry for '" << key << "', assuming 0 delay for " << value << " instances\n";
  }

  return result;

}

static void printFO4Report(const FO4Result &r, llvm::raw_ostream &out)
{
  out << "-------------\n"
      << "Critical Path: " << llvm::format("%.2f", r.criticalFO4) << " FO4\n";

  if(!r.criticalSinkName.empty())
    out << " ends at: " << r.criticalSinkName << "\n";

  out << "Timing paths checked: " << r.numTimingPaths << "\n";
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

  CostModel costModel;

  if(doGE)
  {
    loadGECostModel(costModel, inputCostModelJSON);
    GEResult ge = runGEAnalysis(*module, costModel, outputFile.os(), errorFile.os(), verbose);
    printGEReport(ge, outputFile.os());
  }

  if(doFO4)
  {
    loadFO4CostModel(costModel, inputCostModelJSON);
    FO4Result fo4 = runFO4Analysis(*module, costModel, outputFile.os(), errorFile.os(), verbose);
    printFO4Report(fo4, outputFile.os());
  }

  outputFile.keep();
  errorFile.keep();
  return 0;
}
