"""Bounded arithmetic driver reference. No eval, attribute or executable code.
Unknown constructs fail closed; this is not a general Blender driver engine.
"""
import ast,math

def evaluate(expression,variables):
 if len(expression)>1024:raise ValueError('expression too long')
 tree=ast.parse(expression,mode='eval')
 if len(list(ast.walk(tree)))>128:raise ValueError('expression too complex')
 def finite(value):
  if not math.isfinite(value):raise ValueError('nonfinite intermediate')
  return value
 def visit(n):
  if isinstance(n,ast.Expression):return visit(n.body)
  if isinstance(n,ast.Constant) and type(n.value) in (int,float):return float(n.value)
  if isinstance(n,ast.Name) and n.id in variables:
   value=float(variables[n.id])
   if not math.isfinite(value):raise ValueError('nonfinite driver input')
   return value
  if isinstance(n,ast.UnaryOp) and isinstance(n.op,(ast.UAdd,ast.USub)):return visit(n.operand)*(1 if isinstance(n.op,ast.UAdd) else -1)
  if isinstance(n,ast.BinOp) and isinstance(n.op,(ast.Add,ast.Sub,ast.Mult)):
   a,b=visit(n.left),visit(n.right)
   return finite(a+b if isinstance(n.op,ast.Add) else a-b if isinstance(n.op,ast.Sub) else a*b)
  if isinstance(n,ast.BinOp) and isinstance(n.op,ast.Pow):
   # Admit only small literal nonnegative integer exponents; arbitrary powers
   # introduce domain, cost and overflow semantics this subset does not model.
   if not isinstance(n.right,ast.Constant) or type(n.right.value) is not int or not 0<=n.right.value<=4:raise ValueError('unsupported exponent')
   try:return finite(visit(n.left)**n.right.value)
   except OverflowError as error:raise ValueError('driver power overflow') from error
  if isinstance(n,ast.Call) and isinstance(n.func,ast.Name) and n.func.id in ('min','max') and len(n.args)==2 and not n.keywords:
   return (min if n.func.id=='min' else max)(*(visit(x) for x in n.args))
  if isinstance(n,ast.Compare) and len(n.ops)==1 and isinstance(n.ops[0],(ast.Eq,ast.GtE)):
   a,b=visit(n.left),visit(n.comparators[0]);return float(a==b if isinstance(n.ops[0],ast.Eq) else a>=b)
  if isinstance(n,ast.IfExp):return visit(n.body if visit(n.test) else n.orelse)
  raise ValueError('unsupported driver expression')
 value=visit(tree)
 if not math.isfinite(value):raise ValueError('nonfinite driver output')
 return value

def euler_xyz_matrix(x,y,z):
 """Column-vector XYZ local rotation, Rz @ Ry @ Rx."""
 import numpy as np
 a,b,c=math.cos(x),math.cos(y),math.cos(z);s,t,u=math.sin(x),math.sin(y),math.sin(z)
 return np.array(((c*b,c*t*s-u*a,c*t*a+u*s,0),(u*b,u*t*s+c*a,u*t*a-c*s,0),(-t,b*s,b*a,0),(0,0,0,1)))
