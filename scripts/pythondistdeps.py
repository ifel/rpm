#!/usr/bin/python
# -*- coding: utf-8 -*-
#
# Copyright 2010 Per Øyvind Karlsen <proyvind@moondrake.org>
# Copyright 2015 Neal Gompa <ngompa13@gmail.com>
# Copyright 2020 SUSE LLC
#
# This program is free software. It may be redistributed and/or modified under
# the terms of the LGPL version 2.1 (or later).
#
# RPM python dependency generator, using .egg-info/.egg-link/.dist-info data
#

from __future__ import print_function
import argparse
from os.path import basename, dirname, isdir, sep
from sys import argv, stdin, version
from distutils.sysconfig import get_python_lib
from warnings import warn


class RpmVersion():
    def __init__(self, version_id):
        version = parse_version(version_id)
        if isinstance(version._version, str):
            self.version = version._version
        else:
            self.epoch = version._version.epoch
            self.version = list(version._version.release)
            self.pre = version._version.pre
            self.dev = version._version.dev
            self.post = version._version.post

    def increment(self):
        self.version[-1] += 1
        self.pre = None
        self.dev = None
        self.post = None
        return self

    def __str__(self):
        if isinstance(self.version, str):
            return self.version
        if self.epoch:
            rpm_epoch = str(self.epoch) + ':'
        else:
            rpm_epoch = ''
        while len(self.version) > 1 and self.version[-1] == 0:
            self.version.pop()
        rpm_version = '.'.join(str(x) for x in self.version)
        if self.pre:
            rpm_suffix = '~{}'.format(''.join(str(x) for x in self.pre))
        elif self.dev:
            rpm_suffix = '~{}'.format(''.join(str(x) for x in self.dev))
        elif self.post:
            rpm_suffix = '^post{}'.format(self.post[1])
        else:
            rpm_suffix = ''
        return '{}{}{}'.format(rpm_epoch, rpm_version, rpm_suffix)


def convert_compatible(name, operator, version_id):
    if version_id.endswith('.*'):
        print('Invalid requirement: {} {} {}'.format(name, operator, version_id))
        exit(65)  # os.EX_DATAERR
    version = RpmVersion(version_id)
    if len(version.version) == 1:
        print('Invalid requirement: {} {} {}'.format(name, operator, version_id))
        exit(65)  # os.EX_DATAERR
    upper_version = RpmVersion(version_id)
    upper_version.version.pop()
    upper_version.increment()
    return '({} >= {} with {} < {})'.format(
        name, version, name, upper_version)


def convert_equal(name, operator, version_id):
    if version_id.endswith('.*'):
        version_id = version_id[:-2] + '.0'
        return convert_compatible(name, '~=', version_id)
    version = RpmVersion(version_id)
    return '{} = {}'.format(name, version)


def convert_arbitrary_equal(name, operator, version_id):
    if version_id.endswith('.*'):
        print('Invalid requirement: {} {} {}'.format(name, operator, version_id))
        exit(65)  # os.EX_DATAERR
    version = RpmVersion(version_id)
    return '{} = {}'.format(name, version)


def convert_not_equal(name, operator, version_id):
    if version_id.endswith('.*'):
        version_id = version_id[:-2]
        version = RpmVersion(version_id)
        lower_version = RpmVersion(version_id).increment()
    else:
        version = RpmVersion(version_id)
        lower_version = version
    return '({} < {} or {} > {})'.format(
        name, version, name, lower_version)


def convert_ordered(name, operator, version_id):
    if version_id.endswith('.*'):
        # PEP 440 does not define semantics for prefix matching
        # with ordered comparisons
        version_id = version_id[:-2]
        version = RpmVersion(version_id)
        if '>' == operator:
            # distutils does not behave this way, but this is
            # their recommendation
            # https://mail.python.org/archives/list/distutils-sig@python.org/thread/NWEQVTCX5CR2RKW2LT4H77PJTEINSX7P/
            operator = '>='
            version.increment()
    else:
        version = RpmVersion(version_id)
    return '{} {} {}'.format(name, operator, version)


OPERATORS = {'~=': convert_compatible,
             '==': convert_equal,
             '===': convert_arbitrary_equal,
             '!=': convert_not_equal,
             '<=': convert_ordered,
             '<': convert_ordered,
             '>=': convert_ordered,
             '>': convert_ordered}


def convert(name, operator, version_id):
    return OPERATORS[operator](name, operator, version_id)


parser = argparse.ArgumentParser(prog=argv[0])
group = parser.add_mutually_exclusive_group(required=True)
group.add_argument('-P', '--provides', action='store_true', help='Print Provides')
group.add_argument('-R', '--requires', action='store_true', help='Print Requires')
group.add_argument('-r', '--recommends', action='store_true', help='Print Recommends')
group.add_argument('-C', '--conflicts', action='store_true', help='Print Conflicts')
group.add_argument('-E', '--extras', action='store_true', help='Print Extras')
parser.add_argument('-M', '--majorver-provides', action='store_true', help='Print extra Provides with Python major version only')
parser.add_argument('-m', '--majorver-only', action='store_true', help='Print Provides/Requires with Python major version only')
parser.add_argument('-L', '--legacy-provides', action='store_true', help='Print extra legacy pythonegg Provides')
parser.add_argument('-l', '--legacy', action='store_true', help='Print legacy pythonegg Provides/Requires instead')
parser.add_argument('files', nargs=argparse.REMAINDER)
args = parser.parse_args()

py_abi = args.requires
py_deps = {}


for f in (args.files or stdin.readlines()):
    f = f.strip()
    lower = f.lower()
    name = 'python(abi)'
    # add dependency based on path, versioned if within versioned python directory
    if py_abi and (lower.endswith('.py') or lower.endswith('.pyc') or lower.endswith('.pyo')):
        if name not in py_deps:
            py_deps[name] = []
        purelib = get_python_lib(standard_lib=0, plat_specific=0).split(version[:3])[0]
        platlib = get_python_lib(standard_lib=0, plat_specific=1).split(version[:3])[0]
        for lib in (purelib, platlib):
            if lib in f:
                spec = ('==', f.split(lib)[1].split(sep)[0])
                if spec not in py_deps[name]:
                    py_deps[name].append(spec)

    # XXX: hack to workaround RPM internal dependency generator not passing directories
    lower_dir = dirname(lower)
    if lower_dir.endswith('.egg') or \
            lower_dir.endswith('.egg-info') or \
            lower_dir.endswith('.dist-info'):
        lower = lower_dir
        f = dirname(f)
    # Determine provide, requires, conflicts & recommends based on egg/dist metadata
    if lower.endswith('.egg') or \
            lower.endswith('.egg-info') or \
            lower.endswith('.dist-info'):
        # This import is very slow, so only do it if needed
        from pkg_resources import Distribution, FileMetadata, PathMetadata, Requirement, parse_version
        dist_name = basename(f)
        if isdir(f):
            path_item = dirname(f)
            metadata = PathMetadata(path_item, f)
        else:
            path_item = f
            metadata = FileMetadata(f)
        dist = Distribution.from_location(path_item, dist_name, metadata)
        # Check if py_version is defined in the metadata file/directory name
        if not dist.py_version:
            # Try to parse the Python version from the path the metadata
            # resides at (e.g. /usr/lib/pythonX.Y/site-packages/...)
            import re
            res = re.search(r"/python(?P<pyver>\d+\.\d+)/", path_item)
            if res:
                dist.py_version = res.group('pyver')
            else:
                warn("Version for {!r} has not been found".format(dist), RuntimeWarning)
                continue
        if args.majorver_provides or args.majorver_only or args.legacy_provides or args.legacy:
            # Get the Python major version
            pyver_major = dist.py_version.split('.')[0]
        if args.provides:
            # If egg/dist metadata says package name is python, we provide python(abi)
            if dist.key == 'python':
                name = 'python(abi)'
                if name not in py_deps:
                    py_deps[name] = []
                py_deps[name].append(('==', dist.py_version))
            if not args.legacy or not args.majorver_only:
                name = 'python{}dist({})'.format(dist.py_version, dist.key)
                if name not in py_deps:
                    py_deps[name] = []
            if args.majorver_provides or args.majorver_only:
                pymajor_name = 'python{}dist({})'.format(pyver_major, dist.key)
                if pymajor_name not in py_deps:
                    py_deps[pymajor_name] = []
            if args.legacy or args.legacy_provides:
                legacy_name = 'pythonegg({})({})'.format(pyver_major, dist.key)
                if legacy_name not in py_deps:
                    py_deps[legacy_name] = []
            if dist.version:
                version = dist.version
                spec = ('==', version)
                if spec not in py_deps[name]:
                    if not args.legacy:
                        py_deps[name].append(spec)
                    if args.majorver_provides:
                        py_deps[pymajor_name].append(spec)
                    if args.legacy or args.legacy_provides:
                        py_deps[legacy_name].append(spec)
        if args.requires or (args.recommends and dist.extras):
            name = 'python(abi)'
            # If egg/dist metadata says package name is python, we don't add dependency on python(abi)
            if dist.key == 'python':
                py_abi = False
                if name in py_deps:
                    py_deps.pop(name)
            elif py_abi and dist.py_version:
                if name not in py_deps:
                    py_deps[name] = []
                spec = ('==', dist.py_version)
                if spec not in py_deps[name]:
                    py_deps[name].append(spec)
            deps = dist.requires()
            if args.recommends:
                depsextras = dist.requires(extras=dist.extras)
                if not args.requires:
                    for dep in reversed(depsextras):
                        if dep in deps:
                            depsextras.remove(dep)
                deps = depsextras
            # console_scripts/gui_scripts entry points need pkg_resources from setuptools
            if ((dist.get_entry_map('console_scripts') or
                dist.get_entry_map('gui_scripts')) and
                (lower.endswith('.egg') or
                 lower.endswith('.egg-info'))):
                # stick them first so any more specific requirement overrides it
                deps.insert(0, Requirement.parse('setuptools'))
            # add requires/recommends based on egg/dist metadata
            for dep in deps:
                if args.legacy:
                    name = 'pythonegg({})({})'.format(pyver_major, dep.key)
                else:
                    if args.majorver_only:
                        name = 'python{}dist({})'.format(pyver_major, dep.key)
                    else:
                        name = 'python{}dist({})'.format(dist.py_version, dep.key)
                for spec in dep.specs:
                    if name not in py_deps:
                        py_deps[name] = []
                    if spec not in py_deps[name]:
                        py_deps[name].append(spec)
                if not dep.specs:
                    py_deps[name] = []
        # Unused, for automatic sub-package generation based on 'extras' from egg/dist metadata
        # TODO: implement in rpm later, or...?
        if args.extras:
            deps = dist.requires()
            extras = dist.extras
            print(extras)
            for extra in extras:
                print('%%package\textras-{}'.format(extra))
                print('Summary:\t{} extra for {} python package'.format(extra, dist.key))
                print('Group:\t\tDevelopment/Python')
                depsextras = dist.requires(extras=[extra])
                for dep in reversed(depsextras):
                    if dep in deps:
                        depsextras.remove(dep)
                deps = depsextras
                for dep in deps:
                    for spec in dep.specs:
                        if spec[0] == '!=':
                            print('Conflicts:\t{} {} {}'.format(dep.key, '==', spec[1]))
                        else:
                            print('Requires:\t{} {} {}'.format(dep.key, spec[0], spec[1]))
                print('%%description\t{}'.format(extra))
                print('{} extra for {} python package'.format(extra, dist.key))
                print('%%files\t\textras-{}\n'.format(extra))
        if args.conflicts:
            # Should we really add conflicts for extras?
            # Creating a meta package per extra with recommends on, which has
            # the requires/conflicts in stead might be a better solution...
            for dep in dist.requires(extras=dist.extras):
                name = dep.key
                for spec in dep.specs:
                    if spec[0] == '!=':
                        if name not in py_deps:
                            py_deps[name] = []
                        spec = ('==', spec[1])
                        if spec not in py_deps[name]:
                            py_deps[name].append(spec)
names = list(py_deps.keys())
names.sort()
for name in names:
    if py_deps[name]:
        # Print out versioned provides, requires, recommends, conflicts
        spec_list = []
        for spec in py_deps[name]:
            spec_list.append(convert(name, spec[0], spec[1]))
        if len(spec_list) == 1:
            print(spec_list[0])
        else:
            print('({})'.format(' with '.join(spec_list)))
    else:
        # Print out unversioned provides, requires, recommends, conflicts
        print(name)
