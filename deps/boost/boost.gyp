{
  'targets': [
    {
      'target_name': 'boost_context',
      'type': 'static_library',
      'include_dirs': [
        '.',  # deps/boost/
      ],
      'sources': [
        'src/asm/jump_x86_64_sysv_elf_gas.S',
        'src/asm/make_x86_64_sysv_elf_gas.S',
        'src/asm/ontop_x86_64_sysv_elf_gas.S',
      ],
      'conditions': [
        ['OS=="linux"', {
          'cflags': ['-x', 'assembler-with-cpp'],
        }],
      ],
    },
  ],
}
