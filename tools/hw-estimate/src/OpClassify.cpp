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

// NOT absorption optimization helpers :D

// non-constant operand of a NOT
// since NOT is represented by XOR with one side tied to all-ones, get the operand that isn't all-ones.
Value getNotOperand(Operation *op)
{
  return isAllOnesConst(op->getOperand(0)) ? op->getOperand(1) : op->getOperand(0);
}

// 2-input and/or/xor that isn't a NOT. If a NOT gate is next to one of these, it folds into a NAND/NOR/XNOR cell.
static bool isAbsorbingGate(Operation *op)
{
  if(!isa<circt::comb::AndOp, circt::comb::OrOp, circt::comb::XorOp>(op))
  {
    return false;
  }
  return op->getNumOperands() == 2 && !isNotOp(op);
}

// If we have a NOT defining the input to another NOT, then its just a wire.
bool isDoubleInvOuter(Operation *op)
{
  if(!isNotOp(op))
    return false;

  // check for if we have three NOTs in a row (then we keep one.)
  Operation *inner = getNotOperand(op).getDefiningOp();
  if(!inner || isNotOp(inner))
    return false;

  Operation *base = getNotOperand(inner).getDefiningOp();
  return !(base && isNotOp(base));
}

// Inner NOT of a double inversion: free only if the outer NOT is its sole user.
// Ensures that if a NOT drives two outputs, say an and gate and another NOT, it won't be freed.
static bool isDoubleInvInner(Operation *op)
{
  if(!isNotOp(op) || !op->getResult(0).hasOneUse())
    return false;
  Operation *user = *op->getResult(0).getUsers().begin();

  return isDoubleInvOuter(user);
}

bool isAbsorbedNot(Operation *op)
{
  if(!isNotOp(op))
    return false;

  // Double inversion
  if(isDoubleInvOuter(op) || isDoubleInvInner(op))
    return true;

  // Fed by single-use, 2 input gate: and, or, xor -> nand, nor, xnor
  Value in = getNotOperand(op);
  if(Operation * def = in.getDefiningOp())
    if(isAbsorbingGate(def) && in.hasOneUse())
      return true;

  // Feeds only gates - inversion is absorbed on the gate input
  Value res = op->getResult(0);
  if(res.use_empty())
    return false;
  for(Operation *user : res.getUsers())
    if(!isAbsorbingGate(user))
      return false;

  return true;
}
