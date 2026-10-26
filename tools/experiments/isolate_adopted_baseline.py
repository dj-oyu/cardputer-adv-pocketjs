"""Build a local fixture baseline by reverting one located, adopted change.

Untracked support checkouts/reports are allowed. Tracked worktree/index changes
are rejected. A moved or ambiguous hunk needs review instead of fuzzy matching.
"""
from pathlib import Path
import argparse,difflib,hashlib,json,os,subprocess,tempfile

CASES={
 'result57':('d7d9d90050d172e176c0f3ddbe3666a07af1ff7f','931e4bac4d755bebf5fd7cc0b53a5e9a80f27299','apps/derby/derby_scene.js','function hud(tx) {\n','\nfunction at(u) {'),
 'photo59':('7da4f9087eedef69ea54063bb9a2c721e8ead8da','af99783c02d0984f229708d985a51e3aebd71c97','apps/derby/derby_scene.js','function hud(tx) {\n','\nfunction at(u) {'),
 'rock63':('af99783c02d0984f229708d985a51e3aebd71c97','bd77c6aa9bd22fefd1edc1f4b5a7e367095b9d98','apps/bigwave/big_wave.js','  function draws(D) {\n','\n  const V = P && pocket.kasane;'),
 'plain66':('4b5a4c5d4eed2ca9d1a78ce5d0103e4e58c452d3','32910d4a114c16d9f065676e85528f172562f0c7','main/ui/kasane/ksn_procedural.c','static inline __attribute__((always_inline)) void plain_band(const ksn_proc_segment *s,uint16_t *pixels,int y,int height){\n','/* One pixel of a pattern line: cell u >> 8 of the pattern. */'),
}

def require(condition,message):
 if not condition:raise ValueError(message)

def section(data,start,end):
 start,end=start.encode(),end.encode()
 require(data.count(start)==1 and data.count(end)==1,'scope markers must be unique')
 a=data.index(start);b=data.index(end)
 require(a<b,'scope markers are out of order')
 return data[:a],data[a:b],data[b:]

def reverse_located(before,after,current,start,end):
 bp,bs,bq=section(before,start,end);ap,as_,aq=section(after,start,end)
 cp,cs,cq=section(current,start,end)
 require(bp==ap and bq==aq,'accepted patch changes outside the named scope')
 old=bs.splitlines(keepends=True);new=as_.splitlines(keepends=True)
 live=cs.splitlines(keepends=True)
 groups=list(difflib.SequenceMatcher(None,old,new,autojunk=False).get_grouped_opcodes(3))
 require(bool(groups),'accepted candidate patch is empty')
 hunks=[]
 for group in groups:
  a0,a1=group[0][1],group[-1][2];b0,b1=group[0][3],group[-1][4]
  old_block=b''.join(old[a0:a1]);new_block=b''.join(new[b0:b1])
  require(old_block and new_block,'hunk needs nonempty context on both sides')
  require(current.count(new_block)==1,'candidate context is missing or duplicated')
  require(live[b0:b1]==new[b0:b1],'candidate hunk moved within the named scope')
  hunks.append((a0,a1,b0,b1,old_block,new_block))
 baseline_lines=live[:]
 for a0,a1,b0,b1,old_block,new_block in reversed(hunks):
  baseline_lines[b0:b1]=old[a0:a1]
 baseline=cp+b''.join(baseline_lines)+cq
 forward=baseline_lines[:]
 for a0,a1,b0,b1,old_block,new_block in reversed(hunks):
  require(baseline.count(old_block)==1,'baseline context is ambiguous')
  require(forward[a0:a1]==old[a0:a1],'baseline hunk position mismatch')
  forward[a0:a1]=new[b0:b1]
 require(cp+b''.join(forward)+cq==current,'content roundtrip differs')
 positions=[{'before_lines':[a0+1,a1],'after_lines':[b0+1,b1],
             'context_sha256':hashlib.sha256(nb).hexdigest()}
            for a0,a1,b0,b1,ob,nb in hunks]
 return baseline,positions

def isolate(root,before,after,path,start,end,output):
 root=Path(root).resolve();output=Path(output).resolve()
 require(not os.environ.get('GIT_INDEX_FILE'),'inherited alternate index is unsupported')
 def git(*args,env=None,data=None):
  return subprocess.check_output(['git',*args],cwd=root,env=env,input=data)
 def clean():
  require(not git('status','--porcelain=v1','--untracked-files=no'),
          'tracked worktree or index is dirty')
 clean()
 require(git('rev-parse','--show-toplevel').decode().strip()==str(root),'use checkout root')
 relative_output=output.relative_to(root).as_posix()
 require(not git('ls-files','--',relative_output),'report would overwrite a tracked file')
 head=git('rev-parse','HEAD').decode().strip()
 head_tree=git('rev-parse','HEAD^{tree}').decode().strip()
 git('merge-base','--is-ancestor',after,head)
 metadata=[]
 for ref in (before,after,head):
  entry=git('ls-tree',ref,'--',path).decode().split()
  require(len(entry)==4 and entry[0]=='100644' and entry[1]=='blob' and entry[3]==path,
          'expected one regular tracked source file')
  metadata.append(entry[2])
 previous,accepted,current=[git('show',ref+':'+path) for ref in (before,after,head)]
 baseline_bytes,positions=reverse_located(previous,accepted,current,start,end)
 with tempfile.TemporaryDirectory(prefix='fixture-index-') as directory:
  env=dict(os.environ,GIT_INDEX_FILE=str(Path(directory)/'index'))
  git('read-tree',head,env=env)
  blob=git('hash-object','-w','--stdin',data=baseline_bytes).decode().strip()
  git('update-index','--cacheinfo','100644',blob,path,env=env)
  tree=git('write-tree',env=env).decode().strip()
  changed=git('diff','--name-only',head_tree,tree).decode().splitlines()
  require(changed==[path],'comparison changes unexpected paths')
  git('update-index','--cacheinfo','100644',metadata[2],path,env=env)
  require(git('write-tree',env=env).decode().strip()==head_tree,'tree roundtrip differs')
  clean()
  require(git('rev-parse','HEAD').decode().strip()==head,'HEAD moved during isolation')
  require(git('write-tree').decode().strip()==head_tree,'real index changed')
  env.update(GIT_AUTHOR_NAME='Fixture baseline',GIT_AUTHOR_EMAIL='fixture@example.invalid',
             GIT_COMMITTER_NAME='Fixture baseline',GIT_COMMITTER_EMAIL='fixture@example.invalid')
  baseline=git('commit-tree',tree,'-p',head,env=env,
               data=b'Local comparison baseline; never publish this commit\n').decode().strip()
 report={'version':2,'source_head':head,'source_tree':head_tree,'accepted_before':before,
         'accepted_after':after,'path':path,'scope_start':start,'scope_end':end,
         'baseline_commit':baseline,'baseline_tree':tree,'source_blobs':metadata,
         'baseline_blob':blob,'hunks':positions,'roundtrip_tree_equal':True,
         'tracked_worktree_and_index_clean':True,'untracked_support_allowed':True}
 output.parent.mkdir(parents=True,exist_ok=True)
 output.write_text(json.dumps(report,indent=2)+'\n')
 return baseline

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('case',choices=CASES);p.add_argument('--output',type=Path,required=True)
 a=p.parse_args();print(isolate(Path.cwd(),*CASES[a.case],a.output))
