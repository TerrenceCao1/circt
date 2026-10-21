#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "CostModel.h"
#include "Options.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

// --------------------------------------------------------------------------
// Running GE Analysis
// --------------------------------------------------------------------------
void loadGECostModel(CostModel& model, llvm::StringRef path)
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
  else if(warn)
    llvm::errs() << "Warning: cost model is missing 'ff_ge_per_bit', defaulting to 0\n";

  // load cost for SRAM bits
  if(auto v = root->getNumber("sram_ge_per_bit"))
    model.sramGEPerBit = *v;
  else if(warn)
    llvm::errs() << "Warning cost mosdel is missing 'sram_ge_per_bit', defraulting to 0\n";

  if(auto v = root->getNumber("sram_fixed_ge"))
    model.sramFixedGE = *v;
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

  if(!recognized && warn)
    llvm::errs() << "Warning: delay_fo4 entry '" << name << "' has no recognized delay keys; defaults to 0 FO4.\n";
  return cost;
}

void loadFO4CostModel(CostModel& model, llvm::StringRef path)
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

double evalFO4Cost(const FO4Cost & cost, unsigned width)
{
  if(cost.hasLog2Model)
  {
    double bits = static_cast<double>(std::max<unsigned>(width, 1));
    return cost.log2Coeff * std::log2(bits) + cost.log2Const;
  }
  return cost.fixed + cost.perBit * width;
}
