project(mytest)

#add_executable(mytest server.c)
#target_link_libraries(mytest uv)
#
#add_executable(mytest_worker worker.c)
#target_link_libraries(mytest_worker uv)

add_executable(mytest test.c)
target_link_libraries(mytest uv)

