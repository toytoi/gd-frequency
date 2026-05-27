project(
  'gd-frequency',
  'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++23',
    'buildtype=release',
    'optimization=3',
    'b_ndebug=true',
    'b_lto=true',
    'strip=true',
    'warning_level=3',
  ],
)

cpp = meson.get_compiler('cpp')
libdeflate_dep = dependency('libdeflate')
simdjson_dep = dependency('simdjson')
threads_dep = dependency('threads')

cpp_args = [
  '-DGDF_DEFAULT_BLOCK_SIZE=' + get_option('block_size').to_string(),
]

if get_option('native_optimizations')
  foreach arg : ['-march=native', '-mtune=native']
    if cpp.has_argument(arg)
      cpp_args += arg
    endif
  endforeach
endif

foreach arg : ['-fno-rtti', '-fvisibility=hidden']
  if cpp.has_argument(arg)
    cpp_args += arg
  endif
endforeach

link_args = []
foreach arg : ['-static-libstdc++', '-static-libgcc', '-Wl,--gc-sections']
  if cpp.has_link_argument(arg)
    link_args += arg
  endif
endforeach

executable(
  'gd-frequency',
  'src/lookup.cpp',
  cpp_args: cpp_args,
  link_args: link_args,
  install: true,
)

executable(
  'gd-frequency-build-cache',
  'src/builder.cpp',
  cpp_args: cpp_args,
  dependencies: [libdeflate_dep, simdjson_dep, threads_dep],
  install: true,
)

install_data(
  [
    'data/JPDB_v2.2_Frequency_2024-10-13.zip',
    'data/H_Freq.zip',
    'data/vn_freq.zip',
  ],
  install_dir: get_option('datadir') / 'gd-frequency',
)

meson.add_install_script(
  'scripts/build-cache.sh',
  get_option('bindir'),
  get_option('datadir'),
)
