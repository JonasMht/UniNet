# CMake generated Testfile for 
# Source directory: /home/jonas/Documents/Research/UniNet
# Build directory: /home/jonas/Documents/Research/UniNet/build3
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(roundtrip "/home/jonas/Documents/Research/UniNet/build3/test_roundtrip")
set_tests_properties(roundtrip PROPERTIES  LABELS "uninet" _BACKTRACE_TRIPLES "/home/jonas/Documents/Research/UniNet/CMakeLists.txt;250;add_test;/home/jonas/Documents/Research/UniNet/CMakeLists.txt;0;")
add_test(network "/home/jonas/Documents/Research/UniNet/build3/test_network")
set_tests_properties(network PROPERTIES  LABELS "uninet" TIMEOUT "90" _BACKTRACE_TRIPLES "/home/jonas/Documents/Research/UniNet/CMakeLists.txt;258;add_test;/home/jonas/Documents/Research/UniNet/CMakeLists.txt;0;")
add_test(cabi "/home/jonas/Documents/Research/UniNet/build3/test_cabi")
set_tests_properties(cabi PROPERTIES  LABELS "uninet" TIMEOUT "90" _BACKTRACE_TRIPLES "/home/jonas/Documents/Research/UniNet/CMakeLists.txt;307;add_test;/home/jonas/Documents/Research/UniNet/CMakeLists.txt;0;")
