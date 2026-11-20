"""Host same-frame visibility admission. No renderer/publication side effects."""
from driver_subset import evaluate

def visibility_plan(default_hidden,drivers,variables):
 """Return immutable visibility values, or reject the entire candidate.

 drivers contains index,expression records; variables is a corresponding
 per-driver variable mapping resolved from the same input frame. Unsupported
 expressions, duplicate owners and non-boolean results reject, never default.
 Source default flags include explicitly excluded non-character geometry.
 """
 if any(type(v) is not bool for v in default_hidden):raise ValueError('boolean defaults required')
 if len(drivers)!=len(variables):raise ValueError('driver input identity mismatch')
 hidden=list(default_hidden);seen=set()
 for record,values in zip(drivers,variables):
  index=record['index']
  if type(index) is not int or index<0 or index>=len(hidden) or index in seen:raise ValueError('invalid or duplicate visibility owner')
  seen.add(index);value=evaluate(record['expression'],values)
  if value not in (0,1):raise ValueError('visibility driver must resolve boolean')
  hidden[index]=bool(value)
 return tuple(not v for v in hidden)
