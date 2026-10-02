"""Static integrated-build safety contracts; no firmware, serial or downloads.

These do not substitute for the PowerShell parser/runtime tests in Test-Offline.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parent
BUILD = (ROOT / 'Build-Integrated.ps1').read_text()
COMMON = (ROOT / 'Common.ps1').read_text()


class IntegratedBuildGuards(unittest.TestCase):
    def test_exact_commit_is_required(self):
        self.assertRegex(BUILD, r"\[Parameter\(Mandatory=\$true\)\]\[ValidatePattern\('\^\[0-9a-fA-F\]\{40\}\$'\)\]\[string\]\$Commit")
        self.assertIn("@('rev-parse','--verify',\"$Commit^{commit}\")", BUILD)
        self.assertIn("if ($resolved -ne $Commit)", BUILD)
        self.assertIn("'worktree','add','--detach',$worktree,$Commit", BUILD)
        self.assertNotIn("'fetch'", BUILD)

    def test_plan_precedes_mutation_and_toolchain(self):
        plan = BUILD.index('if ($Plan)')
        self.assertLess(plan, BUILD.index('$python = Get-IdfPython'))
        self.assertLess(plan, BUILD.index('New-Item -ItemType Directory'))
        self.assertLess(plan, BUILD.index("'worktree','add'"))
        self.assertIn('RunRoot already exists', BUILD)
        self.assertIn('RunRoot must be outside', BUILD)
        self.assertIn('Assert-PlainPath $RunRoot', BUILD)
        self.assertNotIn('Remove-Item', BUILD)

    def test_private_config_and_explicit_disabled_experiments(self):
        self.assertIn("Join-Path $worktree 'build_integrated'", BUILD)
        self.assertIn('"-DSDKCONFIG=$build/sdkconfig"', BUILD)
        self.assertIn('"-DSDKCONFIG_DEFAULTS=$worktree/sdkconfig.defaults"', BUILD)
        self.assertIn("'-DCMAKE_C_FLAGS=-DPOCKET_VM_TYPED_PUT_INT_FAST=0'", BUILD)
        self.assertIn("'-DPOCKET_APP_PRECOMPILE_EXPERIMENT=OFF'", BUILD)
        self.assertNotIn('EnableTypedPut', BUILD)
        self.assertNotIn('EnablePrecompileExperiment', BUILD)
        self.assertIn('runtimePrecompileIntegrated=$false', BUILD)

    def test_configuration_is_checked_before_and_after_build(self):
        configure = BUILD.index("@('reconfigure')")
        compile_start = BUILD.index("@('build')")
        checks = [m.start() for m in re.finditer('Assert-IntegratedConfiguration \\$build', BUILD)]
        self.assertEqual(len(checks), 2)
        self.assertLess(configure, checks[0])
        self.assertLess(checks[0], compile_start)
        self.assertLess(compile_start, checks[1])
        for evidence in ('compile_commands.json', '^POCKET_APP_PRECOMPILE_EXPERIMENT:BOOL=OFF$',
                         '$flags.Count -ne 1', "$flags[0].Groups[1].Value -ne '0'",
                         'Unexpected diagnostic config', 'Unexpected F bytecode outputs'):
            self.assertIn(evidence, BUILD)

    def test_build_only_manifest_is_rejected_by_device_reader(self):
        self.assertIn("schema='pocketjs-integrated-build-v1'", BUILD)
        self.assertIn("manifestType='integrated-build-only'", BUILD)
        self.assertIn("$run.schema -ne 1", COMMON)
        for script in ('Flash-Validation.ps1', 'Test-Device.ps1'):
            body = (ROOT / script).read_text()
            self.assertLess(body.index('$run = Read-Run $RunRoot'), body.index('Get-IdfPython'))
            self.assertLess(body.index('$run = Read-Run $RunRoot'), body.index('Enter-DeviceLock'))

    def test_no_flash_serial_or_fault_runner(self):
        for forbidden in ('-Port', 'AllowFlash', 'esptool', 'Enter-DeviceLock',
                          'safe_smoke.py', 'smoke_device.py', 'stress_app.py',
                          'device_g12.py', 'device_reloc.py', "@('flash')", "@('monitor')"):
            self.assertNotIn(forbidden, BUILD)
        self.assertIn('tools/check_flash.py', BUILD)  # File-size gate, no port access.

    def test_logs_hashes_and_failure_manifest_are_preserved(self):
        for artifact in ('cardputer_pocketjs.bin', 'cardputer_pocketjs.elf',
                         'cardputer_pocketjs.map', 'sdkconfig', 'compile_commands.json',
                         'CMakeCache.txt', 'dependencies.lock'):
            self.assertIn(artifact, BUILD)
        self.assertIn('Get-FileHash -Algorithm SHA256', BUILD)
        self.assertIn("$manifest.status = 'failed'", BUILD)
        self.assertIn('finally { Write-Json $manifest $manifestPath }', BUILD)
        self.assertIn("$entry.configurationVerified = $true", BUILD)

    def test_pinned_y_comparison_is_separate(self):
        comparison = (ROOT / 'Build-Validation.ps1').read_text()
        self.assertIn("[string]$Candidate = 'Y'", comparison)
        self.assertIn('89a71ae72c721206dfbb63acfeffc74b38dde604', comparison)
        self.assertIn('c5a4993f399e57c1df31054373f1c42cf62b9e57', comparison)
        self.assertNotIn('Build-Integrated.ps1', comparison)
        self.assertNotIn('pocketjs-integrated-build-v1', comparison)


if __name__ == '__main__':
    unittest.main()
