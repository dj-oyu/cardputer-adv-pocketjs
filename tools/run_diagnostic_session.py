"""Host-check the actual firmware callback binding; never drives a device."""
from pathlib import Path
import os, subprocess, sys
root=Path(__file__).resolve().parents[1]
out=root/'.cache/diagnostic-session';out.mkdir(parents=True,exist_ok=True)
main=(root/'main/main.c').read_text()
a=main.index('static void *ksn_probe_acquire(')
b=main.index('static const diagnostic_lifecycle_ops ksn_probe_ops=',a)
(out/'diagnostic_main_callbacks.inc').write_text(main[a:b])
a=main.index('        bool diagnostic_admitted=')
b=main.index('\n#endif',a)
(out/'diagnostic_main_dispatch.inc').write_text(main[a:b]+'\n')
shell=(root/'main/ui/shell.c').read_text()
assert 'if(presented!=ESP_OK)return presented;' in shell
assert 'return shell_draw_impl(error,phase);' in shell
assert 'ESP_ERROR_CHECK(shell_draw_impl(error,phase));' in shell

# These checks verify the surrounding dispatch rather than silently testing a
# detached copy: quarantine must precede every normal UI owner operation.
loop=main[main.index('static void ui_task(void *arg)'):]
assert loop.index('==DIAG_CMD_TERMINAL')<loop.index('native_observe();')
assert 'atomic_exchange(&ksn_probe_requested' not in main
assert 'diagnostic_command_cancel(&ksn_probe_session.command)' in main
assert '!overlay_starting()&&!app_guest_present()&&!app_dormant_id()[0]' in main
flags=['-std=c11','-O2','-g','-Wall','-Wextra','-Werror']
if '--sanitize' in sys.argv:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
cmd=['cc',*flags,'-I'+str(out),str(root/'tools/test_diagnostic_session.c'),str(root/'main/ui/diagnostic_control.c'),'-o',str(out/'test')]
print(' '.join(cmd),flush=True);subprocess.run(cmd,check=True)
subprocess.run([str(out/'test')],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
