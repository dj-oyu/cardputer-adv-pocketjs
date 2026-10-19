"""Negative fixtures must not turn missing context into successful acceptance."""
import copy
import unittest
from derby_observation import camera_windows,compare_fixture,parse_line

class ObservationTests(unittest.TestCase):
    def row(self):
        return parse_line('DERBY_TLM v=1 run=1 us=1000000 begin=0 frame=30 ctx=30 seed=00000002 race=1 phase=2 cam=8 tier=1 step=391 u=500000 gu_sample_max=130000 cap=163840 eval_used=120000 eval_cap=163840 refused=0 seq=1 limit=300 io_us_max=200 intervals='+','.join('29:966667' if i==8 else '0:0' for i in range(10)))
    def test_valid_window_and_gaps_fail_closed(self):
        a=self.row();b=copy.deepcopy(a)
        b.update(us=2000000,frame=60,ctx=60,seq=2)
        b['intervals'][8]=(59,1966667)
        window=camera_windows([a,b])[0]
        self.assertEqual(window['diagnostic_fps'],30)
        self.assertEqual((window['start_seq'],window['end_seq'],window['start_step'],window['end_step']),
                         (1,2,391,391))
        self.assertEqual((window['start_device_us'],window['end_device_us'],window['context_delta']),
                         (1000000,2000000,30))
        for field,value in [('ctx',61),('seq',3),('refused',1),('run',2),('cam',7),('us',1),('frame',30)]:
            c=copy.deepcopy(b);c[field]=value
            self.assertEqual(camera_windows([a,c]),[],field)
        b['intervals'][8]=(58,1966667) # one intervening camera/frame cannot disappear
        self.assertEqual(camera_windows([a,b]),[])
        b['intervals'][8]=(59,1066667) # inconsistent device time cannot inflate fps
        self.assertEqual(camera_windows([a,b]),[])
        b['intervals'][8]=(59,1966667);a['intervals'][7]=(1,100)
        self.assertEqual(camera_windows([a,b]),[]) # another camera regressed
        a['intervals'][7]=(0,0);b['intervals'][7]=(1,100)
        self.assertEqual(camera_windows([a,b]),[]) # another camera cannot increase either
        b['intervals'][7]=(0,0);b['begin']=1
        self.assertEqual(camera_windows([a,b]),[])
        b['begin']=0;b['us']=1500000;b['intervals'][8]=(59,1466667)
        self.assertEqual(camera_windows([a,b]),[]) # equal durations but less than 1 s
    def test_duplicate_unknown_and_negative_rejected(self):
        for tail in ['v=1 v=1','v=2','v=1 run=-1']:
            with self.assertRaises((ValueError,KeyError)):parse_line('DERBY_TLM '+tail)
    def test_oracle_requires_full_controlled_inputs(self):
        e=dict(seed=2,oval=1,step=391,camera=8,interpolation=.5,tier=1,
               xs=[1],depths=[12],projected=[[110.5,.2]],drawn=[{'l':0,'X':110.5}],side=[0])
        o=dict(e,kind='controlled_draw_inputs',camera_cut=True)
        self.assertTrue(compare_fixture(e,o))
        for change in [dict(camera_cut=False),dict(step=392),dict(projected=[[110.6,.2]]),dict(projected=[[float('nan'),.2]])]:
            with self.assertRaises(ValueError):compare_fixture(e,dict(o,**change))
        del o['drawn']
        with self.assertRaises(ValueError):compare_fixture(e,o)

if __name__=='__main__':unittest.main()
