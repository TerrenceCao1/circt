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
#include <vector>
#include "circt/Dialect/Comb/CombDialect.h"
#include "circt/Dialect/HW/HWTypes.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Seq/SeqTypes.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
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
    bool    hasLog2Model{false};
    double  log2Coeff{};
    double  log2Const{};
    double  fixed{};
    double  perBit{};
    bool    scalesWithOperands{false};
  };

  struct FO4Result
  {
    double                    criticalFO4{};
    long long                 numTimingPaths{};
    std::string               criticalSinkName;
    std::vector<std::string>  criticalPath;
  };

  struct CostModel
  {
    llvm::StringMap<GECost> logicGE;
    llvm::StringMap<FO4Cost>  delayFO4;
    llvm::StringMap<std::string> icmpPredicateGroup;
    double ffGEPerBit{};
    double sramGEPerBit{};
    double sramFixedGE{};
    double clkToQFO4{};   // [JSON] optional "clk_to_q_fo4", added at register startpoints
    double setupFO4{};
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

static bool isReactiveLLHDProcess(circt::llhd::ProcessOp proc)
{
  bool hasWait = false;
  proc.getBody().walk([&](circt::llhd::WaitOp) { hasWait = true; });
  return hasWait;
}

static GEResult runGEAnalysis(ModuleOp module, const CostModel &costModel, llvm::raw_ostream &out,
                              llvm::raw_ostream &err, bool verbose)
{
  GEResult result;
  std::unordered_map<std::string, unsigned> missingOperationsTable;
  unsigned unloweredReactiveProcesses = 0;

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

    if(auto proc = dyn_cast<circt::llhd::ProcessOp>(op))
    {
      if(isReactiveLLHDProcess(proc))
        unloweredReactiveProcesses++;
      else if(verbose)
        out << "Note: ignoring non-reactive llhd.process (simulation-only, e.g. an 'initial block); 0 GE\n";
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


 if(unloweredReactiveProcesses > 0)
    out << "WARNING: " << unloweredReactiveProcesses << " unlowered REACTIVE llhd.process op(s) found "
        << "(contains llhd.wait, meant to react to signal changes). Logic GE is likely INCOMPLETE -- "
        << "run additional LLHD lowering passes before trusting this total.\n";

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
static FO4Cost parseFO4Entry(llvm::StringRef name, llvm::json::Object *entry)
{
  FO4Cost cost;
  bool recognized = false;

  if(auto v = entry->getNumber("fo4"))          {cost.fixed = *v; recognized = true;}
  if(auto v = entry->getNumber("fo4_fixed"))    {cost.fixed = *v; recognized = true;}
  if(auto v = entry->getNumber("fo4_per_bit"))  {cost.fixed = *v; recognized = true;}

  if(auto v = entry->getNumber("fo4_log2_coeff"))
  {
    cost.log2Coeff = *v;
    cost.hasLog2Model = true;
    recognized = true;
  }

  if(auto v = entry->getNumber("fo4_log2_const")) {cost.fixed = *v; recognized = true;}

  // TODO: implement scales_with_operands from json.

  if(!recognized)
    llvm::errs() << "Warning: delay_fo4 entry '" << name << "' has no recognized delay keys; defaults to 0 FO4.\n";
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
  if(!root)
  {
    llvm::errs() << "Error: cost model root must be a JSON object\n";
    exit(1);
  }

  if(!fo4Delays)
  {
    llvm::errs() << "Error: cost model is missing 'delay_fo4' object\n";
    exit(1);
  }

  if(auto v = root->getNumber("clk_to_q_fo4"))
    model.clkToQFO4 = *v;
  if(auto v = root->getNumber("setup_fo4"))
    model.setupFO4 = *v;

  for(auto &kv : *fo4Delays)
  {
    llvm::json::Object *entry = kv.second.getAsObject();
    if(!entry) continue;

    if(kv.first == "comb.icmp")
    {
      if(auto *eq = entry->getObject("eq_predicates"))
      {
        model.delayFO4["comb.icmp.eq"] = parseFO4Entry("comb.icmp.eq_predicates", eq);
        if(auto *preds = eq->getArray("predicates"))
          for(auto &p : *preds)
            if(auto s = p.getAsString())
              model.icmpPredicateGroup[*s] = "comb.icmp.eq";
      }
      if(auto *mag = entry->getObject("mag_predicates"))
      {
        model.delayFO4["comb.icmp.mag"] = parseFO4Entry("comb.icmp.mag_predicates", mag);
        if(auto *preds = mag->getArray("predicates"))
          for(auto &p : *preds)
            if(auto s = p.getAsString())
              model.icmpPredicateGroup[*s] = "comb.icmp.mag";
      }
      continue;
    }
    model.delayFO4[kv.first] = parseFO4Entry(kv.first, entry);
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

  llvm::DenseMap<Value, double> arrival;    // Value -> arrival time
  llvm::DenseMap<Value, Value>  critPred;   // Value -> operand that set its arrival
  llvm::DenseSet<Operation *>   inProgress; // Operations in DFS stack
  std::unordered_map<std::string, unsigned> missingOperationsTable;
  unsigned unloweredReactiveProcesses = 0;
  unsigned combLoops = 0;
  unsigned numInstances = 0;

  auto opDelayOf = [&](Operation *op, unsigned& widthOut) -> double
  {
    llvm::StringRef opName = op->getName().getStringRef();
    llvm::StringRef lookupName = opName;
    std::string missingKey = opName.str();
    widthOut = 0;

    if(auto icmp = dyn_cast<circt::comb::ICmpOp>(op))
    {
      std::string predName = circt::comb::stringifyICmpPredicate(icmp.getPredicate()).str();
      missingKey = "comb.icmp." + predName;   // [JSON] report WHICH predicate is missing (e.g. weq)
      auto groupIt = costModel.icmpPredicateGroup.find(predName);
      if(groupIt != costModel.icmpPredicateGroup.end())
        lookupName = groupIt->second;

      // icmp's result is i1, so cost depends on the OPERAND width
      if(auto intTy = dyn_cast<IntegerType>(icmp.getLhs().getType()))
        widthOut = intTy.getWidth();
    }
    else if(op->getNumResults() > 0)
    {
      if(auto intTy = dyn_cast<IntegerType>(op->getResult(0).getType()))
        widthOut = intTy.getWidth();
    }

    auto it = costModel.delayFO4.find(lookupName);
    if(it == costModel.delayFO4.end())
    {
      missingOperationsTable[missingKey]++;   // each op is evaluated once (memoized), so this counts instances
      return 0.0;
    }

    const FO4Cost &cost = it->second;
    double d = evalFO4Cost(cost, widthOut);

    // [JSON] Variadic ops: an N-input op is a tree of ceil(log2(N)) levels of the
    // 2-input delay in the table. Driven by "scales_with_operands" instead of a hardcoded list.
    if(cost.scalesWithOperands && op->getNumOperands() > 2)
      d *= std::ceil(std::log2(static_cast<double>(op->getNumOperands())));

    return d;
  };

  std::function<double(Value)> arrivalOf = [&](Value v) -> double
  {
    if(auto it = arrival.find(v); it != arrival.end())
      return it->second;

    Operation *def = v.getDefiningOp();

    // Timing for paths start here:
    //  - block args: the input ports to modules
    //  - seq.firreg: register outputs (arrival = clk->Q)
    //  - hw.instance: submodule outputes
    //  - llhd.process: unlowered.
    if(!def || isa<circt::seq::FirRegOp, circt::hw::InstanceOp, circt::llhd::ProcessOp>(def))
    {
      double t = isa_and_nonnull<circt::seq::FirRegOp>(def) ? costModel.clkToQFO4 : 0.0;
      arrival[v] = t;
      return t;
    }

    // Combinational loop guard (So we don't recurse forever)
    if(!inProgress.insert(def).second)
    {
      combLoops++;
      return 0.0;
    }

    double maxIn = 0.0;
    Value  pred;
    for(Value in : def->getOperands())
    {
      double a = arrivalOf(in);
      if(!pred || a > maxIn)
      {
        maxIn = a;
        pred = in;
      }
    }

    unsigned width = 0;
    double t = maxIn + opDelayOf(def, width);

    for (Value res : def->getResults())
    {
      arrival[res] = t;
      if(pred)
        critPred[res] = pred;
    }

    inProgress.erase(def);

    if(verbose)
      out << def->getName() << " (width " << width << "): arrival " << llvm::format("%.2f", t) << " FO4\n";

    return t;
  };

  // Describing one hop on a path
  auto describe = [&](Value v) -> std::string
  {
    std::string s;
    llvm::raw_string_ostream os(s);
    if(Operation *def = v.getDefiningOp())
    {
      os << def->getName();
      if(auto r = dyn_cast<circt::seq::FirRegOp>(def))
        os << " '" << r.getName() << "'";
      else if(auto i = dyn_cast<circt::hw::InstanceOp>(def))
        os << " '" << i.getInstanceName() << "'";
    }
    else
      os << "input port";
    os << " @ " << llvm::format("%.2f", arrivalOf(v)) << " FO4";
    return os.str();
  };

  // Listing out the critical path through an endpoint
  auto buildPath = [&](Value endpoint) -> std::vector<std::string>
  {
    std::vector<std::string> path;
    Value cur = endpoint;
    while(cur)
    {
      path.push_back(describe(cur));
      auto it = critPred.find(cur);
      if(it == critPred.end())
        break;
      cur = it->second;
    }
    std::reverse(path.begin(), path.end());
    return path;
  };

  auto recordTimingPath = [&](Value endpoint, double overhead, llvm::StringRef sinkName)
  {
    double delay = arrivalOf(endpoint) + overhead;
    result.numTimingPaths++;
    if(delay > result.criticalFO4)
    {
      result.criticalFO4 = delay;
      result.criticalSinkName = sinkName.str();
      result.criticalPath = buildPath(endpoint);
    }

    if(verbose)
      out << "Path -> " << sinkName << ": " << llvm::format("%.2f", delay) << " FO4\n";
  };

  module.walk([&](Operation *op)
  {
    if(auto firreg = dyn_cast<circt::seq::FirRegOp>(op))
    {
      recordTimingPath(firreg.getNext(), costModel.setupFO4, firreg.getName());
      return;
    }

    if(auto proc = dyn_cast<circt::llhd::ProcessOp>(op))
    {
      if(isReactiveLLHDProcess(proc))
        unloweredReactiveProcesses++;
      else if(verbose)
        out << "Note: ignoring non-reactive llhd.process (simulation-only); 0 FO4\n";
      return;
    }

    if(auto inst = dyn_cast<circt::hw::InstanceOp>(op))
    {
      numInstances++;
      std::string name = ("instance " + inst.getInstanceName() + " input").str();
      for(Value operand : inst->getOperands())
        recordTimingPath(operand, 0.0, name);
      return;
    }

    if(isa<circt::hw::OutputOp>(op))
    {
      for(Value operand : op->getOperands())
        recordTimingPath(operand, 0.0, "output");
      return;
    }
  });

  for(const auto &[key, value] : missingOperationsTable)
    err << "Warning: No FO4 entry for '" << key << "', assuming 0 delay for " << value
        << " instances (add it to delay_fo4 in the cost model; use { \"fo4\": 0.0 } if it's just wires)\n";

  if(combLoops > 0)
    err << "WARNING: " << combLoops << " combinational loop(s) detected and cut. "
        << "Critical path may be wrong.\n";

  if(numInstances > 0)
    err << "WARNING: " << numInstances << " hw.instance op(s) not analyzed internally. "
        << "Paths are split at instance boundaries; run --hw-flatten-modules for end-to-end timing.\n";

  if(unloweredReactiveProcesses > 0)
    err << "WARNING: " << unloweredReactiveProcesses << " unlowered REACTIVE llhd.process op(s) found "
        << "(contains llhd.wait, meant to react to signal changes). Critical path is likely INCOMPLETE -- "
        << "run additional LLHD lowering passes before trusting this number.\n";

  return result;
}

static void printFO4Report(const FO4Result &r, llvm::raw_ostream &out)
{
  out << "-------------\n"
      << "Critical Path: " << llvm::format("%.2f", r.criticalFO4) << " FO4\n";

  if(!r.criticalSinkName.empty())
    out << " ends at: " << r.criticalSinkName << "\n";

  if(!r.criticalPath.empty())
  {
    out << " path:\n";
    for(const std::string &hop : r.criticalPath)
      out << "   " << hop << "\n";
  }

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
