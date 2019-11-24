project('tllist', 'c', version: '1.0.0', license: 'MIT', meson_version: '>=0.50.0')
tllist = declare_dependency(include_directories: '.')

unittest = executable('unittest', 'test.c', dependencies: [tllist])
test('unittest', unittest)
