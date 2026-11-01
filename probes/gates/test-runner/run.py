"""Exercise vyxc test through the real CLI, including failure and isolation paths."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import shutil
import threading


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--output',type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    gate = Path(__file__).resolve().parent
    output = args.output.resolve() if args.output else gate / '.cache'
    output.mkdir(parents=True,exist_ok=True)
    env = dict(os.environ)
    env['PATH'] = str(compiler.parent) + os.pathsep + env.get('PATH', '')
    records = []
    report_lock=threading.Lock()
    with tempfile.TemporaryDirectory(prefix='vyxc test space ') as temp:
        root = Path(temp)

        def project(name, sources):
            directory = root / name
            for rel, text in sources.items():
                path = directory / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding='utf-8')
            return directory

        def check(name, cwd, arguments, expected=0, contains=(), absent=(), kind='test', codegen_units=None):
            command = [str(compiler), kind, *map(str, arguments)]
            case_env=dict(env)
            if codegen_units is not None:case_env['VYX_CODEGEN_UNITS']=str(codegen_units)
            result = subprocess.run(command, cwd=cwd, env=case_env, capture_output=True, timeout=120)
            text = (result.stdout + result.stderr).decode('utf-8', errors='replace')
            valid_exit = result.returncode in expected if isinstance(expected,tuple) else result.returncode == expected
            passed = valid_exit and all(t in text for t in contains) and all(t not in text for t in absent)
            record = dict(name=name, command=command, exit=result.returncode, expected=expected,
                          passed=passed, codegen_units=codegen_units, stdout=result.stdout.decode(errors='replace'), stderr=result.stderr.decode(errors='replace'))
            with report_lock:
                records.append(record)
                (output/'verification.partial.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
            if not passed:
                shutil.copytree(cwd,output/'failures'/name,dirs_exist_ok=True,
                                ignore=shutil.ignore_patterns('*.exe','*.obj','*.lib','*.o','*.dll','*.dylib','*.so'))
            print(f'{name}: {"PASS" if passed else "FAIL"}', flush=True)
            if not passed:
                print(text, flush=True)
            return record

        good = project('good', {
            'src/unit.vyx': '@[test]\nfn unit_assertion() { assert(2 + 2 == 4); }\n',
            'tests/integers.vyx': 'module test_integer;\n@[test]\nfn integer_status() -> i32 { return 0; }\n@[test]\nfn void_explicit() -> void { assert(true); }\n',
            'tests/main.vyx': '@[test]\nfn alongside_main() -> i32 {return 0;}\nfn main()->i32 {return 9;}\n',
            'tests/helper.vyx': 'fn helper() -> i32 { return 42; }\n',
            'tests/.cache/should_skip.vyx': 'this is not valid Vyx',
            'tests/target/should_skip.vyx': '@[test]\nfn should_skip()->i32 {return 4;}',
        })
        check('list-default-src-and-tests',good,['--list'],contains=('unit_assertion','integer_status','void_explicit','alongside_main','test list: 4 tests'),absent=('should_skip',' ... ok'))
        check('run-void-i32-main',good,[],contains=('test result: ok. 4 passed; 0 failed',))
        check('exact-filter',good,['--filter','integer_status','--exact'],contains=('1 passed; 0 failed',),absent=('::unit_assertion','::void_explicit'))
        check('substring-filter',good,['--list','--filter','integer'],contains=('integer_status','void_explicit','test list: 2 tests'),absent=('alongside_main',))
        check('one-way-filter',good,['--filter','integer_status_extra'],1,contains=('no tests selected',))
        check('empty-filter-opt-in',good,['--filter','missing','--allow-empty'],contains=('no tests selected',))
        check('deduplicate-inputs',good,['--list','tests','tests/main.vyx','tests'],contains=('test list: 3 tests',))
        check('filter-full-identity',good,['--list','--exact','--filter',str(good/'tests/main.vyx').replace('\\','/')+'::alongside_main'],contains=('test list: 1 tests',))
        # cwd and the user's absolute spelling can cross a directory symlink
        # (/var -> /private/var on macOS). Use real filesystem aliases on every
        # host that permits them; hardlinks exercise distinct path spellings too.
        alias=root/'good-alias'
        try:
            alias.symlink_to(good,target_is_directory=True)
        except OSError as error:
            print(f'directory-symlink checks unavailable: {error}',flush=True)
        else:
            check('filter-through-directory-alias',good,['--list','--exact','--filter',str(alias/'tests/main.vyx').replace('\\','/')+'::alongside_main'],contains=('test list: 1 tests',))
            check('deduplicate-directory-alias',good,['--list',good/'tests',alias/'tests'],contains=('test list: 3 tests',))
            original=(good/'tests/main.vyx').read_bytes()
            for option in ('--report','--junit'):
                check('source-directory-alias-'+option[2:],good,[option,alias/'tests/main.vyx'],2,contains=('must not overwrite',))
                assert (good/'tests/main.vyx').read_bytes()==original
            check('new-output-directory-alias',good,['--report',good/'new.json','--junit',alias/'new.json'],2,contains=('must be distinct',))
        hardlink=root/'source-hardlink.vyx'
        os.link(good/'tests/main.vyx',hardlink)
        check('filter-through-hardlink',good,['--list','--exact','--filter',str(hardlink).replace('\\','/')+'::alongside_main'],contains=('test list: 1 tests',))
        check('deduplicate-file-hardlink',good,['--list',good/'tests/main.vyx',hardlink],contains=('test list: 1 tests',))
        original=(good/'tests/main.vyx').read_bytes()
        for option in ('--report','--junit'):
            check('source-hardlink-'+option[2:],good,[option,hardlink],2,contains=('must not overwrite',))
            assert (good/'tests/main.vyx').read_bytes()==original
        check('help',good,['--help'],contains=('--list','--timeout-ms','--allow-empty'))
        for name,arguments in [
            ('unknown',['--filtter','hello']),('missing-filter',['--filter']),
            ('missing-timeout',['--timeout-ms']),('bad-timeout',['--timeout-ms=abc']),
            ('zero-timeout',['--timeout-ms=0']),('overflow-timeout',['--timeout-ms=999999999999999999999999']),
            ('exact-without-filter',['--exact']),('missing-path',['does-not-exist.vyx']),
            ('wrong-extension',['readme.txt']),('project-option-rejected',['--target','app']),
        ]:
            if name == 'wrong-extension': (good/'readme.txt').write_text('text')
            check(name,good,arguments,2,contains=('error: test:',),absent=('test result: ok',))
        empty = project('empty',{'tests/helper.vyx':'fn helper() {}'})
        check('no-tests-is-not-green',empty,[],1,contains=('no tests selected',))
        check('allow-no-tests',empty,['--allow-empty'])
        for name,code in [
            ('lex-error','@[test]\nfn bad() { let s = "unterminated; }'),
            ('parse-error','@[test]\nfn bad( { }'),
            ('parameter','@[test]\nfn bad(value:i32) -> i32 {return 0;}'),
            ('generic','@[test]\nfn bad<T>() -> i32 {return 0;}'),
            ('return-type','@[test]\nfn bad() -> bool {return true;}'),
            ('missing-body','@[test]\nfn bad() -> i32;'),
            ('method','class Bad { @[test] public fn bad(&self) -> i32 {return 0;} }'),
        ]:
            bad=project(name,{'tests/bad.vyx':code})
            check(name,bad,[],1,contains=('discovery errors','error:'),absent=('test result: ok',))
            if name == 'lex-error': check('lex-errors-cannot-allow-empty',bad,['--allow-empty'],1,contains=('discovery errors',))
        entry=project('entry',{'tests/app.vyx':'fn main() -> i32 {return 42;}\n@[test] fn calls_app_main()->i32 {if(main()==42){return 0;}return 1;}\n'})
        check('test-can-call-original-main',entry,[],contains=('1 passed; 0 failed',))
        entry_test=project('entry-test',{'tests/app.vyx':'@[test] fn main() -> i32 {return 0;}\n'})
        check('main-can-be-a-test',entry_test,[],contains=('1 passed; 0 failed',))
        false_marker=project('false-marker',{'tests/a.vyx':'@[tag(test)]\nfn fake() -> i32 {return 0;}\n'})
        check('attribute-argument-is-not-a-test',false_marker,['--list'],1,contains=('no tests selected',),absent=('::fake',))
        documented=project('documented',{'tests/a.vyx':'/// This note mentions @[test] but the function is ordinary.\nfn fake() -> i32 {return 0;}\n'})
        check('doc-attribute-is-not-a-test',documented,['--list'],1,contains=('no tests selected',),absent=('::fake',))
        documented_test=project('documented-test',{'tests/a.vyx':'/// A test\'s `code` (and parentheses) in its documentation.\n@[test] fn documented_test() {assert(true);}\n'})
        check('documented-test-has-no-spurious-diagnostics',documented_test,[],contains=('1 passed; 0 failed',),absent=('error:',))
        failed_main=project('failed-main',{'tests/a.vyx':'@[test] fn caught() -> i32 {return 1;}\nfn main()->i32{return 0;}\n'})
        check('failing-test-with-successful-main',failed_main,[],1,contains=('::caught ... FAILED','0 passed; 1 failed'))
        assertion=project('assertion',{'tests/a.vyx':'@[test] fn broken_assertion() { assert(false); }\n@[test] fn after_assertion() {assert(true);}'})
        check('assertion-failure-isolation',assertion,[],1,contains=('::broken_assertion ... FAILED','::after_assertion ... ok','1 passed; 1 failed'))
        errors=project('compile-error',{'tests/bad.vyx':'@[test]\nfn bad() -> i32 {return not_defined;}\n'})
        check('compile-error-not-pass',errors,['--junit','errors.xml'],1,contains=('compile failed:','1 build errors','1 not run'),absent=('test result: ok',))
        import xml.etree.ElementTree as ET
        error_xml=ET.parse(errors/'errors.xml').getroot()
        assert error_xml.get('errors')=='1' and error_xml.get('failures')=='0' and len(error_xml.findall('testcase'))==2
        mixed=project('mixed',{'tests/mixed.vyx': '''extern "C" { fn abort(); }
@[test] fn fails() -> i32 { return 1; }
@[test] fn high_status() -> i32 { return 256; }
@[test] fn crash() { abort(); }
@[test] fn after_crash() { assert(true); }
@[test] fn endless() { while (true) {} }
@[test] fn after_timeout() -> i32 { return 0; }
'''})
        check('failure-crash-timeout-isolation',mixed,['--timeout-ms','250'],1,contains=('::fails ... FAILED','::high_status ... FAILED','::crash ... FAILED','::after_crash ... ok','::endless ... TIMEOUT','::after_timeout ... ok','2 passed; 4 failed'))
        check('fail-fast',mixed,['--fail-fast'],1,contains=('::fails ... FAILED','5 not run','remaining selection not run'),absent=('::after_crash ...','::after_timeout ...'))
        named=project('names',{'tests/a.vyx':'@[test] fn same_name() -> i32 {return 0;}','tests/b.vyx':'@[test] fn same_name() -> i32 {return 0;}'})
        check('same-name-separate-files',named,[],contains=('2 passed; 0 failed',))
        ignored = project('ignored',{'tests/a.vyx':'@[ignore("external device required")] @[test] fn device()->i32 {return 1;}\n@[test] fn ordinary(){assert(true);}\n'})
        check('ignored-default',ignored,[],contains=('1 passed; 0 failed; 1 ignored',))
        check('ignored-only',ignored,['--ignored'],1,contains=('::device ... FAILED',),absent=('::ordinary ...',))
        check('include-ignored',ignored,['--include-ignored'],1,contains=('1 passed; 1 failed',))
        check('compile-without-run',mixed,['--no-run'],contains=('0 passed; 0 failed','6 not run'),absent=('... ok (','... FAILED','... TIMEOUT'))
        check('repeat-isolation',entry,['--repeat','3','-j2','--report','repeated.json','--junit','repeated.xml'],contains=('3 passed; 0 failed',))
        report=json.loads((entry/'repeated.json').read_text())
        import xml.etree.ElementTree as ET
        xml=ET.parse(entry/'repeated.xml').getroot()
        assert report['schema_version']==1 and len(report['results'])==3 and report['summary']['passed']==3
        assert len(xml.findall('testcase'))==3 and xml.get('failures')=='0'
        log_bytes=[65,0,66,255,195,169,239,191,190,240,159,152,128]
        binary=project('binary-logs',{'tests/a.vyx':
            'extern "C" { fn vyx_print_bytes_abi(value:rawptr,len:u64,newline:i32); }\n'
            '@[test] fn byte_output(){var bytes:[u8;13];'+
            ''.join(f'bytes[{i}]={v};' for i,v in enumerate(log_bytes))+
            'vyx_print_bytes_abi(to_rawptr(&bytes),13,0);}\n'})
        check('binary-log-keeps-nul-and-valid-utf8-reports',binary,['--report','binary.json','--junit','binary.xml'],contains=('1 passed; 0 failed',))
        binary_result=json.loads((binary/'binary.json').read_text())['results'][0]
        assert binary_result['stdout']=='A\x00B\ufffdé\ufffe😀',repr(binary_result['stdout'])
        assert (binary/binary_result['stdout_path']).read_bytes()==bytes(log_bytes)
        assert ET.parse(binary/'binary.xml').getroot().find('testcase/system-out').text=='AB\ufffdé😀'
        check('report-write-failure',entry,['--report',str(entry)],1,contains=('cannot write complete runner report',))
        platform = 'windows' if os.name=='nt' else ('macos' if __import__('sys').platform=='darwin' else 'linux')
        other = 'posix' if os.name=='nt' else 'windows'
        selected_platform=project('platform',{'tests/a.vyx':f'@[platform({platform})] @[test] fn host(){{assert(true);}}\n@[platform({other})] @[test] fn foreign(){{assert(false);}}'})
        check('host-platform-selection',selected_platform,[],contains=('1 passed; 0 failed',),absent=('::foreign',))
        manifest=project('manifest', {
            'Vyx.toml':'''[package]
name="application"
version="0.1.0"
entry="src/main.vyx"
[build]
output_dir="target"
cache_dir=".cache"
parallel_vyx=true
[target.application]
type="executable"
entry="src/main.vyx"
sources=["src/math.vyx","native/native.c"]
output_aliases=["keep_original"]
[target.source_library]
type="source"
entry="src/math.vyx"
sources=["native/native.c"]
[dependencies]
support={path="dependency",target="support"}
''',
            'src/main.vyx':'module application;\nuse application.math;\nfn main()->i32{return 9;}\n',
            'src/math.vyx':'''module application.math;
use std.string;
use support.numbers;
extern "C" { fn native_add(a:i32,b:i32)->i32; }
fn private_helper()->i32 {return native_add(answer(),2);}
public fn value()->i32{return private_helper();}
@[test] fn private_unit(){
    var builder=StringBuilder.create(); builder.append("42");
    let text=builder.build(); assert(text.as_str()=="42"); assert(private_helper()==42);
}
''',
            'native/native.c':'int native_add(int a,int b){return a+b;}\n',
            'dependency/Vyx.toml':'[package]\nname="support"\nversion="0.1.0"\n[target.support]\ntype="source"\nsources=["src/numbers.vyx"]\n',
            'dependency/src/numbers.vyx':'module support.numbers;\npublic fn answer()->i32{return 40;}\n',
            'tests/integration.vyx':'module application.integration;\nuse application.math;\nuse std.fs;\n@[test] fn integration(){assert(value()==42);assert(readFile("assets/data.txt")=="fixture");}\n',
            'assets/data.txt':'fixture',
        })
        (manifest/'target').mkdir()
        alias=manifest/'target'/('keep_original.exe' if os.name=='nt' else 'keep_original')
        alias.write_text('do not replace')
        check('project-native-source-dependency-private-unit',manifest,['-j2','--report','project.json'],contains=('2 passed; 0 failed',))
        assert alias.read_text()=='do not replace'
        check('project-source-library-target',manifest,['--target','source_library','--filter','private_unit','-j2'],contains=('1 passed; 0 failed',))
        check('build-jobs-independent-of-option-order',manifest,['--filter','private_unit','--build-jobs','8','-j2','--report','jobs.json'],contains=('1 passed; 0 failed',))
        jobs=json.loads((manifest/'jobs.json').read_text())
        prefix=jobs['results'][0]['stdout_path'].split('.case_')[0]
        assert '8 thread(s)' in (manifest/(prefix+'.compile.stdout')).read_text()
        check('project-warm-build',manifest,['-j2'],contains=('2 passed; 0 failed',))
        (manifest/'dependency/src/numbers.vyx').write_text('module support.numbers;\npublic fn answer()->i32{return 39;}\n')
        check('project-dependency-invalidation',manifest,['--filter','private_unit'],1,contains=('::private_unit ... FAILED',))
        bench=project('bench',{'benches/work.vyx':'''use std.benchmark;
var input:i64=17;
@[bench] fn arithmetic(){let x=black_box(input);let y=black_box(x*x+3);assert(y==292);}
fn main()->i32{return 9;}
'''})
        check('bench-help',bench,['--help'],contains=('--samples','--warmup-ms'),kind='bench')
        check('bench-list',bench,['--list'],contains=('::arithmetic','bench list: 1'),kind='bench')
        check('bench-real-sampling',bench,['--samples','5','--warmup-ms','10','--sample-ms','5','--report','bench.json'],contains=('ns/iter','samples 5','bench result: ok. 1 passed'),kind='bench')
        measurement=json.loads((bench/'bench.json').read_text())['results'][0]['benchmark']
        raw=measurement['raw']; assert len(raw)==5 and all(x['elapsed_ns']>0 and x['iterations']>0 for x in raw)
        import statistics,math
        values=[x['elapsed_ns']/x['iterations'] for x in raw]
        assert math.isclose(measurement['mean'],statistics.mean(values),rel_tol=1e-4)
        assert math.isclose(measurement['median'],statistics.median(values),rel_tol=1e-4)
        assert math.isclose(measurement['stddev'],statistics.stdev(values),rel_tol=1e-4,abs_tol=1e-6)
        assert measurement['min']<=measurement['median']<=measurement['p95']<=measurement['max']
        baseline_args=['--samples','5','--warmup-ms','10','--sample-ms','5']
        check('bench-baseline-comparison',bench,[*baseline_args,'--baseline','bench.json','--max-regression-percent','1000','--report','comparison.json'],contains=('baseline mean change','bench result: ok'),kind='bench')
        baseline=json.loads((bench/'bench.json').read_text())
        baseline['results'][0]['benchmark']['raw']=[{'iterations':1073741824,'elapsed_ns':1} for _ in range(5)]
        (bench/'fast.json').write_text(json.dumps(baseline))
        check('bench-regression-fails',bench,[*baseline_args,'--baseline','fast.json','--report','regression.json'],1,contains=('benchmark regression','0 passed; 1 failed'),kind='bench')
        (bench/'malformed.json').write_text('{"schema_version":1} trailing garbage')
        check('bench-invalid-baseline',bench,[*baseline_args,'--baseline','malformed.json'],2,contains=('invalid JSON document',),kind='bench')
        for label,mutate in [
            ('fractional-iterations',lambda d:d['results'][0]['benchmark']['raw'][0].update(iterations=1.5)),
            ('fractional-duration',lambda d:d['results'][0]['benchmark']['raw'][0].update(elapsed_ns=1.5)),
            ('declared-sample-count',lambda d:d['results'][0]['benchmark'].update(samples=999)),
            ('baseline-target',lambda d:d.update(target='different-target')),
        ]:
            doc=json.loads((bench/'bench.json').read_text());mutate(doc)
            (bench/'invalid.json').write_text(json.dumps(doc))
            check(label,bench,[*baseline_args,'--baseline','invalid.json'],2,contains=('benchmark baseline:',),absent=(' ... ok (',),kind='bench')
        payload=(bench/'bench.json').read_text()
        (bench/'duplicate.json').write_text(payload.replace('"schema_version":1','"schema_version":1,"schema_version":999',1))
        check('duplicate-baseline-keys',bench,[*baseline_args,'--baseline','duplicate.json'],2,contains=('duplicate JSON field',),kind='bench')
        doc=json.loads(payload);doc['kind']='bench\x00suffix'
        (bench/'decoded-nul.json').write_text(json.dumps(doc))
        check('decoded-nul-is-not-truncated',bench,[*baseline_args,'--baseline','decoded-nul.json'],2,contains=('NUL-containing baseline',),kind='bench')
        doc=json.loads(payload)
        doc['results'][0]['benchmark']['samples']=3
        doc['results'][0]['benchmark']['raw']=[{'iterations':1,'elapsed_ns':50},{'iterations':1,'elapsed_ns':100},{'iterations':1,'elapsed_ns':150}]
        (bench/'variable-reference.json').write_text(json.dumps(doc))
        check('threshold-scaled-baseline-variance',bench,['--samples','3','--warmup-ms','1','--sample-ms','1','--baseline','variable-reference.json','--max-regression-percent','100','--report','contrast.json'],(0,1),contains=('baseline mean change',),kind='bench')
        contrast=json.loads((bench/'contrast.json').read_text())['results'][0]
        current=[v['elapsed_ns']/v['iterations'] for v in contrast['benchmark']['raw']]
        margin=4.303*math.sqrt(statistics.variance(current)/3+4*2500/3)
        excess=statistics.mean(current)-200
        assert math.isclose(contrast['comparison']['margin_ns'],margin,rel_tol=1e-4)
        assert math.isclose(contrast['comparison']['threshold_contrast_ns'],excess,rel_tol=1e-4,abs_tol=1e-6)
        assert contrast['comparison']['regression'] is (excess>margin)
        assert len(contrast['comparison']['baseline_sha256'])==64
        multi=project('multi-bench',{
            f'benches/{name}.vyx':'use std.benchmark;\n@[bench] fn '+name+'(){let v=black_box(42);assert(v==42);}'
            for name in ('first','second','third')})
        check('multiple-benchmark-files',multi,['--samples','3','--sample-ms','1','--warmup-ms','1','--report','base.json'],contains=('3 passed; 0 failed',),kind='bench')
        check('baseline-cache-survives-file-arenas',multi,['--samples','3','--sample-ms','1','--warmup-ms','1','--baseline','base.json','--max-regression-percent','1000','--report','next.json'],contains=('3 passed; 0 failed',),kind='bench')
        multi_results=json.loads((multi/'next.json').read_text())['results']
        assert len(multi_results)==3 and all(r['comparison']['baseline_sha256']==hashlib.sha256((multi/'base.json').read_bytes()).hexdigest() for r in multi_results)
        before=(bench/'bench.json').read_bytes()
        check('baseline-output-collision',bench,[*baseline_args,'--baseline','bench.json','--report','bench.json'],2,contains=('must not overwrite',),kind='bench')
        assert (bench/'bench.json').read_bytes()==before
        bad_bench=project('failed-bench',{'benches/a.vyx':'@[bench] fn broken()->i32{return 256;}'})
        check('bench-status-cannot-wrap',bad_bench,['--samples','3','--warmup-ms','1','--sample-ms','1'],1,contains=('::broken ... FAILED',),kind='bench')
        check('bench-rejects-concurrent-measurement',bench,['-j2'],2,contains=('measurements run serially',),kind='bench')
        check('bench-invalid-samples',bench,['--samples','2'],2,contains=('error:',),kind='bench')
        dci=project('dci',{
            'api.vyx':'module runner_export;\n@[dci_export(symbol="runner_offset")] public fn offset(value:i32)->i32{return value+7;}\n',
            'src/main.vyx':'''module runner_dci;
use std.benchmark;
@[dci_import("../api.dcib")]
extern "dci" { fn offset(value:i32)->i32; }
@[test] fn contract_call(){assert(offset(35)==42);}
@[bench] fn contract_benchmark(){assert(offset(black_box(35))==42);}
fn main()->i32{return 9;}
''',
        })
        native_object=dci/('api.obj' if os.name=='nt' else 'api.o')
        for emit,outfile in [('dcib',dci/'api.dcib'),('obj',native_object)]:
            command=[str(compiler),'--src=file',str(dci/'api.vyx'),'--emit='+emit,'-o',str(outfile)]
            if emit=='obj':command.extend(['--no-process-entry','--export-all'])
            setup_env=dict(env,VYX_CODEGEN_UNITS='1')
            result=subprocess.run(command,cwd=dci,env=setup_env,capture_output=True,timeout=60)
            if result.returncode:raise RuntimeError(f'DCI producer {emit}: {result.stdout!r} {result.stderr!r}')
        (dci/'Vyx.toml').write_text('[package]\nname="consumer"\nversion="0.1.0"\nentry="src/main.vyx"\n[build]\ncache_dir=".cache/mixed/cache"\n[target.consumer]\ntype="executable"\nentry="src/main.vyx"\ndci_file="api.dcib"\nlibs=['+json.dumps(str(native_object).replace("\\","/"))+']\n')
        check('project-dci-contract-and-native-artifact',dci,['--target','consumer'],contains=('1 passed; 0 failed',),codegen_units=4)
        cgu_lists=list((dci/'.cache/mixed/cache/harness/consumer').glob('*.cgu.list'))
        assert any(len(p.read_text().splitlines())>1 for p in cgu_lists),'CGU split was not exercised'
        for listing in cgu_lists:
            for item in listing.read_text().splitlines():
                assert '.tmp' not in item and (dci/item).is_file(),item
        check('project-dci-benchmark',dci,['--target','consumer','--samples','3','--warmup-ms','1','--sample-ms','1'],contains=('bench result: ok. 1 passed','ns/iter'),kind='bench',codegen_units=4)
        parallel=project('parallel',{'tests/a.vyx':'use std.time;\n@[test] fn a(){sleep_ms(500);}\n@[test] fn b(){sleep_ms(500);}\n@[test] fn c(){sleep_ms(500);}\n@[test] fn d(){sleep_ms(500);}\n'})
        check('bounded-parallel-execution',parallel,['-j3','--report','parallel.json'],contains=('4 passed; 0 failed',))
        parallel_report=json.loads((parallel/'parallel.json').read_text())
        events=[]
        for item in parallel_report['results']:
            events.extend([(item['started_ms'],1),(item['started_ms']+item['elapsed_ms'],-1)])
        active=peak=0
        for _,delta in sorted(events):active+=delta;peak=max(peak,active)
        assert 2<=peak<=3,peak
        noisy=project('noisy',{'tests/a.vyx':'@[test] fn excessive(){var n:i32=0;while(n<10000){print("'+('x'*1024)+'");n=n+1;}}\n@[test] fn after_output(){assert(true);}\n'})
        check('output-limit-isolation',noisy,['--output-limit-kb','64','--report','noise.json','--junit','noise.xml'],1,contains=('output_limit','::after_output ... ok','1 passed; 1 failed'))
        noise_report=json.loads((noisy/'noise.json').read_text())
        assert noise_report['results'][0]['status']=='output_limit' and noise_report['summary']['failed']==1
        assert ET.parse(noisy/'noise.xml').getroot().get('failures')=='1'
        nul=project('nul-source',{'tests/a.vyx':'@[test] fn valid(){}\x00garbage'})
        check('source-read-cannot-truncate-at-nul',nul,['--allow-empty'],1,contains=('embedded NUL','discovery errors'))
        check('test-rejects-benchmark-only-options',good,['--samples','5'],2,contains=('benchmark option',))
        typed=project('typed-barriers',{'tests/a.vyx':'''use std.benchmark;
use std.string;
use std.collections;
@[test] fn owning_values(){
    let text=black_box(String.fromBytes("abc".c_str(),3)); assert(text.as_str()=="abc");
    var values=Vec::<i32>.with_capacity(2);values.push(42);
    let result=black_box(values);assert(result.get(0)==42);
}
'''})
        check('black-box-owning-string-and-vector',typed,[],contains=('1 passed; 0 failed',))
        scaling=project('scaling',{'tests/many.vyx':'\n'.join(f'@[test] fn case_{index:04}(){{assert(true);}}' for index in range(128))})
        check('many-tests-bounded-workers',scaling,['-j4','--report','many.json'],contains=('128 passed; 0 failed',))
        assert len(json.loads((scaling/'many.json').read_text())['results'])==128
        source=(entry/'tests/app.vyx');original=source.read_bytes()
        check('report-cannot-overwrite-source',entry,['--report',source],2,contains=('must not overwrite',))
        assert source.read_bytes()==original
        check('json-junit-path-collision',entry,['--report','same.json','--junit','same.json'],2,contains=('must be distinct',))
        native_only=project('native-only',{
            'Vyx.toml':'[package]\nname="native"\nversion="0.1.0"\n[target.native]\ntype="executable"\nentry="native.c"\n',
            'native.c':'int main(void){return 0;}\n',
            'src/unrelated.vyx':'@[test] fn unrelated(){assert(false);}',
        })
        check('native-target-does-not-scan-unrelated-src',native_only,['--list'],1,contains=('no tests selected',),absent=('::unrelated',))
        native_source=native_only/'native.c';original=native_source.read_bytes()
        check('report-cannot-overwrite-native-input',native_only,['--allow-empty','--report',native_source],2,contains=('must not overwrite',))
        assert native_source.read_bytes()==original
        native_alias=root/'native-alias.c';os.link(native_source,native_alias)
        check('report-cannot-overwrite-native-hardlink',native_only,['--allow-empty','--report',native_alias],2,contains=('must not overwrite',))
        assert native_source.read_bytes()==original
        with ThreadPoolExecutor(max_workers=2) as pool:
            list(pool.map(lambda index:check(f'concurrent-invocation-{index}',good,['--filter','alongside_main','--exact'],contains=('1 passed; 0 failed',)),range(2)))
    report=dict(compiler=str(compiler),compiler_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(),
                passed=sum(r['passed'] for r in records),total=len(records),records=records)
    (output/'verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(f'{report["passed"]}/{report["total"]} test runner checks passed',flush=True)
    if report['passed'] != report['total']:
        raise SystemExit(1)


if __name__=='__main__':
    main()
