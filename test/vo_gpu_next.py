#!/usr/bin/env python3
"""Test gpu-next mapping ownership and motion cadence with production functions.

The fixture stubs GPU/codec APIs, not the comparisons, lifecycle or motion clock.
It can run on a host without a GPU; it does not test Android buffer interop.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def extract_function(source, name):
    match = re.search(r'^[\w *]+\b' + name + r'\([^;{]*\)\s*\{',
                      source, re.M)
    if not match:
        raise ValueError(f'Missing function: {name}')
    end, depth = match.end(), 1
    # These functions contain no braces in strings or comments.
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def extract_structure(source, name):
    start = source.index('struct ' + name + ' {')
    return source[start:source.index('\n};', start) + 3]


def generate(source, contracts, functions):
    params = (source / 'video/mp_image.h').read_text(encoding='utf-8')
    hwdec = (source / 'video/out/gpu/hwdec.h').read_text(encoding='utf-8')
    image = (source / 'video/mp_image.c').read_text(encoding='utf-8')
    video = (source / 'video/out/vo_gpu_next.c').read_text(encoding='utf-8')
    draw = extract_function(video, 'draw_frame')
    if not re.search(r'struct pl_render_params params = pars->params;\s*'
                     r'params.source_parameters = NULL;\s*'
                     r'params.num_source_parameters = 0;', draw):
        raise ValueError('draw_frame must reset local source parameter bindings')
    motion = (source / 'video/out/gpu_next/motion.h').read_text(encoding='utf-8')
    vo = (source / 'video/out/vo.h').read_text(encoding='utf-8')
    command = (source / 'player/command.c').read_text(encoding='utf-8')
    properties = (source / 'options/m_property.h').read_text(encoding='utf-8')
    contracts.write_text('\n\n'.join((
        extract_structure(params, 'mp_image_params'),
        '\n'.join(re.search(r'^bool ' + name + r'\([^;]+;', params,
                            re.M).group()
                  for name in ('mp_image_params_equal',
                               'mp_image_params_static_equal')),
        extract_structure(hwdec, 'ra_hwdec_mapper'),
        extract_structure(video, 'hwdec_slot'),
        extract_structure(video, 'hwdec_snapshot'),
        extract_structure(video, 'source_frame'),
        extract_structure(video, 'motion_mix'),
        re.search(r'^enum mp_motion_result \{.*?^\};', motion,
                  re.M | re.S).group(),
        re.search(r'^enum mp_motion_domain \{.*?^\};', motion,
                  re.M | re.S).group(),
        extract_structure(motion, 'mp_motion_frame'),
        extract_structure(motion, 'mp_motion_stats'),
        extract_structure(vo, 'voctrl_motion_info'),
        re.search(r'^enum motion_state \{.*?^\};', video, re.M | re.S).group(),
        '\n'.join(re.search(r'^#define SUB_PROP_' + name + r'\([^\n]*\) \\\n[^\n]*',
                            properties, re.M).group()
                  for name in ('INT', 'INT64', 'STR', 'BOOL')),
        '#define E(x, ...) static const char *const event_ ## x[] = {__VA_ARGS__, NULL};\n' +
        '\n'.join(re.search(r'\bE\(' + name + r',.*?\)', command, re.S).group()
                  for name in ('MPV_EVENT_TICK', 'MPV_EVENT_VIDEO_RECONFIG',
                               'MP_EVENT_CORE_IDLE')) + '\n#undef E\n',
    )), encoding='utf-8')
    functions.write_text('\n\n'.join(
        extract_function(text, name) for text, names in (
            (image, ('mp_image_params_equal', 'mp_image_params_static_equal')),
            (video, ('slot_unmap', 'slot_reconfig', 'slot_release_owner',
                     'slot_uninit', 'slot_preload', 'slot_acquire', 'slot_release',
                     'setup_hwdec_plane_mapping', 'describe_hwdec_frame',
                     'destroy_hwdec_snapshot', 'unref_source_frame',
                     'get_source_frame', 'source_needs_snapshot',
                     'prune_source_frames', 'clear_source_frames',
                     'snapshot_hwdec_frame',
                     'free_frame', 'clear_pending_frame', 'reset_frame_queue',
                     'update_seek_reset', 'source_crop', 'source_peak_acquire',
                     'update_hook_opts_dynamic', 'view_acquire', 'view_release', 'reset_source_peak',
                     'update_source_policy', 'submit_source_frames',
                     'preload_deferred_hwdec_frame', 'unmap_frame', 'discard_frame',
                     'prune_pending_frame', 'queue_frame',
                     'capture_motion_frame', 'prepare_motion_frame',
                     'select_motion_pair', 'motion_views_supported',
                     'mix_motion_views', 'source_context', 'render_source_mix',
                     'motion_interpolation_needed', 'motion_output_pts',
                     'motion_backend_failed', 'motion_state_name', 'get_motion_info',
                     'report_motion_stats', 'update_motion_options')),
            (command, ('mp_property_motion_info',)),
        ) for name in names
    ), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mpv', type=Path, required=True)
    parser.add_argument('--cc', nargs='+', default=['cc'])
    parser.add_argument('--generate', nargs=2, type=Path)
    args = parser.parse_args()
    if args.generate:
        generate(args.mpv, *args.generate)
        return

    fixture = Path(__file__).with_suffix('.c').resolve()
    with tempfile.TemporaryDirectory(prefix='mpv-gpu-next-') as directory:
        work = Path(directory)
        generate(args.mpv, work / 'vo_gpu_next_contracts.h',
                 work / 'vo_gpu_next_functions.h')
        executable = work / ('test.exe' if os.name == 'nt' else 'test')
        compiler = Path(args.cc[0]).stem.lower()
        if compiler in ('cl', 'clang-cl'):
            flags = ['/nologo', '/std:c11', '/W4', '/WX', '/wd4100', f'/I{work}',
                     str(fixture), f'/Fe:{executable}', f'/Fo:{work / "test.obj"}']
            if compiler == 'clang-cl':
                flags += ['/clang:-Wno-sign-compare']
        else:
            # Match mpv's warning policy for the extracted production code.
            flags = ['-std=c11', '-Wall', '-Wextra', '-Werror',
                     '-Wno-sign-compare', '-Wno-unused-parameter', '-I', str(work),
                     str(fixture), '-o', str(executable)]
            if os.name != 'nt':
                flags += ['-lm']
        subprocess.run(args.cc + flags, cwd=work, check=True)
        subprocess.run([str(executable)], cwd=work, check=True)


if __name__ == '__main__':
    main()
