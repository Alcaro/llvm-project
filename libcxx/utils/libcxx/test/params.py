#===----------------------------------------------------------------------===##
#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
#===----------------------------------------------------------------------===##

from libcxx.test.dsl import *
from libcxx.test.features import _isMSVC

_warningFlags = [
  '-Werror',
  '-Wall',
  '-Wextra',
  '-Wshadow',
  '-Wundef',
  '-Wno-unused-command-line-argument',
  '-Wno-attributes',
  '-Wno-pessimizing-move',
  '-Wno-c++11-extensions',
  '-Wno-user-defined-literals',
  '-Wno-noexcept-type',
  '-Wno-aligned-allocation-unavailable',
  '-Wno-atomic-alignment',

  # GCC warns about places where we might want to add sized allocation/deallocation
  # functions, but we know better what we're doing/testing in the test suite.
  '-Wno-sized-deallocation',

  # These warnings should be enabled in order to support the MSVC
  # team using the test suite; They enable the warnings below and
  # expect the test suite to be clean.
  '-Wsign-compare',
  '-Wunused-variable',
  '-Wunused-parameter',
  '-Wunreachable-code',
  '-Wno-unused-local-typedef',
]

_allStandards = ['c++03', 'c++11', 'c++14', 'c++17', 'c++20', 'c++2b']
def getStdFlag(cfg, std):
  fallbacks = {
    'c++11': 'c++0x',
    'c++14': 'c++1y',
    'c++17': 'c++1z',
    'c++20': 'c++2a',
  }
  if hasCompileFlag(cfg, '-std='+std):
    return '-std='+std
  if std in fallbacks and hasCompileFlag(cfg, '-std='+fallbacks[std]):
    return '-std='+fallbacks[std]
  return None

DEFAULT_PARAMETERS = [
  # Core parameters of the test suite
  Parameter(name='target_triple', type=str, default=getHostTriple,
            help="The target triple to compile the test suite for. This must be "
                 "compatible with the target that the tests will be run on.",
            actions=lambda triple: filter(None, [
              AddFeature(triple),
              AddFlagIfSupported('--target={}'.format(triple)),
              AddFeature('linux-gnu') if re.match(r'^.*-linux-gnu', triple) else None,
              AddFeature('x86_64-linux') if re.match(r'^x86_64.*-linux', triple) else None,
              AddFeature('x86_64-apple') if re.match(r'^x86_64.*-apple', triple) else None,
              AddFeature('target-x86') if re.match(r'^i.86.*', triple) else None,
              AddFeature('target-x86_64') if re.match(r'^x86_64.*', triple) else None,
              AddFeature('target-aarch64') if re.match(r'^aarch64.*', triple) else None,
              AddFeature('target-arm') if re.match(r'^arm.*', triple) else None,
            ])),

  Parameter(name='std', choices=_allStandards, type=str,
            help="The version of the standard to compile the test suite with.",
            default=lambda cfg: next(s for s in reversed(_allStandards) if getStdFlag(cfg, s)),
            actions=lambda std: [
              AddFeature(std),
              AddCompileFlag(lambda cfg: getStdFlag(cfg, std)),
            ]),

  Parameter(name='enable_exceptions', choices=[True, False], type=bool, default=True,
            help="Whether to enable exceptions when compiling the test suite.",
            actions=lambda exceptions: [] if exceptions else [
              AddFeature('no-exceptions'),
              AddCompileFlag('-fno-exceptions')
            ]),

  Parameter(name='enable_rtti', choices=[True, False], type=bool, default=True,
            help="Whether to enable RTTI when compiling the test suite.",
            actions=lambda rtti: [] if rtti else [
              AddFeature('no-rtti'),
              AddCompileFlag('-fno-rtti')
            ]),

  Parameter(name='stdlib', choices=['libc++', 'libstdc++', 'msvc'], type=str, default='libc++',
            help="The C++ Standard Library implementation being tested.",
            actions=lambda stdlib: [
              AddFeature(stdlib)
            ]),

  Parameter(name='enable_warnings', choices=[True, False], type=bool, default=True,
            help="Whether to enable warnings when compiling the test suite.",
            actions=lambda warnings: [] if not warnings else [
              AddOptionalWarningFlag(w) for w in _warningFlags
            ]),

  Parameter(name='debug_level', choices=['', '0', '1'], type=str, default='',
            help="The debugging level to enable in the test suite.",
            actions=lambda debugLevel: [] if debugLevel == '' else [
              AddFeature('debug_level={}'.format(debugLevel)),
              AddCompileFlag('-D_LIBCPP_DEBUG={}'.format(debugLevel))
            ]),

  Parameter(name='use_sanitizer', choices=['', 'Address', 'Undefined', 'Memory', 'MemoryWithOrigins', 'Thread', 'DataFlow', 'Leaks'], type=str, default='',
            help="An optional sanitizer to enable when building and running the test suite.",
            actions=lambda sanitizer: filter(None, [
              AddFlag('-g -fno-omit-frame-pointer') if sanitizer else None,

              AddFlag('-fsanitize=undefined -fno-sanitize=float-divide-by-zero -fno-sanitize-recover=all') if sanitizer == 'Undefined' else None,
              AddFeature('ubsan')                                                                          if sanitizer == 'Undefined' else None,

              AddFlag('-fsanitize=address') if sanitizer == 'Address' else None,
              AddFeature('asan')            if sanitizer == 'Address' else None,

              AddFlag('-fsanitize=memory')               if sanitizer in ['Memory', 'MemoryWithOrigins'] else None,
              AddFeature('msan')                         if sanitizer in ['Memory', 'MemoryWithOrigins'] else None,
              AddFlag('-fsanitize-memory-track-origins') if sanitizer == 'MemoryWithOrigins' else None,

              AddFlag('-fsanitize=thread') if sanitizer == 'Thread' else None,
              AddFeature('tsan')           if sanitizer == 'Thread' else None,

              AddFlag('-fsanitize=dataflow') if sanitizer == 'DataFlow' else None,
              AddFlag('-fsanitize=leaks') if sanitizer == 'Leaks' else None,

              AddFeature('sanitizer-new-delete') if sanitizer in ['Address', 'Memory', 'MemoryWithOrigins', 'Thread'] else None,
            ])),

  # Parameters to enable or disable parts of the test suite
  Parameter(name='enable_experimental', choices=[True, False], type=bool, default=True,
            help="Whether to enable tests for experimental C++ libraries (typically Library Fundamentals TSes).",
            actions=lambda experimental: [] if not experimental else [
              AddFeature('c++experimental'),
              # When linking in MSVC mode via the Clang driver, a -l<foo>
              # maps to <foo>.lib, so we need to use -llibc++experimental here
              # to make it link against the static libc++experimental.lib.
              # We can't check for the feature 'msvc' in available_features
              # as those features are added after processing parameters.
              PrependLinkFlag(lambda config: '-llibc++experimental' if _isMSVC(config) else '-lc++experimental')
            ]),

  Parameter(name='long_tests', choices=[True, False], type=bool, default=True,
            help="Whether to enable tests that take longer to run. This can be useful when running on a very slow device.",
            actions=lambda enabled: [] if not enabled else [
              AddFeature('long_tests')
            ]),

  Parameter(name='enable_debug_tests', choices=[True, False], type=bool, default=True,
            help="Whether to enable tests that exercise the libc++ debugging mode.",
            actions=lambda enabled: [] if enabled else [
              AddFeature('libcxx-no-debug-mode')
            ]),
]

DEFAULT_PARAMETERS += [
  Parameter(name='use_system_cxx_lib', choices=[True, False], type=bool, default=False,
            help="""
    Whether the test suite is being *run* against the library shipped on the
    target triple in use, as opposed to the trunk library.

    When vendor-specific availability annotations are enabled, we add the
    'use_system_cxx_lib' Lit feature to allow writing XFAIL or UNSUPPORTED
    markup for tests that are known to fail on a particular triple.

    That feature can be used to XFAIL a test that fails when deployed on (or is
    compiled for) an older system. For example, if the test exhibits a bug in the
    libc on a particular system version, or if the test uses a symbol that is not
    available on an older version of the dylib, it can be marked as XFAIL with
    the above feature.

    It is sometimes useful to check that a test fails specifically when compiled
    for a given deployment target. For example, this is the case when testing
    availability markup, where we want to make sure that using the annotated
    facility on a deployment target that doesn't support it will fail at compile
    time, not at runtime. This can be achieved by creating a `.compile.pass.cpp`
    and XFAILing it for the right deployment target. If the test doesn't fail at
    compile-time like it's supposed to, the test will XPASS. Another option is to
    create a `.verify.cpp` test that checks for the right errors, and mark that
    test as requiring `use_system_cxx_lib && <target>`.
    """,
    actions=lambda useSystem: [
      AddFeature('use_system_cxx_lib')
    ] if useSystem else [
      # If we're testing upstream libc++, disable availability markup,
      # which is not relevant for non-shipped flavors of libc++.
      AddCompileFlag('-D_LIBCPP_DISABLE_AVAILABILITY')
    ])
]
