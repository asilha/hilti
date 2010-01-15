# $Id$ 
#
# Code generation for the struct type.

import llvm.core

from hilti.core import *
from hilti import instructions
from codegen import codegen

import sys

_doc_c_conversion = """ 
A ``struct` is passed as a pointer to an eqivalent C struct; the fields' types
are converted recursively per the same rules. 
"""

def _llvmStructType(struct):
    # Currently, we support only up to 32 fields.
    assert len(struct.fields()) <= 32
    fields = [llvm.core.Type.int(32)] + [codegen.llvmTypeConvert(id.type()) for (id, default) in struct.fields()]
    return llvm.core.Type.struct(fields)

@codegen.typeInfo(type.Struct)
def _(type):
    typeinfo = codegen.TypeInfo(type)
    typeinfo.to_string = "hlt::struct_to_string";
    typeinfo.args = [id.type() for (id, op) in type.fields()]
    
    ## FIXME: This code is copied (and slightly adapted) from tuple. Factor
    # this out. 
    
    # Calculate the offset array. 
    zero = codegen.llvmGEPIdx(0)
    null = llvm.core.Constant.null(llvm.core.Type.pointer(_llvmStructType(type)))
    
    offsets = []
    for i in range(len(type.fields())):
        # Note that we skip the bitmask here. 
        idx = codegen.llvmGEPIdx(i + 1)
        # This is a pretty awful hack but I can't find a nicer way to
        # calculate the offsets as *constants*, and this hack is actually also
        # used by LLVM internaly to do sizeof() for constants so it can't be
        # totally disgusting. :-)
        offset = null.gep([zero, idx]).ptrtoint(llvm.core.Type.int(16))
        offsets += [offset]

    name = codegen.nameTypeInfo(type) + "_offsets"
    const = llvm.core.Constant.array(llvm.core.Type.int(16), offsets)
    glob = codegen.llvmCurrentModule().add_global_variable(const.type, name)
    glob.global_constant = True    
    glob.initializer = const
    glob.linkage = llvm.core.LINKAGE_LINKONCE_ANY

    typeinfo.aux = glob
    
    return typeinfo

@codegen.llvmCtorExpr(type.Struct)
def _(op, refine_to):
    assert False

@codegen.llvmType(type.Struct)
def _(type, refine_to):
    return llvm.core.Type.pointer(_llvmStructType(type))

@codegen.operator(instructions.struct.New)
def _(self, op):
    # Allocate memory for struct. 
    structt = op.op1().value()
    llvm_type = _llvmStructType(structt)
    s = codegen.llvmMalloc(llvm_type)
    
    # Initialize fields
    zero = codegen.llvmGEPIdx(0)
    mask = 0
    
    fields = structt.fields()
    for j in range(len(fields)):
        (id, default) = fields[j]
        if default:
            # Initialize with default. 
            mask |= (1 << j)
            index = codegen.llvmGEPIdx(j + 1)
            addr = codegen.builder().gep(s, [zero, index])
            codegen.llvmInit(codegen.llvmOp(default, id.type()), addr)
        else:
            # Leave untouched. As we keep the bitmask of which fields are
            # set,  we will never access it. 
            pass
        
    # Set mask.
    addr = codegen.builder().gep(s, [zero, zero])
    codegen.llvmInit(codegen.llvmConstInt(mask, 32), addr)

    codegen.llvmStoreInTarget(op.target(), s)
    
def _getIndex(instr):
    
    fields = instr.op1().type().refType().fields()
    
    for i in range(len(fields)):
        (id, default) = fields[i]
        if id.name() == instr.op2().value():
            return (i, id.type())
        
    return (-1, None)

@codegen.when(instructions.struct.Get)
def _(self, i):
    (idx, ftype) = _getIndex(i)
    assert idx >= 0
    
    s = codegen.llvmOp(i.op1())
    
    # Check whether field is valid. 
    zero = codegen.llvmGEPIdx(0)
    
    addr = codegen.builder().gep(s, [zero, zero])
    mask = codegen.builder().load(addr)
    
    bit = codegen.llvmConstInt(1<<idx, 32)
    isset = codegen.builder().and_(bit, mask)
    
    block_ok = self.llvmNewBlock("ok")
    block_exc = self.llvmNewBlock("exc")
    
    notzero = self.builder().icmp(llvm.core.IPRED_NE, isset, self.llvmConstInt(0, 32))
    self.builder().cbranch(notzero, block_ok, block_exc)
    
    self.pushBuilder(block_exc)
    self.llvmRaiseExceptionByName("hlt_exception_undefined_value", i.location())
    self.popBuilder()
    
    self.pushBuilder(block_ok)
    
    # Load field.
    index = codegen.llvmGEPIdx(idx + 1)
    addr = codegen.builder().gep(s, [zero, index])
    codegen.llvmStoreInTarget(i.target(), codegen.builder().load(addr))
    
@codegen.when(instructions.struct.Set)
def _(self, i):
    (idx, ftype) = _getIndex(i)
    assert idx >= 0

    s = codegen.llvmOp(i.op1())
    
    # Set mask bit.
    zero = codegen.llvmGEPIdx(0)
    addr = codegen.builder().gep(s, [zero, zero])
    mask = codegen.builder().load(addr)
    bit = codegen.llvmConstInt(1<<idx, 32)
    mask = codegen.builder().or_(bit, mask)
    codegen.llvmAssign(mask, addr)
    
    index = codegen.llvmGEPIdx(idx + 1)
    addr = codegen.builder().gep(s, [zero, index])
    codegen.llvmAssign(codegen.llvmOp(i.op3(), ftype), addr)

@codegen.when(instructions.struct.Unset)
def _(self, i):
    (idx, ftype) = _getIndex(i)
    assert idx >= 0

    s = codegen.llvmOp(i.op1())
    
    # Clear mask bit.
    zero = codegen.llvmGEPIdx(0)
    addr = codegen.builder().gep(s, [zero, zero])
    mask = codegen.builder().load(addr)
    bit = codegen.llvmConstInt(~(1<<idx), 32)
    mask = codegen.builder().and_(bit, mask)
    codegen.llvmAssign(mask, addr)

@codegen.when(instructions.struct.IsSet)
def _(self, i):
    (idx, ftype) = _getIndex(i)
    assert idx >= 0

    s = codegen.llvmOp(i.op1())

    # Check mask.
    zero = codegen.llvmGEPIdx(0)
    addr = codegen.builder().gep(s, [zero, zero])
    mask = codegen.builder().load(addr)
    
    bit = codegen.llvmConstInt(1<<idx, 32)
    isset = codegen.builder().and_(bit, mask)

    notzero = self.builder().icmp(llvm.core.IPRED_NE, isset, self.llvmConstInt(0, 32))
    codegen.llvmStoreInTarget(i.target(), notzero)

