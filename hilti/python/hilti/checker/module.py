# $Id$
#
# Modulel-level checks as well those applying to all instructions. 

builtin_type = type

import types

from hilti.core import *
from checker import checker

@checker.pre(module.Module)
def _(self, m):
    if self.moduleSeen():
        self.error(m, "more than one *module* statement")
        
    if self.declSeen():
        self.error(m, "*module* statement does not come first")
        
    self._have_module = True
    self._module = m

    if m.name() == "main":
        run = m.lookupID("run")
        if not run:
            self.error(m, "module Main must define a run() function")
            
        elif not isinstance(run.type(), type.Function):
            self.error(run, "in module Main, ID 'run' must be a function")
    
    for i in m.IDs():
        if i.role() == id.Role.GLOBAL:
            
            if not isinstance(i.type(), type.ValueType) and \
               not isinstance(i.type(), type.Function) and \
               not isinstance(i.type(), type.TypeDeclType):
                self.error(i, "illegal type for global identifier")
                break
            
            if isinstance(i.type(), type.ValueType) and i.type().wildcardType():
                self.error(i, "global variable cannot have a wildcard type")
                break
    
@checker.post(module.Module)
def _(self, m):
    self._module = None
    
### Global ID definitions. 

@checker.when(id.ID, type.StructDecl)
def _(self, id):
    self._have_others = True
    
    if self.currentFunction():
        self.error(id, "structs cannot be declared inside functions")
        
@checker.when(id.ID, type.ValueType)
def _(self, id):
    self._have_others = True

### Function definitions.

@checker.pre(function.Function)
def _(self, f):
    if not self.moduleSeen():
        self.error(f, "input file must start with module declaration")
        
    self._have_others = True
    self._function = f

    for a in f.type().args():
        if isinstance(a.type(), type.Any) and f.callingConvention() != function.CallingConvention.C_HILTI:
            self.error(f, "only functions using C-HILTI calling convention can take parameters of undefined type")
            break
        
        if isinstance(a.type(), type.ValueType) and a.type().wildcardType() and f.callingConvention() != function.CallingConvention.C_HILTI:
            self.error(f, "only functions using C-HILTI calling convention can take wildcard parameters")
            break
        
    for i in f.IDs():
        if i.role() == id.Role.LOCAL:
            if not isinstance(i.type(), type.ValueType) and not isinstance(i.type(), type.Label):
                self.error(i, "local variable %s must be of storage type" % i.name())
                break
            
            if isinstance(i.type(), type.ValueType) and i.type().wildcardType():
                self.error(i, "local variable cannot have a wildcard type")
                break
            
            if isinstance(i.type(), type.Integer) and i.type().width() == 0:
                self.error(i, "local variable cannot have zero width")
                break
    
@checker.post(function.Function)
def _(self, f):
    self._function = None

### Instructions.
@checker.when(instruction.Instruction)
def _(self, i):
    if not self.moduleSeen():
        self.error(f, "input file must start with module declaration")
        
    self._have_others = True

    (success, errormsg) = i.signature().matchWithInstruction(i)
    if not success:
        self.error(i, errormsg)
        return

    if i.target() and not isinstance(i.target(), instruction.IDOperand):
        self.error(i, "target must be an identifier")
