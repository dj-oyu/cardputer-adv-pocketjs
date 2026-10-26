"""Regression tests for conservative adopted-candidate baseline isolation."""
from pathlib import Path
import json,os,subprocess,tempfile,unittest
from isolate_adopted_baseline import isolate,reverse_located

START='function target() {\n'
END='\nfunction next() {'
PREFIX='outside_before\n'
SUFFIX='\nfunction next() {\n}\n'
BODY=START+''.join('line%d;\n'%i for i in range(18))+'}\n'
BEFORE=(PREFIX+BODY+SUFFIX).encode()
AFTER=BEFORE.replace(b'line8;\n',b'value=new;\n')
CONTEXT=b'line5;\nline6;\nline7;\nvalue=new;\nline9;\nline10;\nline11;\n'

class Isolation(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory(prefix='baseline-regression-')
  self.root=Path(self.temp.name)
  self.env=dict(os.environ,GIT_AUTHOR_NAME='Test',GIT_AUTHOR_EMAIL='test@example.invalid',
                GIT_COMMITTER_NAME='Test',GIT_COMMITTER_EMAIL='test@example.invalid')
  self.git('init','-q')
  self.before=self.commit(BEFORE,b'old other\n')
  self.after=self.commit(AFTER,b'old other\n')
  self.current=AFTER.replace(b'outside_before',b'updated\noutside_before').replace(b'line16;',b'later_same_function;')
  self.head=self.commit(self.current,b'new other\n')
 def tearDown(self):self.temp.cleanup()
 def git(self,*args):
  return subprocess.check_output(['git',*args],cwd=self.root,env=self.env)
 def commit(self,source,other):
  (self.root/'app.js').write_bytes(source);(self.root/'other.js').write_bytes(other)
  self.git('add','.');self.git('commit','-qm','fixture')
  return self.git('rev-parse','HEAD').decode().strip()
 def snapshot(self):
  return (self.git('rev-parse','HEAD'),self.git('write-tree'),
          (self.root/'app.js').read_bytes(),(self.root/'other.js').read_bytes(),
          self.git('status','--porcelain=v1','--untracked-files=no'))
 def invoke(self,output=None):
  return isolate(self.root,self.before,self.after,'app.js',START,END,
                 output or self.root/'out/report.json')
 def reject(self):
  before=self.snapshot()
  with self.assertRaises(ValueError):self.invoke()
  self.assertEqual(self.snapshot(),before)
  self.assertFalse((self.root/'out/report.json').exists())
 def test_preserves_later_changes_and_real_checkout(self):
  (self.root/'untracked-support').write_text('allowed')
  before=self.snapshot();baseline=self.invoke()
  expected=self.current.replace(b'value=new;\n',b'line8;\n')
  self.assertEqual(self.git('show',baseline+':app.js'),expected)
  self.assertEqual(self.git('show',baseline+':other.js'),b'new other\n')
  self.assertEqual(self.snapshot(),before)
  self.assertEqual(self.git('diff','--name-only',baseline,self.head),b'app.js\n')
  report=json.loads((self.root/'out/report.json').read_text())
  self.assertTrue(report['roundtrip_tree_equal'])
  self.assertEqual(len(report['hunks']),1)
 def test_wrong_candidate(self):
  self.commit(self.current.replace(b'value=new;',b'value=wrong;'),b'new other\n')
  self.reject()
 def test_duplicate_context_inside_scope(self):
  self.commit(self.current.replace(END.encode(),CONTEXT+END.encode()),b'new other\n')
  self.reject()
 def test_duplicate_context_outside_scope(self):
  self.commit(self.current+CONTEXT,b'new other\n')
  self.reject()
 def test_only_outside_duplicate_survives(self):
  self.commit(self.current.replace(b'value=new;',b'value=wrong;')+CONTEXT,b'new other\n')
  self.reject()
 def test_moved_context_in_same_scope(self):
  moved=self.current.replace(CONTEXT,b'',1).replace(END.encode(),CONTEXT+END.encode())
  self.commit(moved,b'new other\n');self.reject()
 def test_duplicate_scope_marker(self):
  self.commit(self.current+START.encode(),b'new other\n');self.reject()
 def test_dirty_target_worktree(self):
  (self.root/'app.js').write_bytes(self.current+b'dirty\n');self.reject()
 def test_dirty_other_worktree(self):
  (self.root/'other.js').write_text('dirty\n');self.reject()
 def test_dirty_target_index(self):
  (self.root/'app.js').write_bytes(self.current+b'dirty\n');self.git('add','app.js');self.reject()
 def test_dirty_other_index(self):
  (self.root/'other.js').write_text('dirty\n');self.git('add','other.js');self.reject()
 def test_report_cannot_overwrite_tracked_file(self):
  before=self.snapshot()
  with self.assertRaises(ValueError):self.invoke(self.root/'other.js')
  self.assertEqual(self.snapshot(),before)
 def test_empty_patch_rejected(self):
  with self.assertRaises(ValueError):reverse_located(AFTER,AFTER,self.current,START,END)
 def test_accepted_outside_scope_change_rejected(self):
  wrong_after=AFTER.replace(b'outside_before',b'wrong_outside')
  with self.assertRaises(ValueError):reverse_located(BEFORE,wrong_after,self.current,START,END)

if __name__=='__main__':unittest.main()
