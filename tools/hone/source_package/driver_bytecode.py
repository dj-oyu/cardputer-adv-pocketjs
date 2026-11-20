"""Compile the bounded driver subset to a native candidate, not runtime admission."""
import ast, math

OPS={'constant':1,'variable':2,'neg':3,'add':4,'sub':5,'mul':6,'pow':7,
     'min':8,'max':9,'eq':10,'ge':11,'jz':12,'jump':13,'end':14}


def compile_expression(expression,names):
    if not isinstance(expression,str) or len(expression)>1024:raise ValueError('expression length')
    if len(names)>64 or len(set(names))!=len(names) or any(not isinstance(x,str) for x in names):raise ValueError('variable names')
    tree=ast.parse(expression,mode='eval')
    if sum(1 for _ in ast.walk(tree))>128:raise ValueError('expression too complex')
    output=[]
    def emit(op,arg=0,value=0.):
        output.append([OPS[op],arg,value]);return len(output)-1
    def visit(n):
        if isinstance(n,ast.Constant) and type(n.value) in (int,float):
            v=float(n.value)
            if not math.isfinite(v):raise ValueError('constant')
            emit('constant',value=v);return
        if isinstance(n,ast.Name) and n.id in names:emit('variable',names.index(n.id));return
        if isinstance(n,ast.UnaryOp) and isinstance(n.op,(ast.UAdd,ast.USub)):
            visit(n.operand)
            if isinstance(n.op,ast.USub):emit('neg')
            return
        if isinstance(n,ast.BinOp) and isinstance(n.op,(ast.Add,ast.Sub,ast.Mult)):
            visit(n.left);visit(n.right);emit({ast.Add:'add',ast.Sub:'sub',ast.Mult:'mul'}[type(n.op)]);return
        if isinstance(n,ast.BinOp) and isinstance(n.op,ast.Pow):
            if not isinstance(n.right,ast.Constant) or type(n.right.value) is not int or not 0<=n.right.value<=4:raise ValueError('unsupported exponent')
            visit(n.left);emit('pow',n.right.value);return
        if isinstance(n,ast.Call) and isinstance(n.func,ast.Name) and n.func.id in ('min','max') and len(n.args)==2 and not n.keywords:
            visit(n.args[0]);visit(n.args[1]);emit(n.func.id);return
        if isinstance(n,ast.Compare) and len(n.ops)==1 and isinstance(n.ops[0],(ast.Eq,ast.GtE)):
            visit(n.left);visit(n.comparators[0]);emit('eq' if isinstance(n.ops[0],ast.Eq) else 'ge');return
        if isinstance(n,ast.IfExp):
            visit(n.test);conditional=emit('jz');visit(n.body);end=emit('jump')
            output[conditional][1]=len(output);visit(n.orelse);output[end][1]=len(output);return
        raise ValueError('unsupported expression')
    visit(tree.body);emit('end')
    if len(output)>256:raise ValueError('instruction bound')
    # Every path must fit the target's fixed stack and agree at branch joins.
    heights={0:0};todo=[0]
    while todo:
        pc=todo.pop();height=heights[pc];op,arg,_=output[pc]
        if op in (OPS['constant'],OPS['variable']):need,delta=0,1
        elif op in (OPS['neg'],OPS['pow']):need,delta=1,0
        elif op in (OPS['add'],OPS['sub'],OPS['mul'],OPS['min'],OPS['max'],OPS['eq'],OPS['ge']):need,delta=2,-1
        elif op==OPS['jz']:need,delta=1,-1
        else:need,delta=0,0
        if height<need or height+delta>32:raise ValueError('driver stack bound')
        after=height+delta
        if op==OPS['end']:
            if height!=1 or pc!=len(output)-1:raise ValueError('driver end stack')
            continue
        successors=[arg] if op==OPS['jump'] else ([pc+1,arg] if op==OPS['jz'] else [pc+1])
        for target in successors:
            if not pc<target<len(output):raise ValueError('driver branch')
            if target in heights:
                if heights[target]!=after:raise ValueError('driver stack merge')
            else:heights[target]=after;todo.append(target)
    return output
