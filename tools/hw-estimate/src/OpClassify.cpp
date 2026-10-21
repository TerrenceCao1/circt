#include "OpClassify.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/HW/HWTypes.h"
#include "mlir/IR/BuiltinTypes.h"

using namespace mlir;

// Recursive helper for getting the total number of flip flop bits
uint64_t getTotalBits(Type type)
{
  if(auto intTy = dyn_cast<IntegerType>(type))
    return intTy.getWidth();

  if(auto arrTy = dyn_cast<circt::hw::ArrayType>(type))
    return arrTy.getNumElements() * getTotalBits(arrTy.getElementType());

  return 0;
}

bool isReactiveLLHDProcess(circt::llhd::ProcessOp proc)
{
  bool hasWait = false;
  proc.getBody().walk([&](circt::llhd::WaitOp) { hasWait = true; });
  return hasWait;
}

static bool isAllOnesConst(Value v)
{
  if(auto c = v.getDefiningOp<circt::hw::ConstantOp>())
    return c.getValue().isAllOnes();
  return false;
}

bool isNotOp(Operation *op)
{
  auto x = dyn_cast<circt::comb::XorOp>(op);
  if(!x || x->getNumOperands() != 2)
    return false;
  return isAllOnesConst(x->getOperand(0)) != isAllOnesConst(x->getOperand(1));
}
