import unittest
from provider_binding import binding_plan
from test_consumer_plan import fixture
from consumer_plan import prepare

def source():
    p=fixture();m=p['meshes'][0];m['material_slots']=['a','b']
    for i,poly in enumerate(m['polygons']):poly['material']=i
    return p
class TestBinding(unittest.TestCase):
    def test_material_order_source_transform(self):
        p=source();b=binding_plan(p);mesh=prepare(p)['meshes'][0]
        self.assertFalse(b['runtime_ready']);self.assertEqual(b['geometry_nodes'],2)
        self.assertEqual([n['material_slot'] for n in b['nodes']],[0,1])
        self.assertEqual([n['source_transform'] for n in b['nodes']],[0,0])
        self.assertEqual([t for n in b['nodes'] for t in n['source_triangles']],[0,1])
        actual=[[n['prepared_vertices'][v] for v in t] for n in b['nodes'] for t in n['triangles']]
        self.assertEqual(actual,mesh['triangles'])
    def test_missing_or_bad_material(self):
        for bad in [-1,2,True,None]:
            p=source();p['meshes'][0]['polygons'][0]['material']=bad
            with self.assertRaises(ValueError):binding_plan(p)
        p=source();p['meshes'][0]['material_slots']=[]
        with self.assertRaises(ValueError):binding_plan(p)
if __name__=='__main__':unittest.main()
