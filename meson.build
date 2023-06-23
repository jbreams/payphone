project('payphone', 'cpp', default_options: ['cpp_std=c++17'])

libphonenumber_dep = dependency('libphonenumber', modules: ['libphonenumber::phonenumber-shared'])
pjsip_dep = dependency('libpjproject', static: true)
yamlcpp_dep = dependency('yaml-cpp')
sources = [ 'main.cpp', 'cin_dialer.cpp', 'yaml_persisted_obj.cpp' ]
executable('payphone', sources, dependencies: [ pjsip_dep, libphonenumber_dep, yamlcpp_dep] )

