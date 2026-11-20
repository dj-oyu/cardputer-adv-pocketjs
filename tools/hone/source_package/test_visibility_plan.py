from visibility_plan import visibility_plan
committed=(True,True,False)
for a in (0,.5,.979999,.98,.980001,1):
 candidate=visibility_plan((False,False,True),({'index':0,'expression':'max(a,b)>=0.98'},),({'a':a,'b':0},))
 assert candidate==(a<.98,True,False)
assert visibility_plan((False,),({'index':0,'expression':'max(a,b)>=0.98'},),({'a':0,'b':1},))==(False,)
for records,values in (([{'index':0,'expression':'a'}],[{'a':.5}]),([{'index':3,'expression':'a'}],[{'a':0}]),([{'index':0,'expression':'a'},{'index':0,'expression':'a'}],[{'a':0},{'a':1}])):
 try:visibility_plan((False,False,True),records,values)
 except ValueError:pass
 else:raise AssertionError('invalid candidate accepted')
 assert committed==(True,True,False)
assert visibility_plan((False,False,True),[],[])==committed
print('PASS visibility thresholds/defaults/duplicate owner rejection and candidate retry; publication remains caller-owned')
