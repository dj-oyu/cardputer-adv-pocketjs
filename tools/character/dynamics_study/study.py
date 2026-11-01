"""Host-only binary64 reference; no firmware binding or device performance claim."""
import argparse
import csv
import hashlib
import json
import math
import random
from pathlib import Path
import struct

from signals import LutDrive, FREQUENCIES, WEIGHTS

L1 = L2 = 35.0
KA = (2 * math.pi * 1.2) ** 2
KC = (2 * math.pi * 1.8) ** 2
CA = 2 * .7 * 2 * math.pi * 1.2
CC = 2 * .7 * 2 * math.pi * 1.8


class AnalyticDrive(LutDrive):
    def step(self):
        value = sum(weight * math.sin(phase * math.tau / (1 << 32))
                    for phase, weight in zip(self.phases, self.weights))
        for i in range(3):
            self.phases[i] = (self.phases[i] + self.increments[i]) & 0xffffffff
        return value


def tip(a, b):
    return L1 * math.sin(a) + L2 * math.sin(b), L1 * math.cos(a) + L2 * math.cos(b)


class Model:
    def __init__(self, kind='soft', compensate=False, inertia=1., drive=True,
                 step_hz=60, seed=42, lut_size=64, limit=16., events=True,
                 coupling_hz=1.8, coupling_zeta=.7, analytic=False, external_drive=False):
        if kind not in ('rigid', 'soft', 'cascade') or type(step_hz) is not int or step_hz <= 0:
            raise ValueError('kind and positive integer step_hz required')
        if not all(math.isfinite(x) for x in (inertia, limit, coupling_hz, coupling_zeta)) or min(inertia, limit, coupling_hz) <= 0 or coupling_zeta < 0:
            raise ValueError('positive finite inertia/limit/frequency; nonnegative damping required')
        self.kind, self.compensate, self.inertia = kind, compensate, inertia
        self.hz, self.h = step_hz, 1 / step_hz
        self.drive_on, self.limit, self.events = drive, limit, events
        self.signal = None if external_drive else (AnalyticDrive if analytic else LutDrive)(lut_size, seed, step_hz)
        self.n = 0
        self.q0 = self.q1 = self.v0 = self.v1 = 0.
        self.goal = 0.
        self.last_force = 0.
        self.rx = self.vx = self.vy = self.tx = 0.
        self.ry = self.ty = L1 + L2
        self.event_log = []
        h = self.h
        self.set_coupling(coupling_hz, coupling_zeta)
        wr = 2 * math.pi * 1.5
        self.ref_a = 1 / (1 + 2 * .8 * wr * h + wr * wr * h * h)
        self.ref_b = h * wr * wr * self.ref_a

    def set_coupling(self, frequency, damping):
        # Configuration boundary only; preserve current q/v.
        if not math.isfinite(frequency) or not math.isfinite(damping) or frequency <= 0 or damping < 0:
            raise ValueError('positive finite frequency and nonnegative damping required')
        kc = (2 * math.pi * frequency) ** 2
        cc = 2 * damping * 2 * math.pi * frequency
        h = self.h
        a00 = 1 + h * (CA + cc) + h * h * (KA + kc)
        a01 = -h * cc - h * h * kc
        a11 = self.inertia + h * cc + h * h * kc
        determinant = a00*a11-a01*a01
        if not math.isfinite(determinant) or determinant <= 0:
            raise ValueError('finite positive matrix determinant required')
        self.kc, self.cc, self.a00, self.a01, self.a11, self.inv = kc, cc, a00, a01, a11, 1/determinant

    def energy(self):
        return .5 * (self.v0 ** 2 + self.inertia * self.v1 ** 2 +
                     KA * (self.q0 - self.goal) ** 2 + self.kc * (self.q1 - self.q0) ** 2)

    def state(self):
        return (self.q0, self.q1, self.v0, self.v1, self.rx, self.ry, self.vx, self.vy)

    def step(self, drive_value=None):
        self.n += 1
        if self.events and self.n in (2 * self.hz, 6 * self.hz, 8 * self.hz):
            before = self.state()
            self.goal, self.tx, self.ty = {
                2 * self.hz: (.15, 4., 69.),
                6 * self.hz: (-.08, -4., 69.),
                8 * self.hz: (0., 0., 70.)}[self.n]
            self.event_log.append(('retarget', self.n, before, self.state()))
        if self.events and self.n == 4 * self.hz:
            before = self.state()
            self.v0 += .8
            if self.kind == 'rigid':
                # Equal angular momentum impulse for the locked compound body.
                self.v0 = before[2] + .8 / (1 + self.inertia)
                self.v1 = self.v0
            self.event_log.append(('impulse', self.n, before, self.state()))
        drive = self.signal.step() if drive_value is None else drive_value
        force = 2. * drive if self.drive_on else 0.
        self.last_force = force
        h, m, kc, cc = self.h, self.inertia, self.kc, self.cc
        if self.kind == 'rigid':
            mass = 1 + m
            self.v0 = (mass * self.v0 + h * (force - KA * (self.q0 - self.goal))) / (mass + h * CA + h * h * KA)
            self.q0 += h * self.v0
            self.q1, self.v1 = self.q0, self.v0
        elif self.kind == 'cascade':
            # Deliberately one-way visual control, not physical mutual coupling.
            self.v0 = (self.v0 + h * (force - KA * (self.q0 - self.goal))) / (1 + h * CA + h * h * KA)
            self.q0 += h * self.v0
            self.v1 = (m * self.v1 + h * (kc * (self.q0 - self.q1) + cc * self.v0)) / (m + h * cc + h * h * kc)
            self.q1 += h * self.v1
        else:
            e0, e1 = self.q0 - self.goal, self.q1 - self.goal
            r0 = self.v0 + h * (force - (KA + kc) * e0 + kc * e1)
            r1 = m * self.v1 + h * (kc * e0 - kc * e1)
            self.v0 = (self.a11 * r0 - self.a01 * r1) * self.inv
            self.v1 = (self.a00 * r1 - self.a01 * r0) * self.inv
            self.q0 += h * self.v0
            self.q1 += h * self.v1
        self.vx = self.ref_a * self.vx + self.ref_b * (self.tx - self.rx)
        self.vy = self.ref_a * self.vy + self.ref_b * (self.ty - self.ry)
        self.rx += h * self.vx
        self.ry += h * self.vy
        return self.record(force)

    def record(self, force=None):
        force = self.last_force if force is None else force
        x, y = tip(self.q0, self.q1)
        cx = cy = 0.
        limited = False
        if self.compensate:
            cx, cy = self.rx - x, self.ry - y
            length = math.hypot(cx, cy)
            limited = length > self.limit
            if limited:
                cx *= self.limit / length
                cy *= self.limit / length
        return dict(step=self.n, time=self.n / self.hz, q0=self.q0, q1=self.q1,
                    v0=self.v0, v1=self.v1, force=force, raw_x=x, raw_y=y,
                    geom_x=(L1 + L2) * math.sin(self.q0),
                    dynamic_x=L2 * (math.sin(self.q1) - math.sin(self.q0)),
                    base_x=cx, base_y=cy, tip_x=x + cx, tip_y=y + cy,
                    reference_x=self.rx, reference_y=self.ry, reference_vx=self.vx, reference_vy=self.vy,
                    goal_x=self.tx, goal_y=self.ty, goal_angle=self.goal,
                    goal_error=math.hypot(x + cx - self.tx, y + cy - self.ty),
                    residual=math.hypot(x + cx - self.rx, y + cy - self.ry),
                    limited=int(limited), energy=self.energy())


def run(config, fps=30, seconds=16, pause=True):
    model = Model(**config)
    wall_hz = 720
    assert wall_hz % fps == 0
    pause_start, pause_end = int(5.5 * wall_hz), int(6.5 * wall_hz)
    end = int((seconds + int(pause)) * wall_hz)
    trace, frames = [model.record()], []
    paused_states = []
    for wall in range(0, end + 1, wall_hz // fps):
        paused_ticks = max(0, min(wall, pause_end) - pause_start) if pause else 0
        due = (wall - paused_ticks) * model.hz // wall_hz
        while model.n < due:
            trace.append(model.step())
        record = model.record()
        frames.append(dict(record, wall_time=wall / wall_hz))
        if pause and pause_start <= wall <= pause_end:
            paused_states.append((model.state(), model.signal.snapshot(), model.n))
    assert model.n == seconds * model.hz
    assert not paused_states or all(x == paused_states[0] for x in paused_states)
    return model, trace, frames


def rms(values):
    values = list(values)
    return math.sqrt(sum(x * x for x in values) / len(values))


def summary(config, trace):
    tail = [r for r in trace if r['time'] >= 12]
    return dict(config=config, max_angle=max(max(abs(r['q0']), abs(r['q1'])) for r in trace),
                max_speed=max(max(abs(r['v0']), abs(r['v1'])) for r in trace),
                tip_x_rms=rms(r['raw_x'] for r in tail),
                geometric_x_rms=rms(r['geom_x'] for r in tail),
                dynamic_delta_x_rms=rms(r['dynamic_x'] for r in tail),
                relative_angle_rms=rms(r['q1'] - r['q0'] for r in tail),
                max_residual=max(r['residual'] for r in trace),
                max_requested_goal_error=max(r['goal_error'] for r in trace),
                tail_residual_rms=rms(r['residual'] for r in tail),
                limited_steps=sum(r['limited'] for r in trace),
                final_angle=max(abs(trace[-1]['q0']), abs(trace[-1]['q1'])),
                final_speed=max(abs(trace[-1]['v0']), abs(trace[-1]['v1'])))


def digest(trace):
    return hashlib.sha256(b''.join(struct.pack('<8d', *(r[k] for k in
        ('q0', 'q1', 'v0', 'v1', 'reference_x', 'reference_y', 'reference_vx', 'reference_vy'))) for r in trace)).hexdigest()


def asymmetric(order='forward'):
    common = LutDrive(64, 42, 60)
    models = dict(left=Model(external_drive=True, events=False, coupling_hz=.9, coupling_zeta=.35),
                  right=Model(external_drive=True, events=False, inertia=1.6, coupling_hz=1.3, coupling_zeta=.6))
    traces = {name: [model.record()] for name, model in models.items()}
    goals = {'left': {120: .15, 360: -.08, 480: 0.},
             'right': {180: -.11, 420: .04, 540: 0.}}
    for step in range(1, 961):
        signal = common.step()  # One immutable same-step input for both jobs.
        names = ['left', 'right']
        if order == 'reverse' or (order == 'alternating' and step % 2):
            names.reverse()
        for name in names:
            model = models[name]
            if step in goals[name]:
                before = model.state()
                model.goal = goals[name][step]
                if step == (360 if name == 'left' else 420):
                    model.set_coupling(1.1 if name == 'left' else .8, .9 if name == 'left' else .45)
                assert before == model.state()
            traces[name].append(model.step(signal * (1 if name == 'left' else .65)))
    return models, traces, common.snapshot()


def dof_order_probe(order, broken_in_place=False):
    """Asymmetric two-DOF gather from immutable old state; a deliberate negative control."""
    h, m0, m1, kc, cc = 1 / 60, .7, 1.6, KC, CC
    a00 = m0 + h*(CA+cc) + h*h*(KA+kc)
    a01 = -h*cc-h*h*kc
    a11 = m1+h*cc+h*h*kc
    det = a00*a11-a01*a01
    rows = ((a11/det, -a01/det), (-a01/det, a00/det))
    q, v = [.19, -.11], [.08, -.04]
    records = []
    rng = random.Random(42)  # Scheduling test only, never a motion drive.
    for step in range(720):
        g = (.13, -.04) if step < 180 else (-.06, .11)
        old_q, old_v = tuple(q), tuple(v)
        schedule = list(range(2))
        if order == 'reverse': schedule.reverse()
        if order == 'shuffle': rng.shuffle(schedule)
        for i in schedule:
            source_q, source_v = (q, v) if broken_in_place else (old_q, old_v)
            e0, e1 = source_q[0]-g[0], source_q[1]-g[1]
            rhs = (m0*source_v[0]+h*(.3-(KA+kc)*e0+kc*e1),
                   m1*source_v[1]+h*(-.1+kc*e0-kc*e1))
            v[i] = rows[i][0]*rhs[0]+rows[i][1]*rhs[1]
            q[i] = source_q[i]+h*v[i]
        records.append(tuple(q+v))
    return records


def verify():
    results = {}
    configs = dict(rigid=dict(kind='rigid'), reference=dict(kind='soft'),
                   lively=dict(coupling_hz=.9, coupling_zeta=.35), cascade=dict(kind='cascade'))
    for name, parameters in configs.items():
        config = dict(parameters, compensate=True)
        baseline, trace, _ = run(config, fps=30)
        for fps in (15, 60, 120):
            other, candidate, _ = run(config, fps=fps)
            assert digest(candidate) == digest(trace)
            assert other.state() == baseline.state()
            assert other.signal.snapshot() == baseline.signal.snapshot()
        for event, step, before, after in baseline.event_log:
            if event == 'retarget':
                assert before == after, (event, step)
            else:
                assert before[:2] == after[:2]
        results[name + '_cadence_replay_pause'] = 'PASS'
    # Backward Euler energy decay, without target/forcing/input work.
    energy_checks = 0
    strict_failures = []
    convergence = []
    for hz in (30, 60, 120):
        for inertia in (1., 2.):
            for link_hz, link_zeta in ((1.8, .7), (.9, .35)):
                model = Model(step_hz=hz, inertia=inertia, drive=False, events=False,
                              coupling_hz=link_hz, coupling_zeta=link_zeta)
                model.q0, model.q1, model.v0, model.v1 = .2, -.1, .1, -.2
                previous = model.energy()
                for _ in range(hz * 20):
                    model.step()
                    current = model.energy()
                    assert current <= previous + 1e-12
                    previous = current
                    energy_checks += 1
                residual = max(abs(model.q0), abs(model.q1))
                speed = max(abs(model.v0), abs(model.v1))
                item = dict(hz=hz, inertia=inertia, coupling_hz=link_hz,
                            angle_after_20s=residual, speed_after_20s=speed)
                convergence.append(item)
                if residual >= 1e-8:
                    strict_failures.append(item)
                assert residual < 1e-3 and speed < 1e-3
    results['unforced_energy_checks'] = energy_checks
    results['drive_off_20s_residuals'] = convergence
    results['strict_20s_1e8_angle_probe'] = dict(status='FAIL' if strict_failures else 'PASS', failures=strict_failures)
    # Same drive, different child inertia must feed back into the root.
    _, normal, _ = run(dict(inertia=1.))
    _, heavy, _ = run(dict(inertia=2.))
    results['heavy_changes_root_max_rad'] = max(abs(a['q0'] - b['q0']) for a, b in zip(normal, heavy))
    assert results['heavy_changes_root_max_rad'] > .001
    _, cascade_normal, _ = run(dict(kind='cascade', inertia=1.))
    _, cascade_heavy, _ = run(dict(kind='cascade', inertia=2.))
    assert all(a['q0'] == b['q0'] for a, b in zip(cascade_normal, cascade_heavy))
    results['one_way_cascade_has_no_child_reaction'] = 'CONFIRMED'
    # A display compensation must not change physical angles or velocities.
    _, free, _ = run(dict(compensate=False))
    _, held, _ = run(dict(compensate=True))
    assert digest(free) == digest(held)
    assert max(r['residual'] for r in held) < .001
    _, limited, _ = run(dict(compensate=True, limit=.2))
    assert any(r['limited'] for r in limited) and max(r['residual'] for r in limited) > 1
    results['compensation_state_invariance_and_limited_residual'] = 'PASS'
    settling_failures = []
    for drive in (False, True):
        for inertia in (1., 2.):
            for frequency, damping in ((1.8, .7), (.9, .35)):
                _, trace, _ = run(dict(drive=drive, inertia=inertia, coupling_hz=frequency, coupling_zeta=damping))
                assert all(math.isfinite(x) for r in trace for x in r.values())
                assert max(max(abs(r['q0']),abs(r['q1'])) for r in trace) < .5
                assert max(max(abs(r['v0']),abs(r['v1'])) for r in trace) < 4.
                if not drive:
                    angle, speed = abs(trace[-1]['q1']), abs(trace[-1]['v1'])
                    if angle >= .0001 or speed >= .0001:
                        settling_failures.append(dict(inertia=inertia,coupling_hz=frequency,
                                                      angle_after_16s=angle,speed_after_16s=speed))
    results['finite_and_drive_on_bounded'] = 'PASS (declared fixture, not universal displacement bound)'
    results['eventful_16s_1e4_settling_probe'] = dict(status='FAIL' if settling_failures else 'PASS',failures=settling_failures)
    _, branches, phase = asymmetric()
    for order in ('reverse', 'alternating'):
        _, reordered, other_phase = asymmetric(order)
        assert phase == other_phase
        for name in branches:
            assert digest(branches[name]) == digest(reordered[name])
    assert any(abs(a['q1'] + b['q1']) > .01 for a, b in zip(branches['left'], branches['right']))
    results['asymmetric_branch_schedule_permutation'] = 'PASS (no threads or hardware speedup measured)'
    ordered = dof_order_probe('forward')
    assert ordered == dof_order_probe('reverse') == dof_order_probe('shuffle')
    wrong_a, wrong_b = dof_order_probe('forward', True), dof_order_probe('reverse', True)
    gap = max(abs(a-b) for va,vb in zip(wrong_a,wrong_b) for a,b in zip(va,vb))
    assert gap > 1e-4
    results['asymmetric_dof_gather_order'] = 'PASS exact forward/reverse/seeded shuffle, 720 steps'
    results['deliberate_in_place_order_difference'] = gap
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=Path('.cache/dynamics-study'))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    tests = verify()
    cases = dict(rigid=dict(kind='rigid'), reference=dict(kind='soft'),
                 soft=dict(kind='soft', coupling_hz=.9, coupling_zeta=.35),
                 held=dict(kind='soft', compensate=True, coupling_hz=.9, coupling_zeta=.35), heavy=dict(kind='soft', inertia=2.),
                 cascade=dict(kind='cascade'), limited=dict(compensate=True, limit=.2, coupling_hz=.9, coupling_zeta=.35),
                 settled=dict(drive=False, inertia=2.))
    summaries = {}
    animation = {}
    for name, config in cases.items():
        _, trace, frames = run(config)
        summaries[name] = summary(config, trace)
        with (args.out / (name + '.csv')).open('w', newline='') as output:
            writer = csv.DictWriter(output, fieldnames=trace[0].keys())
            writer.writeheader()
            writer.writerows(trace)
        if name in ('rigid', 'reference', 'soft', 'held'):
            animation[name] = frames
        # Isolate step response without the multi-frequency drive or later kick.
        _, response, _ = run(dict(config, drive=False))
        onset = [r for r in response if 2 <= r['time'] < 4]
        for field in ('q0', 'q1'):
            crossing = next((r['time'] - 2 for r in onset if r[field] >= .075), None)
            summaries[name]['step_t50_' + field + '_s'] = crossing
        summaries[name]['pre_kick_q1_overshoot_rad'] = max(0, max(r['q1'] for r in onset) - .15)
    _, branches, _ = asymmetric()
    for name, trace in branches.items():
        with (args.out / (name + '.csv')).open('w', newline='') as output:
            writer = csv.DictWriter(output, fieldnames=trace[0].keys())
            writer.writeheader(); writer.writerows(trace)
    refinement = {}
    traces = {hz: run(dict(step_hz=hz))[1] for hz in (30, 60, 120)}
    for hz in (30, 60):
        pairs = [(r, traces[120][r['step'] * 120 // hz]) for r in traces[hz]]
        refinement[str(hz)] = dict(tip_error_rms=rms(math.hypot(a['raw_x'] - b['raw_x'], a['raw_y'] - b['raw_y']) for a, b in pairs),
                                   q_error_max=max(abs(a['q1'] - b['q1']) for a, b in pairs))
    result = dict(kind='HOST SIMULATION / NOT DEVICE PERFORMANCE', step_hz=60,
                  status='EXPLORATORY; strict settling probes include failures; not a production acceptance',
                  integrator='full backward Euler, piecewise-constant coefficients; inverse refreshed at config boundaries; binary64 host reference',
                  signal=dict(lut_size=64,seed=42,frequencies_hz=FREQUENCIES,weights=WEIGHTS),
                  coefficients=dict(ka=KA, kc=KC, ca=CA, cc=CC, I0=1., L1=L1, L2=L2),
                  tests=tests, cases=summaries, refinement_vs_120hz=refinement)
    _, reference, _ = run(dict(coupling_hz=.9, coupling_zeta=.35, analytic=True))
    lut_errors = {}
    for size in (32, 64, 256):
        _, candidate, _ = run(dict(coupling_hz=.9, coupling_zeta=.35, lut_size=size))
        lut_errors[size] = dict(max_angle_error=max(abs(a['q1']-b['q1']) for a,b in zip(candidate,reference)),
                               max_tip_error=max(math.hypot(a['raw_x']-b['raw_x'],a['raw_y']-b['raw_y']) for a,b in zip(candidate,reference)))
    result['lut_propagated_error_vs_same_phase_analytic'] = lut_errors
    result['source_sha256'] = {p.name:hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in sorted(Path(__file__).parent.iterdir()) if p.suffix in ('.py','.ps1')}
    (args.out / 'results.json').write_text(json.dumps(result, indent=2), encoding='utf8')
    (args.out / 'animation.json').write_text(json.dumps(animation), encoding='utf8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
