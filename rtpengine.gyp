{
  'variables': {
    'cflags_common': [
        '-std=gnu99',
        '-pthread',
        '-fstack-protector-all',
        '-fno-strict-aliasing',
        
        '-Wall',
        '-Werror',
        '-Wimplicit-function-declaration',
        '-Wno-format',
        '-Wno-unused-variable',
        '-Wno-deprecated-declarations',

        # Make local installed libs be first on include path
        '-Ivendor/usr/local/include',
        
        '<!@(pkg-config --cflags glib-2.0)',
        '<!@(pkg-config --cflags gthread-2.0)',
        '<!@(pkg-config --cflags zlib)',
        '<!@(pcre-config --cflags)',
        '<!@(xmlrpc-c-config --cflags)',
        # pkg-config --cflags from homebrew hiredis are incorrect
        '-I/usr/local/opt/hiredis/include',
    ],
    'macosx_deployment_target': '10.10',
  },
  'configurations': {
    'Debug': {
        'xcode_settings': {
            'ONLY_ACTIVE_ARCH': 'YES',
            'SDKROOT': 'macosx',
            'MACOSX_DEPLOYMENT_TARGET': '<(macosx_deployment_target)',
         },
    },
    'Release': {
        'xcode_settings': {
            'SDKROOT': 'macosx',
            'MACOSX_DEPLOYMENT_TARGET': '<(macosx_deployment_target)',
         },
    },
  }, # configurations
  'target_defaults': {
    'configurations': {
         'Release': {
             'defines': [
                 'NDEBUG',
             ],
             'cflags': [
                 '<@(cflags_common)',
                 '-O3',
             ],
             'xcode_settings': {
                 'GCC_OPTIMIZATION_LEVEL': '3',
             },
         },
         'Debug': {
             'defines': [
                 '__DEBUG=1',
             ],
             'cflags': [
                '<@(cflags_common)',
                '-O0',
                '-g',
             ],
             'xcode_settings': {
                 'GCC_OPTIMIZATION_LEVEL': '0',
             },
         },
     },
     'default_configuration': 'Debug',
     'xcode_settings': {
         'OTHER_CFLAGS': ['<@(cflags_common)'],
         'ALWAYS_SEARCH_USER_PATHS': 'NO',
         'DEBUG_INFORMATION_FORMAT': 'dwarf-with-dsym',
         'CODE_SIGN_IDENTITY': 'Developer ID Application',
     },
  },
  'targets': [
    {
      'target_name': 'rtpengine',
      'type': 'executable',
      'include_dirs': [
          'daemon',
          'kernel-module',
      ],
      'defines': [
          'RTPENGINE_VERSION=\"dev\"',
      ],
      'xcode_settings': {
          'OTHER_LDFLAGS': [
              '-lpthread',
              '-Lvendor/usr/local/lib/',
              '-lcrypto',
              '-lssl',
              '<!@(pkg-config --libs glib-2.0)',
              '<!@(pkg-config --libs gthread-2.0)',
              '<!@(pkg-config --libs zlib)',
              '<!@(pkg-config --libs libpcre)',
              '<!@(pcre-config --libs)',
              '<!@(xmlrpc-c-config client --libs)',
              '<!@(pkg-config --libs hiredis)',
          ],
      },
      'sources': [
          '<!@(ls -1 daemon/*.h)',
          '<!@(ls -1 daemon/*.c)',
      ],
      'sources!': [
          'daemon/poller.c',
          'daemon/graphite.c',
      ],
    },
  ],
}
