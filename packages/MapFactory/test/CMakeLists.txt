##---------------------------------------------------------------------------##
## TESTS
##---------------------------------------------------------------------------##
# Disable the test for the moment. We need to replace the parsing library.
#TRIBITS_ADD_EXECUTABLE_AND_TEST(
#  MapInterface_test
#  SOURCES tstMapInterface.cpp unit_test_main.cpp
#  COMM serial mpi
#  STANDARD_PASS_OUTPUT
#  FAIL_REGULAR_EXPRESSION "data race;leak;runtime error"
#  )
