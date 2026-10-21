#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "FO4Analysis.h"
#include "OpClassify.h"
#include "Options.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Format.h"

using namespace mlir;

FO4Result runFO4Analysis(Operation *root, const CostModel &costModel, llvm::raw_ostream &out,
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

    if(isNotOp(op) && costModel.delayFO4.count("comb.not"))
    {
      lookupName = "comb.not";
      missingKey = "comb.not";
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

    // Timing for paths start at:
    //  - block args: the input ports to modules
    //  - seq.firreg: register outputs (arrival = clk->Q)
    //  - hw.instance: submodule outputs
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

  root->walk([&](Operation *op)
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
    if(warn)
      err << "Warning: No FO4 entry for '" << key << "', assuming 0 delay for " << value
          << " instances (add it to delay_fo4 in the cost model; use { \"fo4\": 0.0 } if it's just wires)\n";

  if(combLoops > 0 && warn)
    err << "WARNING: " << combLoops << " combinational loop(s) detected and cut. "
        << "Critical path may be wrong.\n";

  if(numInstances > 0 && warn)
    err << "WARNING: " << numInstances << " hw.instance op(s) not analyzed internally. "
        << "Paths are split at instance boundaries; run --hw-flatten-modules for end-to-end timing.\n";

  if(unloweredReactiveProcesses > 0 && warn)
    err << "WARNING: " << unloweredReactiveProcesses << " unlowered REACTIVE llhd.process op(s) found "
        << "(contains llhd.wait, meant to react to signal changes). Critical path is likely INCOMPLETE -- "
        << "run additional LLHD lowering passes before trusting this number.\n";

  return result;
}

void printFO4Report(const FO4Result &r, llvm::raw_ostream &out)
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
