#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint dart_pty.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'dart_pty'
  s.version          = '0.0.1'
  s.summary          = 'Production-grade PTY for Flutter desktop via FFI.'
  s.description      = <<-DESC
Native pseudo-terminal implementation using posix_spawn on macOS.
                       DESC
  s.homepage         = 'https://github.com/cdrury526/dart_pty'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Chris Drury' => 'cdrury526@gmail.com' }

  # Classes/ contains a forwarder C file that #includes ../../src/dart_pty.c
  # (the unity build file), which in turn includes dart_api_dl.c and the
  # platform-specific implementation.
  s.source           = { :path => '.' }
  s.source_files     = 'Classes/**/*'

  s.dependency 'FlutterMacOS'

  s.platform = :osx, '10.14'
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    # Include path for dart_api_dl.h and other Dart SDK headers.
    'HEADER_SEARCH_PATHS' => '"$(PODS_TARGET_SRCROOT)/../src/include"',
    # C11 standard with strict warnings.
    'GCC_C_LANGUAGE_STANDARD' => 'c11',
    'WARNING_CFLAGS' => '-Wall -Wextra',
  }
end
