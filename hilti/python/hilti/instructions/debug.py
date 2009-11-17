# $Id$
"""
Debugging Support
~~~~~~~~~~~~~~~~~
"""

from hilti.core.instruction import *
from hilti.core.constraints import *
from hilti.core.constant import *

@instruction("debug.msg", op1=string, op2=string, op3=tuple)
class Msg(Instruction):
    """ If compiled in debug mode, prints a debug message to stderr. The
    message is only printed of the debug stream *op1* has been activated.
    *op2* is printf-style format string and *op3* the correponding arguments.
    """

def message(stream, fmt, args = []):
    """Helpers function to create a ~~Msg instruction.
    
    stream: string - The name of the debug stream.
    msg: string - The format string.
    args: list of ~~ConstOperand - The list of format arguments.
    """

    return Msg(op1=ConstOperand(Constant(stream, type.String())), 
               op2=ConstOperand(Constant(fmt, type.String())),
               op3=TupleOperand(args))


    