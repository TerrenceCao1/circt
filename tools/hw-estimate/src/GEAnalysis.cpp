#include <string>
#include <unordered_map>
#include "GEAnalysis.h"
#include "OpClassify.h"
#include "Options.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Seq/SeqTypes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Format.h"

using namespace mlir;

static double evalGECost(const GECost &cost, unsigned width)
{
  double w = static_cast<double>(width);
  return cost.gePerBit * w + cost.quadGE * w * w + cost.fixedGE;
}

GEResult runGEAnalysis(Operation *root, const CostModel &costModel, llvm::raw_ostream &out,
                              llvm::raw_ostream &err, bool verbose)
{
  GEResult result;
  std::unordered_map<std::string, unsigned> missingOperationsTable;
  unsigned unloweredReactiveProcesses = 0;
  unsigned absorbedNots = 0;

  root->walk([&](Operation *op)
  {
    llvm::StringRef opName = op->getName().getStringRef();
    if(isNotOp(op) && costModel.logicGE.count("comb.not"))
      opName = "comb.not";

    // Simulation Blocks (no GE)
    if(auto proc = dyn_cast<circt::llhd::ProcessOp>(op))
    {
      if(isReactiveLLHDProcess(proc))
        unloweredReactiveProcesses++;
      else if(verbose)
        out << "Note: ignoring non-reactive llhd.process (simulation-only, e.g. an 'initial block); 0 GE\n";

      return;
    }

    // Registers (Implemented as Flip Flops)
    if(auto firreg = dyn_cast<circt::seq::FirRegOp>(op))
    {
      uint64_t bits = getTotalBits(firreg.getType());
      if(bits == 0 && warn)
      {
        err << "Warning: seq.firreg with unrecognized type. 0 FF bits counted\n";
        return;
      }

      result.totalFFBits += bits;

      if(verbose)
      {
        auto parent = op->getParentOfType<circt::hw::HWModuleOp>();
        auto sym = parent ? parent->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName()) : StringAttr();
        out << "FF: " << firreg.getName() << " [" << (sym ? sym.getValue() : llvm::StringRef("?"))
            << "] (" << bits << " bits)\n";
      }

      return;
    }

    // SRAM
    if(auto firmem = dyn_cast<circt::seq::FirMemOp>(op))
    {
      auto memType = dyn_cast<circt::seq::FirMemType>(firmem.getType());
      if(!memType && warn)
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

    if(isAbsorbedNot(op))
    {
      absorbedNots++;
      if(verbose)
        out << "comb.net absorbed: 0.00 GE\n";
      return;
    }


    // Missing Operations
    auto it = costModel.logicGE.find(opName);
    if(it == costModel.logicGE.end())
    {
      missingOperationsTable[opName.str()]++;
      return;
    }

    // Combinational Logic
    unsigned width{};
    if(op->getNumResults() > 0)
    {
     if(auto intTy = dyn_cast<IntegerType>(op->getResult(0).getType()))
        width = intTy.getWidth();
    }

    double ge = evalGECost(it->second, width);
    result.totalLogicGE += ge;
    if(verbose)
      out << opName << " (width " << width << "): " << llvm::format("%.2f", ge) << " GE\n";
  });

  for(const auto& [key, value] : missingOperationsTable)
  {
    if(warn)
      out << "Warning: No GE entry for '" << key << "', assuming 0 GE for " << value << " instances\n";
  }


 if(unloweredReactiveProcesses > 0 && warn)
    out << "WARNING: " << unloweredReactiveProcesses << " unlowered REACTIVE llhd.process op(s) found "
        << "(contains llhd.wait, meant to react to signal changes). Logic GE is likely INCOMPLETE -- "
        << "run additional LLHD lowering passes before trusting this total.\n";
 if(verbose && absorbedNots > 0)
   out << "Note: " << absorbedNots << " NOT op(s) absorbed; 0 GE\n";


  result.totalFFGE = result.totalFFBits * costModel.ffGEPerBit;
  result.totalSramGE = result.totalSramBits * costModel.sramGEPerBit + result.totalSramMacros * costModel.sramFixedGE;

  return result;
}

void printGEReport(const GEResult &r, llvm::raw_ostream &out)
{

  out << "--------\n"
      << "Logic GE: " << llvm::format("%.2f", r.totalLogicGE)<< "\n"
      << "FF GE:    " << llvm::format("%.2f", r.totalFFGE) << " (" << r.totalFFBits << " bits)\n"
      << "SRAM GE:  " << llvm::format("%.2f", r.totalSramGE) << " (" << r.totalSramBits << " bits in " << r.totalSramMacros << " macro(s))\n"
      << "Total GE: " << llvm:: format("%.2f", r.totalLogicGE + r.totalFFGE + r.totalSramGE) << "\n\n";
}
