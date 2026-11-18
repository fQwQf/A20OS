"""Bounded, framed diagnostic evidence collection on QEMU or a physical UART."""
from __future__ import annotations
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shlex
import subprocess
import time
import uuid
from dataclasses import asdict
from a20_error import A20Error
from a20_console import ConsoleError
from a20_make import REPO_ROOT


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def parse_jsonl(text, kind):
    rows = [json.loads(line) for line in text.splitlines() if line.strip()]
    if not rows or rows[0].get('kind') != kind or rows[0].get('schema') != 1:
        raise ValueError('missing or unsupported report header')
    if rows[-1].get('kind') != 'end' or rows[-1].get('complete') is not True:
        raise ValueError('incomplete report')
    header, footer = rows[0], rows[-1]
    if not isinstance(header.get('boot'),str) or not header['boot']:
        raise ValueError('missing report boot identity')
    if not isinstance(header.get('ticks_per_second'), int) or header['ticks_per_second'] <= 0:
        raise ValueError('invalid clock frequency')
    if kind == 'porting':
        checks = [r for r in rows[1:-1] if r.get('kind') == 'check']
        if len(checks) != footer.get('checks') or len(checks) != header.get('retained'):
            raise ValueError('check count mismatch')
        if len({r['id'] for r in checks}) != len(checks):
            raise ValueError('duplicate check id')
        states = ('untested', 'running', 'pass', 'fail', 'unsupported', 'degraded', 'deferred')
        if any(r.get('state') not in states for r in rows[1:-1]):
            raise ValueError('invalid check/phase state')
        return {'kind': kind, 'boot': header['boot'], 'loss': {k: header[k] for k in ('overwritten','dropped','late_finishes')},
                'states': {s: sum(r['state'] == s for r in checks) for s in states},
                'longest_checks': sorted((dict(r, elapsed_ms=r['elapsed_ticks'] * 1000 / header['ticks_per_second'])
                                          for r in checks if r.get('clock_valid')), key=lambda r: r['elapsed_ticks'], reverse=True)[:20],
                'phases': [r for r in rows[1:-1] if r.get('kind') == 'phase'],
                'readiness': 'probe and checkpoint results only; functional test status comes from workload exit status'}
    cpus = [r for r in rows if r.get('kind') == 'cpu']
    events = [r for r in rows if r.get('kind') == 'event']
    if len(cpus) != header.get('cpus') or len({r['cpu'] for r in cpus}) != len(cpus):
        raise ValueError('CPU count mismatch')
    if len(events) != footer.get('events') or len(events) != sum(r['retained'] for r in cpus):
        raise ValueError('event count mismatch')
    for cpu in cpus:
        seq = [r['seq'] for r in events if r['cpu'] == cpu['cpu']]
        if len(seq) != cpu['retained'] or (seq and seq != list(range(seq[0], seq[0] + len(seq)))):
            raise ValueError('per-CPU sequence gap')
    categories = {}
    for row in rows:
        if row.get('kind') != 'latency':
            continue
        buckets = row['log2_buckets']
        if len(buckets) != 32 or any(not isinstance(n, int) or n < 0 for n in buckets) or sum(buckets) != row['samples']:
            raise ValueError('invalid latency histogram')
        key = str(row['category'])
        out = categories.setdefault(key, {'samples': 0, 'errors': 0, 'elapsed_ticks': 0, 'max_ticks': 0, 'log2_buckets': [0] * 32})
        out['samples'] += row['samples']; out['errors'] += row.get('errors',0); out['elapsed_ticks'] += row['elapsed_ticks']
        out['max_ticks'] = max(out['max_ticks'],row['max_ticks'])
        out['log2_buckets'] = [a+b for a,b in zip(out['log2_buckets'],buckets)]
    for out in categories.values():
        out['mean_elapsed_ms'] = out['elapsed_ticks'] * 1000 / header['ticks_per_second'] / out['samples']
        out['max_elapsed_ms'] = out['max_ticks'] * 1000 / header['ticks_per_second']
    begins = {r['id']: r for r in events if (r['event'] & 255) == 1}
    spans = []
    for event in events:
        if (event['event'] & 255) != 2 or event['id'] not in begins:
            continue
        start = begins[event['id']]
        if (start['event'] >> 8) != (event['event'] >> 8):
            raise ValueError('span category mismatch')
        result = event['a'] if event['a'] < 1 << 63 else event['a'] - (1 << 64)
        spans.append({'category': event['event'] >> 8, 'id': event['id'],
                      'begin_cpu': start['cpu'], 'end_cpu': event['cpu'], 'a': start['a'], 'b': start['b'],
                      'result': result, 'elapsed_ms': event['b'] * 1000 / header['ticks_per_second']})
    spans.sort(key=lambda r:r['elapsed_ms'], reverse=True)
    return {'kind': kind, 'boot': header['boot'], 'generation': header['generation'], 'active': header['active'],
            'loss': {k: sum(r[k] for r in cpus) for k in ('overwritten','rejected','clock_regressions')},
            'unsupported_cpu': header['unsupported_cpu'], 'events': len(events), 'latency': categories,
            'matched_retained_spans': len(spans), 'longest_retained_spans': spans[:20],
            'units': 'elapsed arch timer ticks, includes waits/preemption; not CPU time',
            'ordering': header['clock']}


def parse_locks(text):
    if text.strip() == 'lock_metrics_compiled: 0':
        return {'compiled': False}
    footer=re.search(r'^lock_counters: registered=(\d+) capacity=(\d+) dropped=(\d+)\n?$',text,re.M)
    if not footer or footer.end() != len(text):
        raise ValueError('missing lock report completion footer')
    rows=[]
    for line in text[:footer.start()].splitlines():
        if line.startswith('  '): continue  # optional callsite samples
        match=re.fullmatch(r'([^:]+): (\d+) (\d+) max=(\d+)',line)
        if not match: raise ValueError('invalid lock total row')
        rows.append({'name':match[1],'contended':int(match[2]),'spins':int(match[3]),'max_spins':int(match[4])})
    registered,capacity,dropped=map(int,footer.groups())
    if len(rows) != registered or registered > capacity:
        raise ValueError('lock registration count mismatch')
    return {'compiled':True,'registered':registered,'dropped':dropped,'locks':rows,
            'units':'spin loop iterations, not elapsed ticks; live cumulative counters'}


def lock_delta(before,after):
    if not before.get('compiled') or not after.get('compiled'):
        return {'status':'unavailable'}
    def unique(data):
        out={}
        for index,row in enumerate(data['locks']):
            out[(index,row['name'])]=row
        return out
    left,right=unique(before),unique(after)
    rows=[]
    for name in left.keys() & right.keys():
        row={'registry_id':name[0],'name':name[1]}
        for field in ('contended','spins'):
            delta=right[name][field]-left[name][field]
            if delta < 0: raise ValueError('lock counters reset while collecting')
            row[field]=delta
        # A cumulative maximum cannot be subtracted into a window maximum.
        row['cumulative_max_spins']=right[name]['max_spins']
        rows.append(row)
    rows.sort(key=lambda r:r['spins'],reverse=True)
    return {'status':'live_delta','top_locks':rows[:20],
            'new_locks_without_baseline':[f'{i}:{name}' for i,name in sorted(right.keys()-left.keys())],
            'registration_drops':after['dropped'],
            'units':'spin loop iterations; snapshots are live and include collection overhead'}


class Session:
    """One command per completed frame; bounded output, deadline, UART queries."""
    def __init__(self, transport, timeout=120, prompt=rb'(?:^|[\r\n])# ', limit=32*1024*1024):
        self.transport, self.timeout, self.prompt = transport, timeout, re.compile(prompt)
        self.raw = bytearray(); self.limit = limit; self.raw_truncated = False
        self.commands = []
        self.pending_query = b''

    def read(self, timeout):
        chunk = self.transport.read(timeout)
        if chunk:
            # Respond even when the terminal status query crosses read chunks.
            query = self.pending_query + chunk
            for _ in range(query.count(b'\x1b[6n')):
                self.transport.write(b'\x1b[1;3R')
            self.pending_query = query[-3:]
            remaining = self.limit - len(self.raw)
            self.raw.extend(chunk[:max(0, remaining)])
            if len(chunk) > remaining:
                self.raw_truncated = True
        return chunk

    def settle(self, deadline):
        # A prompt can precede BusyBox's cursor-position exchange. Wait for
        # quiet after replying, otherwise the next line can join that reply.
        while time.monotonic() < deadline:
            if not self.read(min(.25, max(0, deadline-time.monotonic()))):
                return
        raise TimeoutError('console did not settle after shell prompt')

    def ready(self):
        deadline = time.monotonic() + self.timeout
        output = bytearray()
        # An already-running physical shell may need a newline to show prompt.
        self.transport.write(b'\n')
        while time.monotonic() < deadline:
            output.extend(self.read(.2))
            if self.prompt.search(output[-65536:]):
                self.settle(deadline)
                return
            if len(output) > 1024*1024:
                del output[:-65536]
        raise TimeoutError('shell prompt not reached')

    def execute(self, command, timeout=None):
        token = 'A20E_' + uuid.uuid4().hex
        start, end = token + '_BEGIN', token + '_END'
        # Split marker text in shell source so terminal echo cannot satisfy it.
        quote = lambda s: "'" + s[:5] + "''" + s[5:] + "'"
        wire = (f"printf '\\n%s\\n' {quote(start)}; {{ {command}; }}; a20_e_rc=$?; "
                f"printf '\\n%s:%s\\n' {quote(end)} \"$a20_e_rc\"\n")
        record = {'command': command, 'status': 'incomplete', 'exit_code': None}
        self.commands.append(record)
        begin = time.monotonic()
        self.transport.write(wire.encode())
        deadline = begin + (timeout or self.timeout)
        output = bytearray()
        pattern = re.compile(rb'(?:^|\n)' + end.encode() + rb':([0-9]+)\r?\n')
        while time.monotonic() < deadline:
            output.extend(self.read(.2))
            if len(output) > 4*1024*1024:
                record['status'] = 'oversize'
                raise ValueError('command output exceeds 4 MiB bound')
            match = pattern.search(output)
            marker = re.search(rb'(?:^|\n)' + start.encode() + rb'\r?\n', output)
            if match and marker and self.prompt.search(output[match.end():]):
                self.settle(deadline)
                code = int(match.group(1))
                record.update(status='complete', exit_code=code, elapsed_seconds=time.monotonic()-begin)
                return output[marker.end():match.start()].decode(errors='replace').replace('\r',''), code
        record.update(status='timeout', elapsed_seconds=time.monotonic()-begin)
        raise TimeoutError('command timed out: ' + command)


class PipeTransport:
    def __init__(self, command):
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, start_new_session=True)
    def read(self, timeout):
        if select.select([self.proc.stdout], [], [], timeout)[0]:
            chunk = os.read(self.proc.stdout.fileno(),65536)
            if not chunk and self.proc.poll() is not None:
                raise EOFError(f'QEMU exited with {self.proc.returncode}')
            return chunk
        return b''
    def write(self, data, timeout=10):
        self.proc.stdin.write(data); self.proc.stdin.flush()
    def close(self):
        from a20_test import _reap
        _reap(self.proc)
        self.proc.stdin.close(); self.proc.stdout.close()


def collect(session, output, manifest, workload=None, *, prepared=False):
    """Always leave a manifest and raw transcript, including partial failures."""
    output.mkdir(parents=True, exist_ok=prepared)
    manifest.update(schema=1, status='incomplete', workload=workload, artifacts={}, commands=session.commands)
    def save():
        (output/'serial.log').write_bytes(session.raw)
        manifest['serial_truncated'] = session.raw_truncated
        manifest['serial_sha256'] = sha256(output/'serial.log')
        (output/'manifest.json').write_text(json.dumps(manifest,indent=2,default=str)+'\n')
    def artifact(name, command, kind=None):
        text, code = session.execute(command)
        path = output/name; path.write_text(text)
        item = {'command_exit_code': code, 'sha256': sha256(path), 'status': 'available' if code == 0 else 'unavailable'}
        manifest['artifacts'][name] = item
        feature={'porting.jsonl':'porting','boot-report.txt':'boot_report','trace.jsonl':'trace','boot-trace.jsonl':'trace'}.get(name)
        if code and feature and manifest.get('capabilities',{}).get(feature) == 0:
            item['status']='not_compiled'
        if code == 0 and kind:
            try:
                if kind == 'capabilities':
                    capabilities=json.loads(text)
                    if capabilities.get('kind') != 'capabilities' or capabilities.get('schema') != 1 or not capabilities.get('complete'):
                        raise ValueError('invalid capability descriptor')
                    item['analysis']=capabilities
                    manifest['capabilities']=capabilities
                else:
                    item['analysis'] = parse_locks(text) if kind == 'locks' else parse_jsonl(text, kind)
            except (KeyError, TypeError, ValueError) as exc:
                item.update(status='invalid', error=str(exc))
        save()
        return text, code
    save()
    try:
        session.ready()
        artifact('capabilities.json','cat /proc/a20/observability','capabilities')
        artifact('identity.txt','uname -a; cat /proc/version; cat /proc/cmdline; cat /proc/cpuinfo')
        artifact('porting.jsonl','cat /proc/a20/porting_report','porting')
        artifact('boot-report.txt','cat /proc/a20/boot_report')
        artifact('drivers.txt','cat /proc/a20/driver_lifecycle')
        artifact('interrupts-before.txt','cat /proc/interrupts')
        artifact('locks-before.txt','cat /proc/a20/lock_contention','locks')
        # Freeze boot history before replacing the window for a workload.
        session.execute('echo stop > /proc/a20/trace')
        artifact('boot-trace.jsonl','cat /proc/a20/trace','trace')
        if workload:
            manifest['measurement_setup'] = {}
            for feature, command in (('perf','echo stop > /proc/a20/perf && echo start > /proc/a20/perf'),
                                     ('trace','echo start 63 > /proc/a20/trace')):
                _, code = session.execute(command)
                manifest['measurement_setup'][feature] = code
            artifact('perf-before.txt','cat /proc/a20/perf')
            text, code = artifact('workload.txt','sh -c '+shlex.quote(workload))
            manifest['workload_exit_code'] = code
        session.execute('echo stop > /proc/a20/perf')
        session.execute('echo freeze > /proc/a20/trace')
        artifact('perf-after.txt','cat /proc/a20/perf')
        artifact('trace.jsonl','cat /proc/a20/trace','trace')
        artifact('interrupts-after.txt','cat /proc/interrupts')
        artifact('locks-after.txt','cat /proc/a20/lock_contention','locks')
        artifact('memory.txt','cat /proc/meminfo; cat /proc/pressure/cpu; cat /proc/pressure/memory; cat /proc/pressure/io')
        artifact('dmesg.txt','dmesg')
        if workload and manifest['measurement_setup']['perf'] == 0:
            import importlib.util
            spec = importlib.util.spec_from_file_location('a20_perf_analysis',REPO_ROOT/'tools/bench/a20-perf-diff.py')
            module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
            try:
                manifest['performance'] = module.diff(module.parse_snapshot((output/'perf-before.txt').read_text()),
                                                       module.parse_snapshot((output/'perf-after.txt').read_text()))
            except ValueError as exc:
                manifest['performance_error'] = str(exc)
        try:
            left=manifest['artifacts']['locks-before.txt'].get('analysis',{})
            right=manifest['artifacts']['locks-after.txt'].get('analysis',{})
            manifest['lock_contention']=lock_delta(left,right)
        except ValueError as exc:
            manifest['lock_contention']={'status':'invalid','error':str(exc)}
        boot_ids={a.get('analysis',{}).get('boot') for a in manifest['artifacts'].values() if a.get('analysis',{}).get('boot')}
        if len(boot_ids) > 1:
            manifest['identity_error']='reports span more than one boot'
        invalid = any(a['status'] == 'invalid' for a in manifest['artifacts'].values())
        unavailable = any(a['status'] in ('unavailable','not_compiled') for a in manifest['artifacts'].values())
        manifest['status'] = ('incomplete' if 'identity_error' in manifest else 'failed' if manifest.get('workload_exit_code',0) else
                              'partial' if invalid or unavailable or manifest.get('lock_contention',{}).get('status') == 'invalid' or session.raw_truncated or 'performance_error' in manifest or
                              any(code != 0 for code in manifest.get('measurement_setup',{}).values()) else 'complete')
    except (OSError, ConsoleError, ValueError, EOFError, TimeoutError, KeyboardInterrupt) as exc:
        manifest.update(status='incomplete', error=str(exc) or type(exc).__name__)
    finally:
        save()
    return 0 if manifest['status'] == 'complete' else 1


def run(args, inst):
    from a20_derive import derive_make_vars
    from a20_console import open_transport
    from a20_test import _exclusive, _qemu_cmdline
    kernel = args.kernel.resolve() if args.kernel else None
    if args.timeout <= 0 or args.timeout > 86400:
        raise A20Error('--timeout must be in (0,86400] seconds per command')
    if args.serial and kernel:
        raise A20Error('--kernel applies to QEMU; physical boards use their running image')
    manifest = {'instance': inst.name, 'arch': inst.arch, 'created_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
                'instance_source': str(inst.source), 'instance_sha256': sha256(inst.source),
                'instance_config': asdict(inst), 'derived_make_vars': derive_make_vars(inst),
                'provenance': 'requested config and supplied image; identity.txt records guest identity; not build attestation'}
    try:
        manifest['source_commit'] = subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO_ROOT,text=True).strip()
        manifest['source_dirty'] = bool(subprocess.check_output(['git','status','--porcelain'],cwd=REPO_ROOT))
    except (OSError,subprocess.CalledProcessError):
        manifest['source_commit'] = 'unknown'
    output = args.output.resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        raise A20Error(f'evidence destination already exists: {output}') from None
    manifest.update(schema=1,status='incomplete',artifacts={},commands=[])
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2,default=str)+'\n')
    try:
        with _exclusive(inst.name), contextlib.ExitStack() as stack:
            if args.serial or (inst.target.serial and not kernel):
                manifest.update(transport='serial', device=inst.target.serial, baud=inst.target.baud or 115200)
                transport = open_transport(inst)
            else:
                if kernel:
                    from a20_qemu_image import qemu_command
                    command = stack.enter_context(qemu_command(inst.arch,kernel,512))
                else:
                    command = _qemu_cmdline(inst)
                    if '-kernel' in command:
                        kernel = Path(command[command.index('-kernel')+1])
                manifest.update(transport='qemu', qemu_command=command)
                if kernel and kernel.is_file():
                    manifest.update(kernel=str(kernel), kernel_sha256=sha256(kernel))
                from a20_resource import Policy, Requirement, gate, parse_memory_mb
                policy = Policy.from_env()
                mem = command[command.index('-m')+1] if '-m' in command else '512M'
                vcpus = int(command[command.index('-smp')+1].split(',')[0]) if '-smp' in command else 1
                gate(Requirement(parse_memory_mb(mem)+policy.reserve_mem_mb, vcpus, policy.min_disk_mb),
                     policy, REPO_ROOT, wait=False, label='diagnostic evidence collection')
                transport = PipeTransport(command)
            stack.callback(transport.close)
            result = collect(Session(transport,args.timeout,args.prompt.encode()),output,manifest,args.workload,prepared=True)
            print(f"{manifest['status']}: {output / 'manifest.json'}")
            return result
    except (OSError, ConsoleError, A20Error, ValueError, KeyboardInterrupt) as exc:
        manifest.update(status='incomplete',error=str(exc) or type(exc).__name__)
        (output/'manifest.json').write_text(json.dumps(manifest,indent=2,default=str)+'\n')
        print(f"incomplete: {output / 'manifest.json'}")
        return 1


def compare_evidence(before_path, after_path, *, allow_config_change=False):
    """Compare completed workload windows across boots; verify artifact hashes.

    Within-run snapshot identity checks still apply. Cross-run values are
    compared in seconds, never by subtracting unrelated cumulative snapshots.
    """
    import importlib.util
    spec = importlib.util.spec_from_file_location('a20_perf_comparison',REPO_ROOT/'tools/bench/a20-perf-diff.py')
    analysis = importlib.util.module_from_spec(spec); spec.loader.exec_module(analysis)
    def load(path):
        path=Path(path)
        if path.is_dir(): path=path/'manifest.json'
        manifest=json.loads(path.read_text())
        if manifest.get('schema') != 1 or manifest.get('status') not in ('complete','partial'):
            raise ValueError('evidence is not a completed collection: '+str(path))
        if not manifest.get('workload') or manifest.get('workload_exit_code') != 0:
            raise ValueError('comparison requires a successful explicit workload')
        for name in ('perf-before.txt','perf-after.txt','workload.txt'):
            item=manifest['artifacts'][name]
            if item.get('command_exit_code') != 0 or sha256(path.parent/name) != item.get('sha256'):
                raise ValueError('missing, failed or changed artifact: '+name)
        perf=analysis.diff(analysis.parse_snapshot((path.parent/'perf-before.txt').read_text()),
                           analysis.parse_snapshot((path.parent/'perf-after.txt').read_text()))
        if not perf.get('window') or not perf['window']['snapshot_frozen']:
            raise ValueError('comparison requires schema 2 frozen windows')
        return path,manifest,perf
    left,lm,lp=load(before_path);right,rm,rp=load(after_path)
    for key in ('workload','arch','transport'):
        if lm.get(key) != rm.get(key): raise ValueError('comparison context changed: '+key)
    def platform(m):
        if m.get('transport') == 'serial':
            return (m.get('device'),m['instance_config']['kernel'].get('board'))
        cmd=m.get('qemu_command',[])
        return tuple((flag,cmd[cmd.index(flag)+1] if flag in cmd else None)
                     for flag in ('-machine','-cpu','-m','-smp','-accel'))
    if platform(lm) != platform(rm):
        raise ValueError('target configuration changed; use separate device baselines')
    def instrumentation(m):
        config=m['instance_config']['log']
        defaults=dict(enabled=True,console=True,persistent=True,level='info',metrics=True,
                      trace=False,trace_records=128,trace_mask=63,trace_boot=False,
                      porting_report=False,porting_records=128,report='off',report_window_ms=10000,
                      report_settle_ms=500,report_max_records=48,report_context=True,mode='auto',
                      max_bytes=8388608,session=True)
        policy={k:config.get(k) if config.get(k) is not None else v for k,v in defaults.items()}
        policy['boot_report']=config.get('boot_report') if config.get('boot_report') is not None else policy['report'] in ('error','always')
        return {'compiled':m.get('capabilities'),'requested_policy':policy}
    changed=instrumentation(lm) != instrumentation(rm)
    if changed and not allow_config_change:
        raise ValueError('instrumentation changed; pass --allow-config-change for an explicitly labelled comparison')
    def values(perf):
        frequency=perf['ticks_per_second']
        return {k:(v/frequency if k.endswith('_ticks') else v) for k,v in perf['counters'].items()}
    lv,rv=values(lp),values(rp)
    if lv.keys() != rv.keys():
        # A syscall unused in one run is a zero row, not a changed metric schema.
        regular=lambda v:{k for k in v if not re.fullmatch(r'syscall_[0-9]+_(count|ticks)',k)}
        if regular(lv) != regular(rv): raise ValueError('metric schema changed')
    counters={}
    for key in sorted(lv.keys()|rv.keys()):
        a,b=lv.get(key,0),rv.get(key,0)
        counters[key]={'before':a,'after':b,'change_percent':(b-a)*100/a if a else None,
                       'unit':'elapsed_seconds' if key.endswith('_ticks') else 'count'}
    return {'schema':1,'before':str(left),'after':str(right),'workload':lm['workload'],
            'arch':lm['arch'],'instrumentation_changed':changed,
            'kernel_sha256_before':lm.get('kernel_sha256'),'kernel_sha256_after':rm.get('kernel_sha256'),
            'counters':counters,
            'notes':['Within-run frozen deltas are compared across boots after clock conversion.',
                     'Elapsed sums include waits, concurrency and collection commands; they are not CPU time or workload throughput.',
                     'Repeat runs and compare workload timing separately before attributing a change to an optimization.']}
